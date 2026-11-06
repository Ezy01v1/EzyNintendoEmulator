#include "gb/apu.hpp"

namespace ezy::gb {

namespace {

constexpr std::uint16_t kNr10 = 0xFF10;
constexpr std::uint16_t kNr30 = 0xFF1A;
constexpr std::uint16_t kNr43 = 0xFF22;
constexpr std::uint16_t kNr50 = 0xFF24;
constexpr std::uint16_t kNr51 = 0xFF25;
constexpr std::uint16_t kNr52 = 0xFF26;
constexpr std::uint16_t kWaveRam = 0xFF30;

// First register (NRx0) of each channel; NRx1..NRx4 follow it.
constexpr std::array<std::uint16_t, 4> kBase = {0xFF10, 0xFF15, 0xFF1A, 0xFF1F};

// Bits that always read back as 1, for FF10-FF2F (Pan Docs / gbdev "Sound hardware").
constexpr std::array<std::uint8_t, 0x20> kReadMask = {
    0x80, 0x3F, 0x00, 0xFF, 0xBF,  // NR10-NR14
    0xFF, 0x3F, 0x00, 0xFF, 0xBF,  // ----, NR21-NR24
    0x7F, 0xFF, 0x9F, 0xFF, 0xBF,  // NR30-NR34
    0xFF, 0xFF, 0x00, 0x00, 0xBF,  // ----, NR41-NR44
    0x00, 0x00, 0x70,              // NR50-NR52
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,  // FF27-FF2F unused
};

constexpr std::array<std::array<std::uint8_t, 8>, 4> kDuty = {{
    {0, 0, 0, 0, 0, 0, 0, 1},  // 12.5%
    {1, 0, 0, 0, 0, 0, 0, 1},  // 25%
    {1, 0, 0, 0, 0, 1, 1, 1},  // 50%
    {0, 1, 1, 1, 1, 1, 1, 0},  // 75%
}};

constexpr std::array<int, 8> kNoiseDivisor = {8, 16, 32, 48, 64, 80, 96, 112};

constexpr std::uint32_t kCpuHz = 4'194'304;
// Per-sample decay of the output capacitor: 0.999958 per T-cycle, raised to cycles/sample.
constexpr float kHighPassCharge = 0.99634F;
constexpr std::size_t kMaxBufferedSamples = 2 * Apu::kSampleRate * 2;  // 2 s stereo

}  // namespace

Apu::Apu() {
    // State left by the DMG boot ROM (Pan Docs, "Power Up Sequence").
    struct Init {
        std::uint16_t addr;
        std::uint8_t value;
    };
    constexpr std::array<Init, 21> kPostBoot = {{
        {0xFF10, 0x80}, {0xFF11, 0xBF}, {0xFF12, 0xF3}, {0xFF13, 0xFF}, {0xFF14, 0xBF},
        {0xFF16, 0x3F}, {0xFF17, 0x00}, {0xFF18, 0xFF}, {0xFF19, 0xBF},
        {0xFF1A, 0x7F}, {0xFF1B, 0xFF}, {0xFF1C, 0x9F}, {0xFF1D, 0xFF}, {0xFF1E, 0xBF},
        {0xFF20, 0xFF}, {0xFF21, 0x00}, {0xFF22, 0x00}, {0xFF23, 0xBF},
        {0xFF24, 0x77}, {0xFF25, 0xF3}, {0xFF26, 0xF1},
    }};
    for (const auto& [addr, value] : kPostBoot) regs_[addr - 0xFF10] = value;
    squares_[0].on = true;  // the boot chime leaves CH1 enabled, at volume 0
    samples_.reserve(4096);
}

bool Apu::channel_on(int ch) const {
    switch (ch) {
        case 0: return squares_[0].on;
        case 1: return squares_[1].on;
        case 2: return wave_.on;
        default: return noise_.on;
    }
}

int Apu::square_freq(int ch) const {
    const std::uint16_t base = kBase[static_cast<std::size_t>(ch)];
    return reg(base + 3) | ((reg(base + 4) & 0x07) << 8);
}

// The DAC is on when the channel can produce any output at all.
bool Apu::dac_on(int ch) const {
    if (ch == 2) return (reg(kNr30) & 0x80) != 0;
    return (reg(kBase[static_cast<std::size_t>(ch)] + 2) & 0xF8) != 0;
}

// --- Envelope / length ---

void Apu::Envelope::trigger(std::uint8_t nrx2) {
    volume = nrx2 >> 4;
    const int period = nrx2 & 0x07;
    timer = period == 0 ? 8 : period;
}

void Apu::Envelope::clock(std::uint8_t nrx2) {
    const int period = nrx2 & 0x07;
    if (period == 0) return;
    if (--timer > 0) return;
    timer = period;
    if ((nrx2 & 0x08) != 0) {
        if (volume < 15) ++volume;
    } else if (volume > 0) {
        --volume;
    }
}

void Apu::Length::clock(bool enabled, bool& channel_on) {
    if (!enabled || counter == 0) return;
    if (--counter == 0) channel_on = false;
}

// --- Register access ---

std::uint8_t Apu::read(std::uint16_t addr) const {
    if (addr >= kWaveRam) {
        // While CH3 plays, the CGB shows the CPU the byte the channel is reading. The DMG
        // only allows it on the exact cycle CH3 reads, which we don't model: 0xFF.
        if (wave_.on) return model_ == Model::Cgb ? regs_[kWaveRam - 0xFF10 + wave_.pos / 2] : 0xFF;
        return reg(addr);
    }
    if (addr == kNr52) {
        std::uint8_t status = powered_ ? 0x80 : 0x00;
        for (int ch = 0; ch < 4; ++ch) {
            if (channel_on(ch)) status |= static_cast<std::uint8_t>(1 << ch);
        }
        return status | 0x70;
    }
    return reg(addr) | kReadMask[addr - 0xFF10];
}

void Apu::write(std::uint16_t addr, std::uint8_t value) {
    if (addr >= kWaveRam) {
        if (wave_.on) {
            if (model_ == Model::Cgb) regs_[kWaveRam - 0xFF10 + wave_.pos / 2] = value;
        } else {
            regs_[addr - 0xFF10] = value;
        }
        return;
    }
    if (addr == kNr52) {
        const bool on = (value & 0x80) != 0;
        if (powered_ && !on) power_off();
        if (!powered_ && on) {
            // Power on: the frame sequencer restarts at step 0, the squares at duty step 0.
            frame_step_ = 0;
            for (auto& sq : squares_) sq.duty_pos = 0;
            wave_.sample = 0;
        }
        powered_ = on;
        return;
    }

    // NRx1 index within the channel's 5 registers, if this is one.
    int ch = -1;
    int index = -1;
    for (int c = 0; c < 4; ++c) {
        const int offset = addr - kBase[static_cast<std::size_t>(c)];
        if (offset >= 0 && offset < 5) {
            ch = c;
            index = offset;
        }
    }

    if (!powered_) {
        // While off, everything is read-only except the length counters on the DMG.
        if (index == 1 && model_ == Model::Dmg) write_length_reg(ch, value);
        return;
    }

    if (addr == kNr10) {
        // Clearing negate after a negated sweep calculation disables CH1.
        if (sweep_.negate_used && (reg(kNr10) & 0x08) != 0 && (value & 0x08) == 0) squares_[0].on = false;
        regs_[addr - 0xFF10] = value;
        return;
    }
    if (index == 1) {
        regs_[addr - 0xFF10] = value;
        write_length_reg(ch, value);
        return;
    }
    if (index == 4) {
        write_control_reg(ch, value);
        return;
    }
    regs_[addr - 0xFF10] = value;
    if (index == 2 || (ch == 2 && index == 0)) {
        if (!dac_on(ch)) {
            if (ch < 2) squares_[static_cast<std::size_t>(ch)].on = false;
            if (ch == 2) wave_.on = false;
            if (ch == 3) noise_.on = false;
        }
    }
}

void Apu::write_length_reg(int ch, std::uint8_t value) {
    switch (ch) {
        case 0: case 1: squares_[static_cast<std::size_t>(ch)].len.counter = 64 - (value & 0x3F); break;
        case 2: wave_.len.counter = 256 - value; break;
        default: noise_.len.counter = 64 - (value & 0x3F); break;
    }
}

void Apu::write_control_reg(int ch, std::uint8_t value) {
    const std::uint16_t addr = kBase[static_cast<std::size_t>(ch)] + 4;
    const bool was_enabled = (reg(addr) & 0x40) != 0;
    const bool enabled = (value & 0x40) != 0;
    regs_[addr - 0xFF10] = value;

    Length* len = nullptr;
    bool* on = nullptr;
    switch (ch) {
        case 0: case 1: len = &squares_[static_cast<std::size_t>(ch)].len; on = &squares_[static_cast<std::size_t>(ch)].on; break;
        case 2: len = &wave_.len; on = &wave_.on; break;
        default: len = &noise_.len; on = &noise_.on; break;
    }

    // If the next frame sequencer step won't clock length, enabling length clocks it now.
    const bool length_half = (frame_step_ & 1) != 0;
    if (length_half && !was_enabled && enabled && len->counter != 0) {
        if (--len->counter == 0 && (value & 0x80) == 0) *on = false;
    }
    if ((value & 0x80) != 0) {
        if (len->counter == 0) {
            len->counter = ch == 2 ? 256 : 64;
            if (enabled && length_half) --len->counter;
        }
        trigger(ch);
    }
}

void Apu::trigger(int ch) {
    if (ch < 2) {
        Square& sq = squares_[static_cast<std::size_t>(ch)];
        sq.on = dac_on(ch);
        sq.timer = (2048 - square_freq(ch)) * 4;
        sq.env.trigger(reg(kBase[static_cast<std::size_t>(ch)] + 2));
        if (ch == 0) {
            const std::uint8_t nr10 = reg(kNr10);
            const int period = (nr10 >> 4) & 0x07;
            const int shift = nr10 & 0x07;
            sweep_.shadow = square_freq(0);
            sweep_.timer = period == 0 ? 8 : period;
            sweep_.enabled = period != 0 || shift != 0;
            sweep_.negate_used = false;
            if (shift != 0) (void)sweep_calc();
        }
    } else if (ch == 2) {
        wave_.on = dac_on(2);
        wave_.timer = (2048 - (reg(0xFF1D) | ((reg(0xFF1E) & 0x07) << 8))) * 2;
        wave_.pos = 0;
    } else {
        noise_.on = dac_on(3);
        const std::uint8_t nr43 = reg(kNr43);
        noise_.timer = kNoiseDivisor[nr43 & 0x07] << (nr43 >> 4);
        noise_.lfsr = 0x7FFF;
        noise_.env.trigger(reg(0xFF21));
    }
}

void Apu::power_off() {
    for (std::size_t i = 0; i <= kNr51 - 0xFF10; ++i) regs_[i] = 0;
    // Length counters survive power-off on the DMG; everything else is reset.
    for (auto& sq : squares_) {
        sq.on = false;
        sq.env = {};
        if (model_ == Model::Cgb) sq.len = {};
    }
    if (model_ == Model::Cgb) {
        wave_.len = {};
        noise_.len = {};
    }
    wave_.on = false;
    noise_.on = false;
    noise_.env = {};
    sweep_ = {};
}

// --- Frame sequencer ---

void Apu::div_falling_edge() {
    if (!powered_) return;
    const int step = frame_step_;
    if ((step & 1) == 0) clock_lengths();
    if (step == 2 || step == 6) clock_sweep();
    if (step == 7) clock_envelopes();
    frame_step_ = (step + 1) & 7;
}

void Apu::clock_lengths() {
    for (int ch = 0; ch < 2; ++ch) {
        Square& sq = squares_[static_cast<std::size_t>(ch)];
        sq.len.clock((reg(kBase[static_cast<std::size_t>(ch)] + 4) & 0x40) != 0, sq.on);
    }
    wave_.len.clock((reg(0xFF1E) & 0x40) != 0, wave_.on);
    noise_.len.clock((reg(0xFF23) & 0x40) != 0, noise_.on);
}

int Apu::sweep_calc() {
    const std::uint8_t nr10 = reg(kNr10);
    int delta = sweep_.shadow >> (nr10 & 0x07);
    int next = 0;
    if ((nr10 & 0x08) != 0) {
        next = sweep_.shadow - delta;
        sweep_.negate_used = true;
    } else {
        next = sweep_.shadow + delta;
    }
    if (next > 2047) squares_[0].on = false;
    return next;
}

void Apu::clock_sweep() {
    if (--sweep_.timer > 0) return;
    const std::uint8_t nr10 = reg(kNr10);
    const int period = (nr10 >> 4) & 0x07;
    sweep_.timer = period == 0 ? 8 : period;
    if (!sweep_.enabled || period == 0) return;
    const int next = sweep_calc();
    if (next <= 2047 && (nr10 & 0x07) != 0) {
        sweep_.shadow = next;
        regs_[0xFF13 - 0xFF10] = static_cast<std::uint8_t>(next & 0xFF);
        regs_[0xFF14 - 0xFF10] = static_cast<std::uint8_t>((reg(0xFF14) & 0xF8) | (next >> 8));
        (void)sweep_calc();  // overflow check with the new frequency
    }
}

void Apu::clock_envelopes() {
    if (squares_[0].on) squares_[0].env.clock(reg(0xFF12));
    if (squares_[1].on) squares_[1].env.clock(reg(0xFF17));
    if (noise_.on) noise_.env.clock(reg(0xFF21));
}

// --- Waveform generation and output ---

void Apu::tick() {
    step_channels(4);
    mix_and_sample();
}

void Apu::step_channels(int t_cycles) {
    for (int ch = 0; ch < 2; ++ch) {
        Square& sq = squares_[static_cast<std::size_t>(ch)];
        sq.timer -= t_cycles;
        while (sq.timer <= 0) {
            sq.timer += (2048 - square_freq(ch)) * 4;
            sq.duty_pos = (sq.duty_pos + 1) & 7;
        }
    }
    if (wave_.on) {
        wave_.timer -= t_cycles;
        while (wave_.timer <= 0) {
            wave_.timer += (2048 - (reg(0xFF1D) | ((reg(0xFF1E) & 0x07) << 8))) * 2;
            wave_.pos = (wave_.pos + 1) & 31;
            const std::uint8_t byte = regs_[kWaveRam - 0xFF10 + wave_.pos / 2];
            wave_.sample = (wave_.pos & 1) == 0 ? byte >> 4 : byte & 0x0F;
        }
    }
    if (noise_.on) {
        noise_.timer -= t_cycles;
        const std::uint8_t nr43 = reg(kNr43);
        while (noise_.timer <= 0) {
            noise_.timer += kNoiseDivisor[nr43 & 0x07] << (nr43 >> 4);
            const auto bit = static_cast<std::uint16_t>((noise_.lfsr ^ (noise_.lfsr >> 1)) & 1);
            noise_.lfsr = static_cast<std::uint16_t>((noise_.lfsr >> 1) | (bit << 14));
            if ((nr43 & 0x08) != 0) {
                noise_.lfsr = static_cast<std::uint16_t>((noise_.lfsr & ~0x40) | (bit << 6));
            }
        }
    }
}

int Apu::channel_amplitude(int ch) const {
    if (ch < 2) {
        const Square& sq = squares_[static_cast<std::size_t>(ch)];
        const int duty = reg(kBase[static_cast<std::size_t>(ch)] + 1) >> 6;
        return sq.on && kDuty[static_cast<std::size_t>(duty)][static_cast<std::size_t>(sq.duty_pos)] ? sq.env.volume : 0;
    }
    if (ch == 2) {
        static constexpr std::array<int, 4> kWaveShift = {4, 0, 1, 2};  // mute, 100%, 50%, 25%
        return wave_.on ? wave_.sample >> kWaveShift[(reg(0xFF1C) >> 5) & 0x03] : 0;
    }
    return noise_.on && (noise_.lfsr & 1) == 0 ? noise_.env.volume : 0;
}

std::uint8_t Apu::pcm12() const {
    if (!powered_) return 0;
    return static_cast<std::uint8_t>(channel_amplitude(0) | (channel_amplitude(1) << 4));
}

std::uint8_t Apu::pcm34() const {
    if (!powered_) return 0;
    return static_cast<std::uint8_t>(channel_amplitude(2) | (channel_amplitude(3) << 4));
}

void Apu::mix_and_sample() {
    float left = 0.0F;
    float right = 0.0F;
    if (powered_) {
        std::array<int, 4> amp{};
        for (int ch = 0; ch < 4; ++ch) amp[static_cast<std::size_t>(ch)] = channel_amplitude(ch);

        const std::uint8_t nr51 = reg(kNr51);
        for (int ch = 0; ch < 4; ++ch) {
            if (!dac_on(ch)) continue;
            const float analog = static_cast<float>(amp[static_cast<std::size_t>(ch)]) / 7.5F - 1.0F;
            if ((nr51 & (0x10 << ch)) != 0) left += analog;
            if ((nr51 & (0x01 << ch)) != 0) right += analog;
        }
        const std::uint8_t nr50 = reg(kNr50);
        left *= static_cast<float>(((nr50 >> 4) & 0x07) + 1) / 32.0F;  // /4 channels, /8 volume steps
        right *= static_cast<float>((nr50 & 0x07) + 1) / 32.0F;
    }
    acc_left_ += left;
    acc_right_ += right;
    ++acc_count_;

    sample_phase_ += 4 * kSampleRate;
    if (sample_phase_ < kCpuHz) return;
    sample_phase_ -= kCpuHz;

    const float in_l = acc_left_ / static_cast<float>(acc_count_);
    const float in_r = acc_right_ / static_cast<float>(acc_count_);
    acc_left_ = acc_right_ = 0.0F;
    acc_count_ = 0;

    // High-pass: remove the DC offset the DACs add, as the real output capacitor does.
    const float out_l = in_l - hp_left_;
    const float out_r = in_r - hp_right_;
    hp_left_ = in_l - out_l * kHighPassCharge;
    hp_right_ = in_r - out_r * kHighPassCharge;

    if (samples_.size() >= kMaxBufferedSamples) samples_.erase(samples_.begin(), samples_.begin() + kMaxBufferedSamples / 2);
    samples_.push_back(out_l);
    samples_.push_back(out_r);
}

}  // namespace ezy::gb
