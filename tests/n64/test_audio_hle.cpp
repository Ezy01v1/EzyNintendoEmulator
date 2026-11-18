#include <doctest/doctest.h>

#include <array>
#include <cstdint>
#include <utility>
#include <vector>

#include "n64/rsp_audio_hle.hpp"

using ezy::n64::AudioHle;
using ezy::n64::RspTask;

namespace {

// A synthetic audio task: command list at 0x1000, microcode data at 0x2000 (only its
// resample table is read), sample buffers from 0x4000. The expected values below were
// measured on the real Mario Kart 64 microcode running on the emulated RSP.
struct Fixture {
    std::vector<std::uint8_t> rdram = std::vector<std::uint8_t>(0x10000);
    std::array<std::uint8_t, 0x1000> dmem{};
    std::vector<std::uint32_t> commands;
    std::vector<std::pair<std::uint32_t, std::uint32_t>> writes;

    void put16(std::uint32_t address, std::int16_t value) {
        rdram[address] = static_cast<std::uint8_t>(static_cast<std::uint16_t>(value) >> 8);
        rdram[address + 1] = static_cast<std::uint8_t>(value);
    }
    [[nodiscard]] std::int16_t get16(std::uint32_t address) const {
        return static_cast<std::int16_t>((rdram[address] << 8) | rdram[address + 1]);
    }
    void command(std::uint32_t w1, std::uint32_t w2) { commands.push_back(w1), commands.push_back(w2); }
    void load(std::uint32_t dmem_address, std::uint32_t address, std::uint32_t bytes) {
        command((0x14U << 24) | (bytes << 12) | dmem_address, address);
    }
    void save(std::uint32_t dmem_address, std::uint32_t address, std::uint32_t bytes) {
        command((0x15U << 24) | (bytes << 12) | dmem_address, address);
    }
    void run() {
        for (std::size_t i = 0; i < commands.size(); ++i) {
            for (unsigned b = 0; b < 4; ++b) rdram[0x1000 + i * 4 + b] = static_cast<std::uint8_t>(commands[i] >> (24 - 8 * b));
        }
        RspTask task;
        task.type = RspTask::kAudio;
        task.ucode_data = 0x2000, task.ucode_data_size = 0x300;
        task.data_ptr = 0x1000, task.data_size = static_cast<std::uint32_t>(commands.size() * 4);
        AudioHle::run(task, rdram, dmem, [this](std::uint32_t address, std::uint32_t bytes, bool write) {
            if (write) writes.emplace_back(address, bytes);
        });
    }
};

}  // namespace

TEST_CASE("audio HLE mixer scales the destination by 0x7FFF and rounds once") {
    Fixture f;
    const std::array<std::int16_t, 4> destination{24032, 31951, -17747, -1909};
    const std::array<std::int16_t, 4> source{-32768, -24849, -9011, 6827};
    for (unsigned i = 0; i < 4; ++i) f.put16(0x4000 + i * 2, destination[i]), f.put16(0x4100 + i * 2, source[i]);
    f.load(0x200, 0x4000, 16);
    f.load(0x000, 0x4100, 16);
    f.command((0x0CU << 24) | (0x010 << 12) | 0x4000, (0x000U << 16) | 0x200);  // gain 0.5
    f.save(0x200, 0x4200, 16);
    f.run();
    // A plain dst + round(src * gain) would give 7648, 19527, -22252 and 1505.
    CHECK(f.get16(0x4200) == 7647);
    CHECK(f.get16(0x4202) == 19526);
    CHECK(f.get16(0x4204) == -22252);
    CHECK(f.get16(0x4206) == 1505);
    REQUIRE(f.writes.size() == 1);
    CHECK(f.writes[0] == std::pair<std::uint32_t, std::uint32_t>{0x4200, 16});
}

TEST_CASE("audio HLE resampler rounds every tap and keeps its state in RDRAM") {
    Fixture f;
    const std::array<std::int16_t, 4> taps{0x0C39, 0x66AD, 0x0D46, -33};  // first set of the MK64 table
    for (unsigned i = 0; i < taps.size(); ++i) f.put16(0x2100 + i * 2, taps[i]);
    f.put16(0x3000, 10000);  // history, then accumulator 0
    f.command(0x08U << 24 | 0x100, (0x300U << 16) | 0x20);            // SETBUFF in 0x100, out 0x300, 32 bytes
    f.command(0x05U << 24, 0x3000);                                    // RESAMPLE, pitch 0, state at 0x3000
    f.save(0x300, 0x4000, 32);
    f.run();
    // 10000 * 0x0C39 / 32768 = 954.9: the microcode rounds each product (a sum of
    // truncated products, or one rounding of the sum, both differ on other inputs).
    for (unsigned i = 0; i < 16; ++i) CHECK(f.get16(0x4000 + i * 2) == 955);
    CHECK(f.get16(0x3000) == 10000);
    CHECK(f.get16(0x3008) == 0);
}

TEST_CASE("audio HLE only claims the microcode it reproduces") {
    std::vector<std::uint8_t> rdram(0x10000);
    RspTask task;
    task.type = RspTask::kAudio;
    task.ucode_data = 0x2000, task.ucode_data_size = 0x300;
    task.data_ptr = 0x1000, task.data_size = 8;
    CHECK_FALSE(AudioHle::supports(task, rdram));  // unknown microcode data
    task.type = 1;
    CHECK_FALSE(AudioHle::supports(task, rdram));  // graphics task
    task.type = RspTask::kAudio;
    task.data_ptr = 0xFFF8, task.data_size = 0x10;
    CHECK_FALSE(AudioHle::supports(task, rdram));  // command list outside RDRAM
}
