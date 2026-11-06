#include <doctest/doctest.h>

#include <cmath>
#include <cstdint>

#include "gb/apu.hpp"
#include "gb/model.hpp"

using ezy::gb::Apu;
using ezy::gb::Model;

namespace {

// Runs the frame sequencer `steps` times (each is one DIV bit-4 falling edge).
void sequence(Apu& apu, int steps) {
    for (int i = 0; i < steps; ++i) apu.div_falling_edge();
}

void run_m_cycles(Apu& apu, int n) {
    for (int i = 0; i < n; ++i) apu.tick();
}

}  // namespace

TEST_CASE("APU unused register bits read as 1") {
    Apu apu;
    apu.write(0xFF26, 0x00);
    apu.write(0xFF26, 0x80);
    CHECK(apu.read(0xFF10) == 0x80);
    CHECK(apu.read(0xFF11) == 0x3F);
    CHECK(apu.read(0xFF13) == 0xFF);  // write-only frequency
    CHECK(apu.read(0xFF14) == 0xBF);
    CHECK(apu.read(0xFF1A) == 0x7F);
    CHECK(apu.read(0xFF1C) == 0x9F);
    CHECK(apu.read(0xFF15) == 0xFF);
    CHECK(apu.read(0xFF27) == 0xFF);
    CHECK(apu.read(0xFF26) == 0xF0);  // powered, no channel on
}

TEST_CASE("APU power off clears registers and ignores writes, except wave RAM") {
    Apu apu;
    apu.write(0xFF24, 0x77);
    apu.write(0xFF26, 0x00);
    CHECK_FALSE(apu.powered());
    CHECK(apu.read(0xFF24) == 0x00);
    CHECK(apu.read(0xFF26) == 0x70);
    apu.write(0xFF24, 0x55);
    CHECK(apu.read(0xFF24) == 0x00);
    apu.write(0xFF30, 0x12);
    CHECK(apu.read(0xFF30) == 0x12);
}

TEST_CASE("APU trigger turns a channel on only if its DAC is on") {
    Apu apu;
    apu.write(0xFF17, 0x00);  // CH2 DAC off
    apu.write(0xFF19, 0x80);
    CHECK_FALSE(apu.channel_on(1));
    apu.write(0xFF17, 0xF0);  // volume 15: DAC on
    apu.write(0xFF19, 0x80);
    CHECK(apu.channel_on(1));
    CHECK((apu.read(0xFF26) & 0x02) != 0);
    apu.write(0xFF17, 0x00);  // turning the DAC off disables the channel
    CHECK_FALSE(apu.channel_on(1));
}

TEST_CASE("APU length counter disables the channel") {
    Apu apu;
    apu.write(0xFF26, 0x00);
    apu.write(0xFF26, 0x80);  // frame sequencer back to step 0
    apu.write(0xFF17, 0xF0);
    apu.write(0xFF16, 0x3E);  // length 64 - 62 = 2
    apu.write(0xFF19, 0xC0);  // trigger with length enabled
    CHECK(apu.channel_on(1));
    sequence(apu, 1);         // step 0 clocks length -> 1
    CHECK(apu.channel_on(1));
    sequence(apu, 2);         // step 1 doesn't, step 2 does -> 0
    CHECK_FALSE(apu.channel_on(1));
}

TEST_CASE("APU CH1 sweep overflow on trigger disables the channel") {
    Apu apu;
    apu.write(0xFF12, 0xF0);
    apu.write(0xFF10, 0x11);  // period 1, increase, shift 1
    apu.write(0xFF13, 0xFF);
    apu.write(0xFF14, 0x87);  // freq 2047 + 2047/2 > 2047
    CHECK_FALSE(apu.channel_on(0));
}

TEST_CASE("APU produces 48 kHz stereo samples and a square wave is audible") {
    Apu apu;
    apu.write(0xFF25, 0xFF);   // all channels to both sides
    apu.write(0xFF24, 0x77);
    apu.write(0xFF16, 0x80);   // 50% duty
    apu.write(0xFF17, 0xF0);
    apu.write(0xFF18, 0x00);
    apu.write(0xFF19, 0x87);   // freq 0x700: ~1 kHz, trigger
    run_m_cycles(apu, 4'194'304 / 4 / 10);  // 0.1 s
    const auto s = apu.samples();
    CHECK(s.size() == doctest::Approx(2 * 4800).epsilon(0.01));
    float peak = 0.0F;
    for (const float v : s) peak = std::fmax(peak, std::fabs(v));
    CHECK(peak > 0.1F);
    CHECK(peak <= 1.0F);
    apu.clear_samples();
    CHECK(apu.samples().empty());
}

TEST_CASE("PCM12 and PCM34 report the channel outputs") {
    Apu apu;
    apu.set_model(Model::Cgb);
    CHECK(apu.pcm12() == 0x00);
    apu.write(0xFF12, 0xF0);  // CH1 volume 15, DAC on
    apu.write(0xFF11, 0xC0);  // 75% duty: high most of the time
    apu.write(0xFF14, 0x87);  // trigger
    bool saw_high = false;
    for (int i = 0; i < 4096 && !saw_high; ++i) {
        apu.tick();
        saw_high = (apu.pcm12() & 0x0F) == 0x0F;
    }
    CHECK(saw_high);
    CHECK((apu.pcm12() >> 4) == 0);  // CH2 silent
    CHECK(apu.pcm34() == 0x00);
}

TEST_CASE("CGB length registers ignore writes while the APU is off") {
    Apu apu;
    apu.set_model(Model::Cgb);
    apu.write(0xFF26, 0x00);
    apu.write(0xFF11, 0x3F);  // length = 1 if accepted
    apu.write(0xFF26, 0x80);
    apu.write(0xFF12, 0xF0);
    apu.write(0xFF14, 0xC0);  // trigger with length enabled: length 0 -> reloads 64
    apu.div_falling_edge();   // one length clock
    CHECK(apu.channel_on(0));
}

TEST_CASE("DMG length registers accept writes while the APU is off") {
    Apu apu;
    apu.write(0xFF26, 0x00);
    apu.write(0xFF11, 0x3F);  // length = 1
    apu.write(0xFF26, 0x80);
    apu.write(0xFF12, 0xF0);
    apu.write(0xFF14, 0xC0);
    apu.div_falling_edge();
    CHECK_FALSE(apu.channel_on(0));
}

TEST_CASE("wave RAM while CH3 plays: readable on CGB, 0xFF on DMG") {
    for (const Model model : {Model::Cgb, Model::Dmg}) {
        Apu apu;
        apu.set_model(model);
        apu.write(0xFF30, 0x12);
        apu.write(0xFF1A, 0x80);  // DAC on
        apu.write(0xFF1E, 0x80);  // trigger
        apu.tick();
        if (model == Model::Cgb) {
            CHECK(apu.read(0xFF30) != 0xFF);
        } else {
            CHECK(apu.read(0xFF30) == 0xFF);
        }
    }
}
