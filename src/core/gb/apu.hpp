#pragma once

#include <array>
#include <cstdint>
#include <span>
#include <vector>

#include "gb/model.hpp"

namespace ezy::gb {

// Audio Processing Unit (0xFF10-0xFF3F). Four channels: two squares (CH1 with frequency
// sweep), a programmable wave (CH3) and noise (CH4). Length, envelope and sweep are
// clocked by the 512 Hz frame sequencer, which on hardware is driven by DIV (see
// div_falling_edge()), not by a counter of its own.
//
// Output: stereo float samples at sample_rate(), interleaved L R L R, averaged over each
// sample's window of T-cycles and passed through a high-pass filter like the real
// output capacitor. The frontend drains them with samples()/clear_samples().
class Apu {
public:
    static constexpr int kSampleRate = 48'000;

    Apu();

    void tick();  // one M-cycle (4 T-cycles)
    // Called when bit 12 of the timer's internal counter (bit 4 of DIV) goes 1 -> 0.
    void div_falling_edge();

    [[nodiscard]] std::uint8_t read(std::uint16_t addr) const;
    void write(std::uint16_t addr, std::uint8_t value);

    [[nodiscard]] std::span<const float> samples() const { return samples_; }
    void clear_samples() { samples_.clear(); }
    [[nodiscard]] static constexpr int sample_rate() { return kSampleRate; }

    // For tests and the debugger.
    [[nodiscard]] bool powered() const { return powered_; }
    [[nodiscard]] bool channel_on(int ch) const;  // ch 0-3
    [[nodiscard]] int frame_step() const { return frame_step_; }

    // The DMG and CGB APUs differ in a few corner cases (length counters while powered
    // off, wave RAM access while CH3 plays). Set once at power-on.
    void set_model(Model model) { model_ = model; }
    // CGB PCM12/PCM34 (0xFF76/0xFF77): the current 4-bit digital output of CH1|CH2 and
    // CH3|CH4 (low nibble = the lower channel).
    [[nodiscard]] std::uint8_t pcm12() const;
    [[nodiscard]] std::uint8_t pcm34() const;

private:
    struct Envelope {
        int volume = 0;
        int timer = 0;
        void trigger(std::uint8_t nrx2);
        void clock(std::uint8_t nrx2);
    };

    struct Length {
        int counter = 0;
        void clock(bool enabled, bool& channel_on);
    };

    struct Square {
        bool on = false;
        int timer = 0;
        int duty_pos = 0;
        Envelope env;
        Length len;
    };

    struct Sweep {
        bool enabled = false;
        int timer = 0;
        int shadow = 0;
        bool negate_used = false;
    };

    struct Wave {
        bool on = false;
        int timer = 0;
        int pos = 0;
        std::uint8_t sample = 0;  // last nibble read from wave RAM
        Length len;
    };

    struct Noise {
        bool on = false;
        int timer = 0;
        std::uint16_t lfsr = 0x7FFF;
        Envelope env;
        Length len;
    };

    [[nodiscard]] std::uint8_t reg(std::uint16_t addr) const { return regs_[addr - 0xFF10]; }
    [[nodiscard]] int square_freq(int ch) const;
    [[nodiscard]] bool dac_on(int ch) const;

    void write_length_reg(int ch, std::uint8_t value);
    void write_control_reg(int ch, std::uint8_t value);  // NRx4: length enable + trigger
    void trigger(int ch);
    void power_off();

    void clock_lengths();
    void clock_sweep();
    void clock_envelopes();
    [[nodiscard]] int sweep_calc();  // next frequency; disables CH1 on overflow

    void step_channels(int t_cycles);
    void mix_and_sample();
    [[nodiscard]] int channel_amplitude(int ch) const;  // 0-15, before the DAC

    std::array<std::uint8_t, 0x30> regs_{};  // raw FF10-FF3F (wave RAM at the end)
    bool powered_ = true;
    Model model_ = Model::Dmg;
    int frame_step_ = 0;  // next frame sequencer step to run (0-7)

    std::array<Square, 2> squares_{};
    Sweep sweep_;
    Wave wave_;
    Noise noise_;

    // Resampling: accumulate the mix every M-cycle, emit the average once per sample.
    float acc_left_ = 0.0F;
    float acc_right_ = 0.0F;
    int acc_count_ = 0;
    std::uint32_t sample_phase_ = 0;
    float hp_left_ = 0.0F;
    float hp_right_ = 0.0F;
    std::vector<float> samples_;
};

}  // namespace ezy::gb
