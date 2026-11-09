#include <doctest/doctest.h>

#include <array>
#include <cstdint>
#include <initializer_list>
#include <string>
#include <vector>

#include "n64/gfx_hle/f3dex.hpp"
#include "n64/gfx_hle/task.hpp"
#include "n64/n64.hpp"
#include "n64_test_helpers.hpp"

using namespace ezy::n64;
using namespace n64_test;

namespace {

// The fill list of "RDP runs a frozen RDRAM list...": a red/green 2x2 checker clipped by
// the scissor, drawn into a 5-pixel-wide RGBA5551 image at 0x4000.
const std::vector<std::uint64_t> kCheckerList = {
    0x3F10000400004000ULL,
    rdp_rectangle(0x2D, 1, 1, 4, 3),
    kFillMode,
    0x37000000F80107C1ULL,
    rdp_rectangle(0x36, 3, 2, 0, 0),
    kSyncFull,
};

constexpr std::uint32_t kSpStatus = 0x04040010, kMiIntr = 0x04300008, kMiMask = 0x0430000C;
constexpr std::uint32_t kSpHalt = 1, kSpBroke = 2, kSpSignal2 = 1U << 9;
constexpr std::uint32_t kUcodeData = 0x200000, kDisplayList = 0x210000;
constexpr std::uint64_t kEndDl = 0xB800000000000000ULL;

void write_bytes(Bus& bus, std::uint32_t address, const std::string& text) {
    for (std::size_t i = 0; i < text.size(); ++i) bus.write8(address + static_cast<std::uint32_t>(i), static_cast<std::uint8_t>(text[i]));
}

// A fake ucode data segment carrying the version string the HLE looks for.
void write_ucode_data(Bus& bus, const std::string& version) {
    for (std::uint32_t i = 0; i < 0x800; i += 4) bus.write32(kUcodeData + i, 0);
    write_bytes(bus, kUcodeData + 0x130, "RSP Gfx ucode " + version + " Yoshitaka Yasumoto Nintendo.");
}

void write_os_task(Bus& bus, const ezy::n64::OsTask& task) {
    const std::array<std::uint32_t, 16> fields = {
        task.type, task.flags, task.ucode_boot, task.ucode_boot_size, task.ucode, task.ucode_size,
        task.ucode_data, task.ucode_data_size, task.dram_stack, task.dram_stack_size, task.output_buff,
        task.output_buff_size, task.data_ptr, task.data_size, task.yield_data_ptr, task.yield_data_size};
    for (std::size_t i = 0; i < fields.size(); ++i) bus.write32(0x04000FC0 + static_cast<std::uint32_t>(i) * 4, fields[i]);
}

ezy::n64::OsTask graphics_task(std::uint32_t type = 1) {
    ezy::n64::OsTask task{};
    task.type = type;
    task.ucode_data = kUcodeData;
    task.ucode_data_size = 0x800;
    task.data_ptr = kDisplayList;
    return task;
}

// IMEM program that leaves a mark in DMEM and breaks: shows whether the real RSP ran.
void write_marking_program(Bus& bus) {
    const std::uint32_t program[] = {mips::ori(1, 0, 0x1234), mips::sw(1, 0, 0), mips::special(0, 0, 0, 0, 0x0D)};
    std::uint32_t address = 0x04001000;
    for (const auto word : program) bus.write32(address, word), address += 4;
}

void start_rsp(Bus& bus) { bus.write32(kSpStatus, 1 | 4 | 0x100); }  // clear HALT, BROKE; interrupt on break

std::uint16_t pixel(Bus& bus, unsigned x, unsigned y) {
    const std::uint32_t address = 0x4000 + (y * 5 + x) * 2;
    return static_cast<std::uint16_t>(bus.read32(address & ~3U) >> ((address & 2) ? 0 : 16));
}

}  // namespace

TEST_CASE("RDP submit_words draws like a fetched list") {
    const auto n64 = N64::create(counting_rom("HLE RDP"));
    REQUIRE(n64);
    auto& bus = n64->bus();
    bus.write32(0x0430000C, 1U << 11);  // unmask DP
    bus.rdp().submit_words(kCheckerList, bus.mi(), bus.rdram());
    CHECK(pixel(bus, 1, 1) == 0xF801);  // red
    CHECK(pixel(bus, 2, 1) == 0x07C1);  // green
    CHECK(pixel(bus, 0, 0) == 0);       // outside the scissor
    CHECK(bus.interrupt_line());        // Sync Full
    CHECK((bus.read32(0x0410000C) & 0x20) == 0);  // pipe no longer busy after Sync Full
}

TEST_CASE("RDP trace records every consumed word") {
    const auto n64 = N64::create(counting_rom("HLE TRACE"));
    REQUIRE(n64);
    auto& bus = n64->bus();
    std::vector<std::uint64_t> trace;
    bus.rdp().set_trace(&trace);

    std::uint32_t end = 0x1000;
    for (const auto command : kCheckerList) {
        bus.write64(end, command);
        end += 8;
    }
    bus.write32(kDpcStart, 0x1000);
    bus.write32(kDpcEnd, end);
    for (unsigned i = 0; i < 40; ++i) n64->step();
    CHECK(trace == kCheckerList);

    trace.clear();
    bus.rdp().submit_words(kCheckerList, bus.mi(), bus.rdram());
    CHECK(trace == kCheckerList);
}

TEST_CASE("identify_gfx_ucode recognises F3DEX and F3DLX 0.95 and nothing else") {
    const auto n64 = N64::create(counting_rom("HLE IDENTIFY"));
    REQUIRE(n64);
    auto& bus = n64->bus();
    const OsTask task = graphics_task();
    write_ucode_data(bus, "F3DEX         0.95");
    CHECK(identify_gfx_ucode(task, bus.rdram()) == GfxUcode::F3dex095);
    write_ucode_data(bus, "F3DLX         0.95");
    CHECK(identify_gfx_ucode(task, bus.rdram()) == GfxUcode::F3dlx095);
    write_ucode_data(bus, "F3DEX         1.23");
    CHECK(identify_gfx_ucode(task, bus.rdram()) == GfxUcode::Unknown);
}

TEST_CASE("A recognised graphics task with HLE on completes without running the RSP") {
    const auto n64 = N64::create(counting_rom("HLE TASK"));
    REQUIRE(n64);
    auto& bus = n64->bus();
    n64->set_hle_gfx(true);
    write_ucode_data(bus, "F3DEX         0.95");
    bus.write64(kDisplayList, kEndDl);
    write_os_task(bus, graphics_task());
    write_marking_program(bus);
    bus.write32(kMiMask, 2);  // unmask SP
    start_rsp(bus);
    for (unsigned i = 0; i < 3000; ++i) n64->step();
    const std::uint32_t status = bus.read32(kSpStatus);
    CHECK((status & (kSpHalt | kSpBroke)) == (kSpHalt | kSpBroke));
    CHECK((status & kSpSignal2) != 0);
    CHECK((bus.read32(kMiIntr) & 1) != 0);
    CHECK(bus.read32(0x04000000) == 0);  // the RSP program never ran
}

TEST_CASE("Audio tasks and HLE off still run on the RSP") {
    for (const bool audio : {true, false}) {
        CAPTURE(audio);
        const auto n64 = N64::create(counting_rom("HLE AUDIO"));
        REQUIRE(n64);
        auto& bus = n64->bus();
        n64->set_hle_gfx(audio);  // audio task with HLE on, or graphics task with HLE off
        write_ucode_data(bus, "F3DEX         0.95");
        bus.write64(kDisplayList, kEndDl);
        write_os_task(bus, graphics_task(audio ? 2 : 1));
        write_marking_program(bus);
        start_rsp(bus);
        for (unsigned i = 0; i < 200; ++i) n64->step();
        CHECK(bus.read32(0x04000000) == 0x1234);
    }
}

namespace {

// Writes display-list commands (w0, w1 pairs) at an RDRAM address.
void write_dl(Bus& bus, std::uint32_t address, std::initializer_list<std::uint64_t> commands) {
    for (const auto command : commands) bus.write64(address, command), address += 8;
}

constexpr std::uint64_t dl_cmd(std::uint32_t w0, std::uint32_t w1) { return (std::uint64_t{w0} << 32) | w1; }

struct HleRun {
    std::vector<std::uint64_t> trace;
    F3dex::Result result;
};

HleRun run_hle(N64& n64, GfxUcode ucode = GfxUcode::F3dex095) {
    auto& bus = n64.bus();
    HleRun run{};
    bus.rdp().set_trace(&run.trace);
    F3dex f3dex;
    run.result = f3dex.run_task(graphics_task(), ucode, bus.rdram(), bus.rdp(), bus.mi());
    bus.rdp().set_trace(nullptr);
    return run;
}

}  // namespace

TEST_CASE("F3DEX segments, nested display lists and ENDDL") {
    const auto n64 = N64::create(counting_rom("HLE DL"));
    REQUIRE(n64);
    auto& bus = n64->bus();
    const std::uint64_t inner_rect = rdp_rectangle(0xF6, 4, 4, 0, 0);
    const std::uint64_t outer_rect = rdp_rectangle(0xF6, 8, 8, 4, 4);
    write_dl(bus, kDisplayList, {
        dl_cmd(0xBC001806, 0x00100000),  // segment 6 = 0x100000
        dl_cmd(0x06000000, 0x06000100),  // G_DL push to segment 6 + 0x100
        outer_rect,
        kEndDl,
    });
    write_dl(bus, 0x100100, {inner_rect, kEndDl});
    const HleRun run = run_hle(*n64);
    CHECK(run.result.ok);
    CHECK(run.trace == std::vector<std::uint64_t>{inner_rect, outer_rect});
}

TEST_CASE("F3DEX stops at a DL nested 11 deep and at addresses outside RDRAM") {
    const auto n64 = N64::create(counting_rom("HLE DEPTH"));
    REQUIRE(n64);
    auto& bus = n64->bus();
    // Each list calls the next one, 0x100 further on, eleven times.
    for (std::uint32_t i = 0; i < 12; ++i) {
        write_dl(bus, kDisplayList + i * 0x100, {dl_cmd(0x06000000, kDisplayList + (i + 1) * 0x100), kEndDl});
    }
    CHECK_FALSE(run_hle(*n64).result.ok);

    write_dl(bus, kDisplayList, {dl_cmd(0x06000000, 0x00FFFFF8), kEndDl});
    CHECK_FALSE(run_hle(*n64).result.ok);
    write_dl(bus, kDisplayList, {dl_cmd(0x06010000, 0x00900000), kEndDl});  // G_DL branch past RDRAM
    CHECK_FALSE(run_hle(*n64).result.ok);
}

TEST_CASE("F3DEX texture rectangle pass-through uses RDPHALF words") {
    const auto n64 = N64::create(counting_rom("HLE TEXRECT"));
    REQUIRE(n64);
    auto& bus = n64->bus();
    const std::uint64_t texrect = dl_cmd(0xE4040040, 0x00010010);
    write_dl(bus, kDisplayList, {
        texrect,
        dl_cmd(0xB4000000, 0x00100020),  // G_RDPHALF_1: S, T
        dl_cmd(0xB3000000, 0x04000400),  // G_RDPHALF_2: DsDx, DtDy
        kEndDl,
    });
    const HleRun run = run_hle(*n64);
    CHECK(run.result.ok);
    REQUIRE(Rdp::words_in_command(0x24) == 2);
    CHECK(run.trace == std::vector<std::uint64_t>{texrect, 0x0010002004000400ULL});
}

TEST_CASE("F3DEX translates segmented image addresses") {
    const auto n64 = N64::create(counting_rom("HLE SETIMG"));
    REQUIRE(n64);
    auto& bus = n64->bus();
    write_dl(bus, kDisplayList, {
        dl_cmd(0xBC000806, 0x00300000),  // segment 2 = 0x300000
        dl_cmd(0xFF10013F, 0x02000400),  // Set Color Image at segment 2 + 0x400
        kEndDl,
    });
    const HleRun run = run_hle(*n64);
    CHECK(run.trace == std::vector<std::uint64_t>{dl_cmd(0xFF10013F, 0x00300400)});
}

TEST_CASE("G_LOAD_UCODE to F3DLX switches modes and to an unknown ucode stops") {
    const auto n64 = N64::create(counting_rom("HLE LOADUC"));
    REQUIRE(n64);
    auto& bus = n64->bus();
    constexpr std::uint32_t kOtherData = 0x220000;
    for (std::uint32_t i = 0; i < 0x800; i += 4) bus.write32(kOtherData + i, 0);
    write_bytes(bus, kOtherData + 0x130, "RSP Gfx ucode F3DLX         0.95 Yoshitaka Yasumoto Nintendo.");
    const std::uint64_t rect = rdp_rectangle(0xF6, 4, 4, 0, 0);
    write_dl(bus, kDisplayList, {
        dl_cmd(0xB4000000, kOtherData),  // G_RDPHALF_1: the new ucode's data segment
        dl_cmd(0xAF0007FF, 0x00230000),  // G_LOAD_UCODE, data size 0x800
        rect,
        kEndDl,
    });
    HleRun run = run_hle(*n64);
    CHECK(run.result.ok);
    CHECK(run.trace == std::vector<std::uint64_t>{rect});

    write_bytes(bus, kOtherData + 0x130, "RSP Gfx ucode F3DEX         1.23 Yoshitaka Yasumoto Nintendo.");
    run = run_hle(*n64);
    CHECK_FALSE(run.result.ok);
    CHECK(run.trace.empty());
}

TEST_CASE("G_SETOTHERMODE_H and _L update the mode and send Set Other Modes") {
    const auto n64 = N64::create(counting_rom("HLE OTHERMODE"));
    REQUIRE(n64);
    auto& bus = n64->bus();
    write_dl(bus, kDisplayList, {
        dl_cmd(0xBA001701, 0x00800000),  // H: shift 23, length 1 (F3DEX's initial H is 0x080CFF)
        dl_cmd(0xB900031D, 0x0F0A4000),  // L: shift 3, length 29
        dl_cmd(0xBA000C02, 0x00002000),  // H: shift 12, length 2
        kEndDl,
    });
    const HleRun run = run_hle(*n64);
    CHECK(run.trace == std::vector<std::uint64_t>{0xEF880CFF00000000ULL, 0xEF880CFF0F0A4000ULL,
                                                   0xEF882CFF0F0A4000ULL});
}
