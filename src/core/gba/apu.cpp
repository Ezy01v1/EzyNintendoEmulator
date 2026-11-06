#include "gba/apu.hpp"

#include <algorithm>

namespace ezy::gba {

namespace {

// I/O offsets from 0x04000000.
constexpr std::uint32_t kSound1CntL = 0x060;  // sweep
constexpr std::uint32_t kSoundCntL = 0x080;   // PSG master volume and L/R enables
constexpr std::uint32_t kSoundCntH = 0x082;   // mixing ratios, direct sound control
constexpr std::uint32_t kSoundCntX = 0x084;   // master enable, channel status
constexpr std::uint32_t kSoundBias = 0x088;
constexpr std::uint32_t kSound3CntL = 0x070;  // wave: banks, DAC
constexpr std::uint32_t kSound3CntH = 0x072;  // wave: length, volume
constexpr std::uint32_t kSound4CntH = 0x07C;  // noise: divider, width, shift
constexpr std::uint32_t kWaveRam = 0x090;
constexpr std::uint32_t kWaveRamEnd = 0x0A0;
constexpr std::uint32_t kFifoA = 0x0A0;
constexpr std::uint32_t kFifoEnd = 0x0A8;
constexpr std::uint32_t kPsgFirst = 0x060;  // reset and locked while the master enable is off
constexpr std::uint32_t kPsgLast = 0x081;

// Per PSG channel: the length/duty/envelope register and the frequency/control register.
constexpr std::array<std::uint32_t, 4> kLengthReg = {0x062, 0x068, 0x072, 0x078};
constexpr std::array<std::uint32_t, 4> kControlReg = {0x064, 0x06C, 0x074, 0x07C};
constexpr std::array<int, 4> kMaxLength = {64, 64, 256, 64};

constexpr std::uint8_t kLengthEnable = 0x40;  // control register, high byte
constexpr std::uint8_t kTrigger = 0x80;

constexpr std::array<std::uint8_t, 4> kDutyPatterns = {0b00000001, 0b10000001, 0b10000111, 0b01111110};

constexpr std::uint32_t kFrameSequencerCycles = 32768;  // 512 Hz

}  // namespace

// --- Channel building blocks ---

void Apu::Envelope::trigger(std::uint8_t reg) {
    volume = reg >> 4;
    timer = reg & 7;
}

void Apu::Envelope::clock(std::uint8_t reg) {
    const int period = reg & 7;
    if (period == 0 || --timer > 0) return;
    timer = period;
    if ((reg & 0x08) != 0) {
        if (volume < 15) ++volume;
    } else if (volume > 0) {
        --volume;
    }
}

void Apu::Length::clock(bool enabled, bool& on) {
    if (enabled && counter > 0 && --counter == 0) on = false;
}

void Apu::Fifo::push(std::uint8_t byte) {
    if (count == static_cast<int>(data.size())) return;  // full: the byte is lost
    data[static_cast<std::size_t>((read + count) % 32)] = static_cast<std::int8_t>(byte);
    ++count;
}

// --- Registers ---

Apu::Apu(std::span<std::uint8_t> io, Scheduler* scheduler) : io_(io), scheduler_(scheduler) {}

std::uint16_t Apu::io16(std::uint32_t offset) const {
    return static_cast<std::uint16_t>(io_[offset] | (io_[offset + 1] << 8));
}

void Apu::set_io16(std::uint32_t offset, std::uint16_t value) {
    io_[offset] = static_cast<std::uint8_t>(value);
    io_[offset + 1] = static_cast<std::uint8_t>(value >> 8);
}

bool Apu::channel_on(int ch) const {
    switch (ch) {
        case 0:
        case 1: return squares_[static_cast<std::size_t>(ch)].on;
        case 2: return wave_.on;
        case 3: return noise_.on;
        default: return false;
    }
}

int Apu::square_period(int ch) const {
    const int freq = io16(kControlReg[static_cast<std::size_t>(ch)]) & 0x7FF;
    return (2048 - freq) * 16;  // 8 duty steps per period at 131072 / (2048 - freq) Hz
}

int Apu::wave_period() const { return (2048 - (io16(kControlReg[2]) & 0x7FF)) * 8; }

int Apu::noise_period() const {
    const std::uint8_t reg = io8(kSound4CntH);
    const int r = reg & 7;
    const int divisor = r == 0 ? 8 : 16 * r;
    return (divisor << (reg >> 4)) * 4;  // Game Boy T-cycles are 4 GBA cycles
}

bool Apu::dac_on(int ch) const {
    if (ch == 2) return (io8(kSound3CntL) & 0x80) != 0;
    // Envelope initial volume or direction set (the high byte of the length register).
    return (io8(kLengthReg[static_cast<std::size_t>(ch)] + 1) & 0xF8) != 0;
}

void Apu::start() {
    if (scheduler_ != nullptr) scheduler_->schedule_in(EventType::ApuSample, kCyclesPerSample);
}

void Apu::after_io_write(std::size_t offset, std::size_t size) {
    const auto touches = [&](std::uint32_t at) { return at >= offset && at < offset + size; };

    if (touches(kSoundCntX)) {
        const bool on = (io8(kSoundCntX) & 0x80) != 0;
        if (on && !master_) frame_step_ = 0;
        if (!on && master_) power_off();
        master_ = on;
    }

    if (!master_) {
        // Powered off: the PSG registers stay cleared.
        std::fill(io_.begin() + kPsgFirst, io_.begin() + kPsgLast + 1, std::uint8_t{0});
    } else {
        for (int ch = 0; ch < 4; ++ch) {
            const auto c = static_cast<std::size_t>(ch);
            const std::uint32_t len_reg = kLengthReg[c];
            if (touches(len_reg)) {
                const int mask = ch == 2 ? 0xFF : 0x3F;
                const int counter = kMaxLength[c] - (io8(len_reg) & mask);
                switch (ch) {
                    case 0:
                    case 1: squares_[c].len.counter = counter; break;
                    case 2: wave_.len.counter = counter; break;
                    default: noise_.len.counter = counter; break;
                }
            }
            if (!dac_on(ch)) {
                switch (ch) {
                    case 0:
                    case 1: squares_[c].on = false; break;
                    case 2: wave_.on = false; break;
                    default: noise_.on = false; break;
                }
            }
            const std::uint32_t control_high = kControlReg[c] + 1;
            if (touches(control_high) && (io8(control_high) & kTrigger) != 0) {
                trigger(ch);
                io_[control_high] = static_cast<std::uint8_t>(io_[control_high] & ~kTrigger);  // write-only
            }
        }
    }

    // Wave RAM: the CPU reads and writes the bank that isn't playing.
    if (touches(kSound3CntL)) sync_wave_ram_to_io();
    const std::size_t cpu_bank = ((io8(kSound3CntL) >> 6) & 1U) ^ 1U;
    for (std::uint32_t at = kWaveRam; at < kWaveRamEnd; ++at) {
        if (touches(at)) wave_ram_[cpu_bank][at - kWaveRam] = io8(at);
    }

    // Direct sound: FIFO resets (write-only bits) and FIFO data.
    if (touches(kSoundCntH + 1)) {
        std::uint8_t high = io8(kSoundCntH + 1);
        if ((high & 0x08) != 0) fifos_[0].reset();
        if ((high & 0x80) != 0) fifos_[1].reset();
        high = static_cast<std::uint8_t>(high & ~0x88U);
        io_[kSoundCntH + 1] = high;
    }
    for (std::uint32_t at = kFifoA; at < kFifoEnd; ++at) {
        if (touches(at)) fifos_[at < kFifoA + 4 ? 0 : 1].push(io8(at));
    }

    update_status();
}

void Apu::sync_wave_ram_to_io() {
    const std::size_t cpu_bank = ((io8(kSound3CntL) >> 6) & 1U) ^ 1U;
    std::copy(wave_ram_[cpu_bank].begin(), wave_ram_[cpu_bank].end(), io_.begin() + kWaveRam);
}

void Apu::power_off() {
    std::fill(io_.begin() + kPsgFirst, io_.begin() + kPsgLast + 1, std::uint8_t{0});
    squares_ = {};
    sweep_ = {};
    wave_ = {};
    noise_ = {};
    frame_step_ = 0;
}

void Apu::update_status() {
    std::uint8_t status = io8(kSoundCntX) & 0x80;
    for (int ch = 0; ch < 4; ++ch) {
        if (channel_on(ch)) status = static_cast<std::uint8_t>(status | (1U << ch));
    }
    io_[kSoundCntX] = status;
}

void Apu::trigger(int ch) {
    const auto c = static_cast<std::size_t>(ch);
    const bool dac = dac_on(ch);
    switch (ch) {
        case 0:
        case 1: {
            Square& s = squares_[c];
            s.on = dac;
            if (s.len.counter == 0) s.len.counter = kMaxLength[c];
            s.timer = square_period(ch);
            s.env.trigger(io8(kLengthReg[c] + 1));
            if (ch == 0) {
                const std::uint8_t reg = io8(kSound1CntL);
                const int period = (reg >> 4) & 7;
                const int shift = reg & 7;
                sweep_.shadow = io16(kControlReg[0]) & 0x7FF;
                sweep_.timer = period != 0 ? period : 8;
                sweep_.enabled = period != 0 || shift != 0;
                if (shift != 0) (void)sweep_calc();  // an overflowing sweep stops it at once
            }
            break;
        }
        case 2:
            wave_.on = dac;
            if (wave_.len.counter == 0) wave_.len.counter = kMaxLength[2];
            wave_.timer = wave_period();
            wave_.pos = 0;
            break;
        default:
            noise_.on = dac;
            if (noise_.len.counter == 0) noise_.len.counter = kMaxLength[3];
            noise_.timer = noise_period();
            noise_.lfsr = 0x7FFF;
            noise_.env.trigger(io8(kLengthReg[3] + 1));
            break;
    }
}

int Apu::sweep_calc() {
    const std::uint8_t reg = io8(kSound1CntL);
    const int delta = sweep_.shadow >> (reg & 7);
    const int next = (reg & 0x08) != 0 ? sweep_.shadow - delta : sweep_.shadow + delta;
    if (next > 2047) squares_[0].on = false;
    return next;
}

// --- Frame sequencer: length 256 Hz, sweep 128 Hz, envelope 64 Hz ---

void Apu::clock_frame_sequencer() {
    const int step = frame_step_;
    frame_step_ = (frame_step_ + 1) & 7;
    const auto length_enabled = [this](int ch) {
        return (io8(kControlReg[static_cast<std::size_t>(ch)] + 1) & kLengthEnable) != 0;
    };
    if (step % 2 == 0) {
        squares_[0].len.clock(length_enabled(0), squares_[0].on);
        squares_[1].len.clock(length_enabled(1), squares_[1].on);
        wave_.len.clock(length_enabled(2), wave_.on);
        noise_.len.clock(length_enabled(3), noise_.on);
    }
    if (step == 2 || step == 6) {
        const std::uint8_t reg = io8(kSound1CntL);
        const int period = (reg >> 4) & 7;
        if (--sweep_.timer <= 0) {
            sweep_.timer = period != 0 ? period : 8;
            if (sweep_.enabled && period != 0) {
                const int next = sweep_calc();
                if (next <= 2047 && (reg & 7) != 0) {
                    sweep_.shadow = next;
                    const std::uint16_t control = io16(kControlReg[0]);
                    set_io16(kControlReg[0], static_cast<std::uint16_t>((control & ~0x7FFU) | static_cast<unsigned>(next)));
                    (void)sweep_calc();
                }
            }
        }
    }
    if (step == 7) {
        squares_[0].env.clock(io8(kLengthReg[0] + 1));
        squares_[1].env.clock(io8(kLengthReg[1] + 1));
        noise_.env.clock(io8(kLengthReg[3] + 1));
    }
}

// --- Waveforms ---

float Apu::advance_square(int ch, int cycles) {
    Square& s = squares_[static_cast<std::size_t>(ch)];
    if (!s.on) return 0.0F;
    const std::uint8_t pattern = kDutyPatterns[io8(kLengthReg[static_cast<std::size_t>(ch)]) >> 6];
    const int period = square_period(ch);
    long sum = 0;
    for (int left = cycles; left > 0;) {
        if (s.timer <= 0) s.timer = period;
        const int run = std::min(s.timer, left);
        const bool high = ((pattern >> (7 - s.duty_pos)) & 1) != 0;
        sum += static_cast<long>(high ? s.env.volume : -s.env.volume) * run;
        s.timer -= run;
        left -= run;
        if (s.timer == 0) {
            s.timer = period;
            s.duty_pos = (s.duty_pos + 1) & 7;
        }
    }
    return static_cast<float>(sum) / static_cast<float>(cycles);
}

int Apu::wave_level() const {
    const std::uint8_t cnt_l = io8(kSound3CntL);
    const bool two_banks = (cnt_l & 0x20) != 0;
    const unsigned playing = (cnt_l >> 6) & 1U;
    const unsigned bank = two_banks ? playing ^ (wave_.pos >= 32 ? 1U : 0U) : playing;
    const auto index = static_cast<std::size_t>(wave_.pos % 32);
    const std::uint8_t byte = wave_ram_[bank][index / 2];
    const int nibble = (index & 1) != 0 ? byte & 0xF : byte >> 4;  // high nibble first
    const int level = nibble * 2 - 15;
    const std::uint16_t cnt_h = io16(kSound3CntH);
    if ((cnt_h & 0x8000) != 0) return level * 3 / 4;  // forced 75%
    switch ((cnt_h >> 13) & 3) {
        case 1: return level;
        case 2: return level / 2;
        case 3: return level / 4;
        default: return 0;  // mute
    }
}

float Apu::advance_wave(int cycles) {
    if (!wave_.on) return 0.0F;
    const int period = wave_period();
    const int samples = (io8(kSound3CntL) & 0x20) != 0 ? 64 : 32;
    long sum = 0;
    for (int left = cycles; left > 0;) {
        if (wave_.timer <= 0) wave_.timer = period;
        const int run = std::min(wave_.timer, left);
        sum += static_cast<long>(wave_level()) * run;
        wave_.timer -= run;
        left -= run;
        if (wave_.timer == 0) {
            wave_.timer = period;
            wave_.pos = (wave_.pos + 1) % samples;
        }
    }
    return static_cast<float>(sum) / static_cast<float>(cycles);
}

float Apu::advance_noise(int cycles) {
    if (!noise_.on) return 0.0F;
    const int period = noise_period();
    const bool narrow = (io8(kSound4CntH) & 0x08) != 0;  // 7-bit LFSR
    long sum = 0;
    for (int left = cycles; left > 0;) {
        if (noise_.timer <= 0) noise_.timer = period;
        const int run = std::min(noise_.timer, left);
        const bool high = (noise_.lfsr & 1) == 0;
        sum += static_cast<long>(high ? noise_.env.volume : -noise_.env.volume) * run;
        noise_.timer -= run;
        left -= run;
        if (noise_.timer == 0) {
            noise_.timer = period;
            const auto bit = static_cast<std::uint16_t>((noise_.lfsr ^ (noise_.lfsr >> 1)) & 1U);
            noise_.lfsr = static_cast<std::uint16_t>((noise_.lfsr >> 1) | (bit << 14));
            if (narrow) noise_.lfsr = static_cast<std::uint16_t>((noise_.lfsr & ~0x40U) | (bit << 6));
        }
    }
    return static_cast<float>(sum) / static_cast<float>(cycles);
}

// --- Direct sound and mixing ---

unsigned Apu::on_timer_overflow(int index) {
    unsigned refill = 0;
    const std::uint16_t cnt_h = io16(kSoundCntH);
    for (std::size_t f = 0; f < 2; ++f) {
        const int timer = (cnt_h >> (10 + 4 * f)) & 1;
        if (timer != index) continue;
        Fifo& fifo = fifos_[f];
        if (fifo.count > 0) {
            fifo.current = fifo.data[static_cast<std::size_t>(fifo.read)];
            fifo.read = (fifo.read + 1) % 32;
            --fifo.count;
        }
        if (fifo.count <= 16) refill |= 1U << f;
    }
    return refill;
}

void Apu::on_sample(std::uint64_t when) {
    if (scheduler_ != nullptr) scheduler_->schedule(EventType::ApuSample, when + kCyclesPerSample);
    float left = 0.0F;
    float right = 0.0F;
    if (master_) {
        constexpr int kCycles = static_cast<int>(kCyclesPerSample);
        const std::array<float, 4> psg = {advance_square(0, kCycles), advance_square(1, kCycles),
                                          advance_wave(kCycles), advance_noise(kCycles)};
        sequencer_cycles_ += kCyclesPerSample;
        if (sequencer_cycles_ >= kFrameSequencerCycles) {
            sequencer_cycles_ -= kFrameSequencerCycles;
            clock_frame_sequencer();
        }

        // PSG: each channel -15..15, summed per side, times the side's master volume (1-8),
        // then the PSG ratio (25%, 50%, 100%): at most +-480.
        const std::uint16_t cnt_l = io16(kSoundCntL);
        float psg_right = 0.0F;
        float psg_left = 0.0F;
        for (std::size_t ch = 0; ch < 4; ++ch) {
            if ((cnt_l & (1U << (8 + ch))) != 0) psg_right += psg[ch];
            if ((cnt_l & (1U << (12 + ch))) != 0) psg_left += psg[ch];
        }
        psg_right *= static_cast<float>((cnt_l & 7) + 1);
        psg_left *= static_cast<float>(((cnt_l >> 4) & 7) + 1);
        const std::uint16_t cnt_h = io16(kSoundCntH);
        constexpr std::array<float, 4> kPsgRatio = {0.25F, 0.5F, 1.0F, 1.0F};  // 3 is "prohibited"
        right = psg_right * kPsgRatio[cnt_h & 3];
        left = psg_left * kPsgRatio[cnt_h & 3];

        // Direct sound: -128..127, doubled at 100% volume (bits 2/3), routed by bits 8-9 / 12-13.
        const float a = static_cast<float>(fifos_[0].current) * ((cnt_h & 0x04) != 0 ? 2.0F : 1.0F);
        const float b = static_cast<float>(fifos_[1].current) * ((cnt_h & 0x08) != 0 ? 2.0F : 1.0F);
        if ((cnt_h & 0x0100) != 0) right += a;
        if ((cnt_h & 0x0200) != 0) left += a;
        if ((cnt_h & 0x1000) != 0) right += b;
        if ((cnt_h & 0x2000) != 0) left += b;

        // The DAC adds SOUNDBIAS and clips to 10 bits; take the bias back off for output.
        const auto bias = static_cast<float>(io16(kSoundBias) & 0x3FE);
        const auto clip = [bias](float v) { return std::clamp(v + bias, 0.0F, 1023.0F) - bias; };
        left = clip(left) / 512.0F;
        right = clip(right) / 512.0F;
        update_status();
    }
    samples_.push_back(left);
    samples_.push_back(right);
}

}  // namespace ezy::gba
