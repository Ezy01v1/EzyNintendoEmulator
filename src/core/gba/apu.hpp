#pragma once

#include <array>
#include <cstdint>
#include <span>
#include <vector>

#include "gba/scheduler.hpp"

namespace ezy::gba {

// The sound unit (0x04000060-0x040000A7, GBATEK "GBA Sound Controller").
//
// Two sources are mixed:
// - The four Game Boy channels ("PSG"): two squares (the first with frequency sweep), a
//   wave channel with two banks of wave RAM, and noise. They run at the Game Boy's
//   frequencies; their 512 Hz frame sequencer clocks length, sweep and envelope.
// - Two 8-bit "direct sound" channels, A and B, each a 32-byte FIFO. Timer 0 or 1
//   overflows pop the next sample; when a FIFO drops to half full it asks for a DMA
//   (DMA 1/2 in "special" timing) to refill it with 16 more bytes.
//
// Like the PPU and the timers there's no per-cycle ticking: one scheduler event every 512
// cycles (32768 Hz, the hardware's default output rate) advances the channels by that
// much and emits a stereo sample. Each channel's output is averaged over the 512 cycles,
// which keeps high notes from aliasing. Register writes take effect at once (a trigger
// restarts a channel immediately); only the waveform phase is at 512-cycle granularity.
//
// The APU works on the bus's I/O storage. The bus calls after_io_write() for writes in
// 0x60-0xA7. Write-only bits read back as written (not masked) for now.
class Apu {
public:
    static constexpr int kSampleRate = 32768;
    static constexpr std::uint32_t kCyclesPerSample = 512;

    Apu(std::span<std::uint8_t> io, Scheduler* scheduler);

    void start();  // schedule the first sample
    void on_sample(std::uint64_t when);
    void after_io_write(std::size_t offset, std::size_t size);

    // Timer `index` overflowed. Returns which FIFOs (bit 0: A, bit 1: B) now hold 16
    // bytes or fewer and want a DMA refill.
    [[nodiscard]] unsigned on_timer_overflow(int index);

    // Interleaved stereo floats (L R L R ...) at kSampleRate, until cleared.
    [[nodiscard]] std::span<const float> samples() const { return samples_; }
    void clear_samples() { samples_.clear(); }

    // For tests and the debugger.
    [[nodiscard]] bool enabled() const { return master_; }  // SOUNDCNT_X bit 7
    [[nodiscard]] bool channel_on(int ch) const;  // PSG channel 0-3
    [[nodiscard]] int fifo_size(int fifo) const { return fifos_[static_cast<std::size_t>(fifo)].count; }
    [[nodiscard]] std::int8_t fifo_sample(int fifo) const { return fifos_[static_cast<std::size_t>(fifo)].current; }

private:
    struct Envelope {
        int volume = 0;
        int timer = 0;
        void trigger(std::uint8_t reg);  // reg: bits 0-2 step time, 3 direction, 4-7 initial volume
        void clock(std::uint8_t reg);
    };

    struct Length {
        int counter = 0;
        void clock(bool enabled, bool& on);
    };

    struct Square {
        bool on = false;
        int timer = 0;  // cycles to the next duty step
        int duty_pos = 0;
        Envelope env;
        Length len;
    };

    struct Sweep {
        bool enabled = false;
        int timer = 0;
        int shadow = 0;
    };

    struct Wave {
        bool on = false;
        int timer = 0;
        int pos = 0;  // sample index, 0-31 (one bank) or 0-63 (both)
        Length len;
    };

    struct Noise {
        bool on = false;
        int timer = 0;
        std::uint16_t lfsr = 0x7FFF;
        Envelope env;
        Length len;
    };

    struct Fifo {
        std::array<std::int8_t, 32> data{};
        int read = 0;
        int count = 0;
        std::int8_t current = 0;  // the sample being played
        void push(std::uint8_t byte);
        void reset() { read = count = 0; }
    };

    [[nodiscard]] std::uint8_t io8(std::uint32_t offset) const { return io_[offset]; }
    [[nodiscard]] std::uint16_t io16(std::uint32_t offset) const;
    void set_io16(std::uint32_t offset, std::uint16_t value);

    [[nodiscard]] int square_period(int ch) const;
    [[nodiscard]] int wave_period() const;
    [[nodiscard]] int noise_period() const;
    [[nodiscard]] bool dac_on(int ch) const;

    void trigger(int ch);
    void power_off();
    [[nodiscard]] int sweep_calc();  // next frequency; turns channel 1 off on overflow

    void clock_frame_sequencer();
    // Advance each channel by `cycles`, returning its average output (-15..15) over them.
    [[nodiscard]] float advance_square(int ch, int cycles);
    [[nodiscard]] float advance_wave(int cycles);
    [[nodiscard]] float advance_noise(int cycles);
    [[nodiscard]] int wave_level() const;  // current wave sample, volume applied
    void update_status();  // SOUNDCNT_X bits 0-3

    void sync_wave_ram_to_io();  // the CPU sees the bank that isn't playing

    std::span<std::uint8_t> io_;
    Scheduler* scheduler_;

    std::array<Square, 2> squares_{};
    Sweep sweep_;
    Wave wave_;
    Noise noise_;
    std::array<std::array<std::uint8_t, 16>, 2> wave_ram_{};
    std::array<Fifo, 2> fifos_{};

    bool master_ = false;  // SOUNDCNT_X bit 7: off at power-on
    int frame_step_ = 0;
    std::uint32_t sequencer_cycles_ = 0;
    std::vector<float> samples_;
};

}  // namespace ezy::gba
