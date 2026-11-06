#include <doctest/doctest.h>

#include <cmath>
#include <cstdint>
#include <vector>

#include "gba/apu.hpp"
#include "gba/bus.hpp"
#include "gba/scheduler.hpp"

using namespace ezy::gba;

namespace {

constexpr std::uint32_t kIo = 0x04000000;
constexpr std::uint32_t kSound1CntL = kIo + 0x60;
constexpr std::uint32_t kSound1CntH = kIo + 0x62;
constexpr std::uint32_t kSound1CntX = kIo + 0x64;
constexpr std::uint32_t kSound3CntL = kIo + 0x70;
constexpr std::uint32_t kSound3CntH = kIo + 0x72;
constexpr std::uint32_t kSound3CntX = kIo + 0x74;
constexpr std::uint32_t kSound4CntL = kIo + 0x78;
constexpr std::uint32_t kSound4CntH = kIo + 0x7C;
constexpr std::uint32_t kSoundCntL = kIo + 0x80;
constexpr std::uint32_t kSoundCntH = kIo + 0x82;
constexpr std::uint32_t kSoundCntX = kIo + 0x84;
constexpr std::uint32_t kWaveRam = kIo + 0x90;
constexpr std::uint32_t kFifoA = kIo + 0xA0;
constexpr std::uint32_t kTm0 = kIo + 0x100;
constexpr std::uint32_t kDma1 = kIo + 0xBC;  // SAD, DAD, CNT_L, CNT_H

// A bus with the APU running, driven by hand: advance time and dispatch APU/timer events.
struct Fixture {
    Scheduler scheduler;
    Bus bus{std::vector<std::uint8_t>(0x200), {}, &scheduler};

    Fixture() { bus.apu().start(); }

    void run_for(std::uint64_t cycles) {
        const std::uint64_t end = scheduler.now() + cycles;
        while (true) {
            const auto next = scheduler.next_event_time();
            if (!next || *next > end) break;
            scheduler.advance_to(*next);
            while (const auto e = scheduler.pop_due()) {
                switch (e->type) {
                    case EventType::ApuSample: bus.apu().on_sample(e->when); break;
                    case EventType::Timer0: bus.timers().on_overflow(0, e->when); break;
                    case EventType::Timer1: bus.timers().on_overflow(1, e->when); break;
                    default: break;
                }
            }
        }
        scheduler.advance_to(end);
    }
    void run_samples(int n) { run_for(static_cast<std::uint64_t>(n) * Apu::kCyclesPerSample); }

    // Master on, every PSG channel to both sides at full volume, PSG ratio 100%.
    void sound_on() {
        bus.write16(kSoundCntX, 0x0080);
        bus.write16(kSoundCntL, 0xFF77);
        bus.write16(kSoundCntH, 0x0002);
    }

    // Left-channel samples produced since the last call.
    std::vector<float> take_left() {
        std::vector<float> left;
        const auto s = bus.apu().samples();
        for (std::size_t i = 0; i < s.size(); i += 2) left.push_back(s[i]);
        bus.apu().clear_samples();
        return left;
    }
};

int sign_changes(const std::vector<float>& v) {
    int changes = 0;
    for (std::size_t i = 1; i < v.size(); ++i) {
        if ((v[i - 1] < 0) != (v[i] < 0)) ++changes;
    }
    return changes;
}

float peak(const std::vector<float>& v) {
    float p = 0;
    for (const float x : v) p = std::max(p, std::fabs(x));
    return p;
}

// Peak over the last `n` samples only: the level at the end of a window.
float peak_at_end(const std::vector<float>& v, std::size_t n = 128) {
    return peak(std::vector<float>(v.end() - static_cast<std::ptrdiff_t>(std::min(n, v.size())), v.end()));
}

}  // namespace

TEST_CASE("APU: SOUNDBIAS resets to 0x200, sound starts powered off") {
    Fixture f;
    CHECK(f.bus.read16(kIo + 0x88) == 0x0200);
    CHECK_FALSE(f.bus.apu().enabled());
    f.run_samples(4);
    CHECK(f.bus.apu().samples().size() == 8);  // stereo, silent
    CHECK(f.bus.apu().samples()[0] == 0.0F);
}

TEST_CASE("APU: PSG registers are locked at 0 while the master enable is off") {
    Fixture f;
    f.bus.write16(kSound1CntH, 0xF080);
    CHECK(f.bus.read16(kSound1CntH) == 0);
    f.sound_on();
    f.bus.write16(kSound1CntH, 0xF080);
    CHECK(f.bus.read16(kSound1CntH) == 0xF080);
    f.bus.write16(kSoundCntX, 0);  // powering off clears them
    CHECK(f.bus.read16(kSound1CntH) == 0);
    CHECK(f.bus.read16(kSoundCntL) == 0);
}

TEST_CASE("APU square: trigger, status bit, and pitch 131072 / (2048 - freq) Hz") {
    Fixture f;
    f.sound_on();
    f.bus.write16(kSound1CntH, 0xF080);            // volume 15, no envelope, 50% duty
    f.bus.write16(kSound1CntX, 0x8000 | 1792);     // trigger, 512 Hz
    CHECK(f.bus.apu().channel_on(0));
    CHECK((f.bus.read16(kSoundCntX) & 0x0F) == 0x01);
    CHECK((f.bus.read16(kSound1CntX) & 0x8000) == 0);  // the trigger bit is write-only
    f.take_left();
    f.run_samples(Apu::kSampleRate / 4);  // 0.25 s: 128 periods, 256 sign changes
    const auto left = f.take_left();
    CHECK(std::abs(sign_changes(left) - 256) <= 4);
    // Full scale: 15 * 8 (master volume) = 120 of 512.
    CHECK(peak(left) == doctest::Approx(120.0F / 512.0F).epsilon(0.01));
}

TEST_CASE("APU: the length counter silences a channel") {
    Fixture f;
    f.sound_on();
    f.bus.write16(kSound1CntH, 0xF000 | 63);         // length 63: 1 step left
    f.bus.write16(kSound1CntX, 0xC000 | 1792);       // trigger + length enable
    CHECK(f.bus.apu().channel_on(0));
    f.run_for(2 * 32768ULL);  // two frame sequencer steps: one length clock at least
    CHECK_FALSE(f.bus.apu().channel_on(0));
    CHECK((f.bus.read16(kSoundCntX) & 0x01) == 0);
}

TEST_CASE("APU: the envelope fades a square out at 64 Hz") {
    Fixture f;
    f.sound_on();
    f.bus.write16(kSound1CntH, 0xF100 | 0x80);  // volume 15, decreasing, step 1 (1/64 s)
    f.bus.write16(kSound1CntX, 0x8000 | 1792);
    f.take_left();
    f.run_samples(Apu::kSampleRate / 64);  // about one envelope step
    const float early = peak_at_end(f.take_left());
    f.run_samples(Apu::kSampleRate / 8);  // 8 more steps: volume ~6
    const float later = peak_at_end(f.take_left());
    CHECK(early >= 13 * 8 / 512.0F);
    CHECK(later <= 7 * 8 / 512.0F);
    CHECK(later >= 5 * 8 / 512.0F);
    f.run_samples(Apu::kSampleRate / 2);
    CHECK(peak_at_end(f.take_left()) == 0.0F);  // volume 0, the channel is still on
    CHECK(f.bus.apu().channel_on(0));
}

TEST_CASE("APU sweep: an overflowing sweep turns channel 1 off at trigger") {
    Fixture f;
    f.sound_on();
    f.bus.write16(kSound1CntL, 0x0011);  // period 1, increase, shift 1
    f.bus.write16(kSound1CntH, 0xF080);
    f.bus.write16(kSound1CntX, 0x8000 | 2000);  // 2000 + 1000 > 2047
    CHECK_FALSE(f.bus.apu().channel_on(0));
}

TEST_CASE("APU: writing a DAC off (envelope 0, decreasing) stops the channel") {
    Fixture f;
    f.sound_on();
    f.bus.write16(kSound1CntH, 0xF080);
    f.bus.write16(kSound1CntX, 0x8000 | 1792);
    CHECK(f.bus.apu().channel_on(0));
    f.bus.write16(kSound1CntH, 0x0080);
    CHECK_FALSE(f.bus.apu().channel_on(0));
}

TEST_CASE("APU wave RAM: the CPU sees the bank that isn't playing") {
    Fixture f;
    f.sound_on();
    f.bus.write16(kSound3CntL, 0x0000);   // playing bank 0: the CPU sees bank 1
    f.bus.write32(kWaveRam, 0x11111111);  // into bank 1
    f.bus.write16(kSound3CntL, 0x0040);   // playing bank 1: the CPU sees bank 0
    CHECK(f.bus.read32(kWaveRam) == 0);
    f.bus.write32(kWaveRam, 0x22222222);  // into bank 0
    f.bus.write16(kSound3CntL, 0x0000);
    CHECK(f.bus.read32(kWaveRam) == 0x11111111);
}

TEST_CASE("APU wave: plays the wave RAM nibbles at the chosen volume") {
    Fixture f;
    f.sound_on();
    // Bank 0 (written while bank 1 plays): a square wave, 16 samples of 0xF then 16 of 0x0.
    f.bus.write16(kSound3CntL, 0x0040);
    for (std::uint32_t i = 0; i < 8; i += 4) f.bus.write32(kWaveRam + i, 0xFFFFFFFF);
    for (std::uint32_t i = 8; i < 16; i += 4) f.bus.write32(kWaveRam + i, 0x00000000);
    f.bus.write16(kSound3CntL, 0x0080);           // play bank 0, DAC on
    f.bus.write16(kSound3CntH, 0x2000);           // 100%
    f.bus.write16(kSound3CntX, 0x8000 | 1984);    // (2048 - 1984) * 8 = 512 cycles per sample: 1024 Hz
    CHECK(f.bus.apu().channel_on(2));
    f.take_left();
    f.run_samples(Apu::kSampleRate / 4);
    const auto left = f.take_left();
    CHECK(peak(left) == doctest::Approx(120.0F / 512.0F).epsilon(0.02));
    CHECK(std::abs(sign_changes(left) - 512) <= 8);  // 1024 Hz for 0.25 s
    f.bus.write16(kSound3CntH, 0x6000);  // 25%
    f.take_left();
    f.run_samples(1024);
    CHECK(peak(f.take_left()) < 40.0F / 512.0F);
}

TEST_CASE("APU noise: a triggered noise channel outputs both signs") {
    Fixture f;
    f.sound_on();
    f.bus.write16(kSound4CntL, 0xF000);
    f.bus.write16(kSound4CntH, 0x8000 | 0x0021);  // trigger, divider 1, shift 2
    CHECK(f.bus.apu().channel_on(3));
    f.take_left();
    f.run_samples(1024);
    CHECK(sign_changes(f.take_left()) > 50);
}

TEST_CASE("APU FIFO: writes queue bytes, timer overflows play them, reset empties") {
    Fixture f;
    f.sound_on();
    f.bus.write16(kSoundCntH, 0x0302 | 0x0004);  // FIFO A to both sides at 100%, timer 0
    f.bus.write32(kFifoA, 0x40302010);
    CHECK(f.bus.apu().fifo_size(0) == 4);
    f.bus.write16(kTm0, 0xFF00);   // reload: overflow every 256 cycles
    f.bus.write16(kTm0 + 2, 0x0080);
    f.run_for(256);
    CHECK(f.bus.apu().fifo_sample(0) == 0x10);
    CHECK(f.bus.apu().fifo_size(0) == 3);
    f.run_for(256);
    CHECK(f.bus.apu().fifo_sample(0) == 0x20);
    // 0x20 = 32, doubled at 100% = 64 of 512.
    f.take_left();
    f.run_samples(1);
    f.bus.write16(kSoundCntH, 0x0B06);  // FIFO A reset bit
    CHECK(f.bus.apu().fifo_size(0) == 0);
    CHECK((f.bus.read16(kSoundCntH) & 0x0800) == 0);  // write-only
}

TEST_CASE("APU FIFO: DMA 1 in special timing refills it 16 bytes at a time") {
    Fixture f;
    f.sound_on();
    for (std::uint32_t i = 0; i < 64; ++i) f.bus.write8(0x02000000 + i, static_cast<std::uint8_t>(i));
    f.bus.write16(kSoundCntH, 0x0306);  // FIFO A, both sides, 100%, timer 0
    f.bus.write32(kDma1, 0x02000000);
    f.bus.write32(kDma1 + 4, 0x040000A0);
    f.bus.write16(kDma1 + 8, 4);
    f.bus.write16(kDma1 + 10, 0x8000 | 0x3000 | 0x0400 | 0x0200 | 0x0040);  // on, special, word, repeat, dst fixed
    CHECK(f.bus.apu().fifo_size(0) == 0);  // special timing waits for the FIFO to ask
    f.bus.write16(kTm0, 0xFF00);
    f.bus.write16(kTm0 + 2, 0x0080);
    f.run_for(256);  // empty FIFO overflows: asks for a refill
    CHECK(f.bus.apu().fifo_size(0) == 16);
    std::vector<int> played;
    for (int i = 0; i < 40; ++i) {
        f.run_for(256);
        played.push_back(f.bus.apu().fifo_sample(0));
    }
    for (int i = 0; i < 40; ++i) CHECK(played[static_cast<std::size_t>(i)] == i);  // in order, no gaps
    CHECK(f.bus.apu().fifo_size(0) > 0);
}

TEST_CASE("APU mixing: a FIFO sample at 100% to both sides, through the bias and clipping") {
    Fixture f;
    f.bus.write16(kSoundCntX, 0x0080);
    f.bus.write16(kSoundCntH, 0x0304);  // FIFO A both sides at 100%; PSG ratio 25% (silent anyway)
    f.bus.write32(kFifoA, 0x40404040);  // four samples of 64 (a word write queues 4 bytes)
    f.bus.write16(kTm0, 0xFF00);
    f.bus.write16(kTm0 + 2, 0x0080);
    f.run_for(256);
    f.take_left();
    f.run_samples(1);
    const auto s = f.bus.apu().samples();
    CHECK(s[0] == doctest::Approx(128.0F / 512.0F));
    CHECK(s[1] == doctest::Approx(128.0F / 512.0F));
    f.bus.write16(kIo + 0x88, 0x0000);  // bias 0: negative values clip to 0
    f.bus.write16(kSoundCntH, 0x0B04);  // reset FIFO A, then queue -64s
    f.bus.write32(kFifoA, 0xC0C0C0C0);
    f.run_for(256);
    f.bus.apu().clear_samples();
    f.run_samples(1);
    CHECK(f.bus.apu().samples()[0] == 0.0F);
}
