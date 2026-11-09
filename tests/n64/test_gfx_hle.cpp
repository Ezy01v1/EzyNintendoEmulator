#include <doctest/doctest.h>

#include <array>
#include <cstdint>
#include <initializer_list>
#include <string>
#include <vector>

#include "n64/gfx_hle/f3dex.hpp"
#include "n64/gfx_hle/fixed.hpp"
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

namespace {

constexpr std::int32_t fx(double value) { return static_cast<std::int32_t>(value * 65536.0); }

// A GBI Mtx in RDRAM: the 16 integer halves, then the 16 fractional halves, row by row.
void write_mtx(Bus& bus, std::uint32_t address, const std::array<std::int32_t, 16>& m) {
    for (unsigned i = 0; i < 16; ++i) {
        const auto value = static_cast<std::uint32_t>(m[i]);
        bus.write8(address + i * 2, static_cast<std::uint8_t>(value >> 24));
        bus.write8(address + i * 2 + 1, static_cast<std::uint8_t>(value >> 16));
        bus.write8(address + 32 + i * 2, static_cast<std::uint8_t>(value >> 8));
        bus.write8(address + 32 + i * 2 + 1, static_cast<std::uint8_t>(value));
    }
}

constexpr std::array<std::int32_t, 16> kIdentity = {fx(1), 0, 0, 0, 0, fx(1), 0, 0, 0, 0, fx(1), 0, 0, 0, 0, fx(1)};

}  // namespace

TEST_CASE("load_mtx reads integer halves then fractional halves") {
    const auto n64 = N64::create(counting_rom("HLE MTX"));
    REQUIRE(n64);
    auto& bus = n64->bus();
    std::array<std::int32_t, 16> m{};
    for (unsigned i = 0; i < 16; ++i) m[i] = fx(i * 1.25 - 7.5);
    write_mtx(bus, 0x300000, m);
    CHECK(load_mtx(bus.rdram(), 0x300000).m == m);
}

TEST_CASE("mul_mtx follows the RSP's VMUDL/VMADM/VMADN/VMADH products") {
    Mtx a{kIdentity}, b{};
    for (unsigned i = 0; i < 16; ++i) b.m[i] = fx(i * 0.5 - 3.25);
    CHECK(mul_mtx(a, b).m == b.m);  // identity
    CHECK(mul_mtx(b, a).m == b.m);

    Mtx c{}, d{};
    c.m[0] = fx(1.5), d.m[0] = fx(2.25);  // exact: 3.375
    c.m[5] = fx(-1.5), d.m[5] = fx(0.5);  // exact: -0.75
    c.m[10] = 1, d.m[10] = 1;             // 2^-16 * 2^-16: VMUDL drops it
    c.m[15] = 0x00018001, d.m[15] = 0x00008001;  // 1.50002 * 0.50002, low bits of frac*frac dropped
    const Mtx e = mul_mtx(c, d);
    CHECK(e.m[0] == fx(3.375));
    CHECK(e.m[5] == fx(-0.75));
    CHECK(e.m[10] == 0);
    // (0x8001 * 0x8001) >> 16 = 0x4001; 1 * 0x8001 = 0x8001; 0x8001 * 0 (int part of d) = 0
    CHECK(e.m[15] == 0x4001 + 0x8001);
}

TEST_CASE("transform multiplies an integer vertex by a matrix exactly") {
    Mtx m{kIdentity};
    m.m[12] = fx(10.5), m.m[13] = fx(-2), m.m[14] = fx(0.25);  // translation row
    m.m[0] = fx(2);                                             // scale X
    const auto out = transform(m, 3, -4, 5);
    CHECK(out[0] == fx(16.5));
    CHECK(out[1] == fx(-6));
    CHECK(out[2] == fx(5.25));
    CHECK(out[3] == fx(1));
}

TEST_CASE("rsp_reciprocal is the VRCP result") {
    CHECK(rsp_reciprocal(0, false) == 0x7FFFFFFFU);
    CHECK(rsp_reciprocal(-32768, false) == 0xFFFF0000U);
    CHECK(rsp_reciprocal(1, false) == 0x7FFFC000U);
    CHECK(rsp_reciprocal(2, false) == 0x3FFFE000U);
}

namespace {

// A GBI Vtx: position, flag, texture coordinates and color (or normal).
void write_vtx(Bus& bus, std::uint32_t address, std::int16_t x, std::int16_t y, std::int16_t z, std::int16_t s = 0,
               std::int16_t t = 0, std::uint32_t rgba = 0xFFFFFFFF) {
    bus.write32(address, (static_cast<std::uint32_t>(static_cast<std::uint16_t>(x)) << 16) | static_cast<std::uint16_t>(y));
    bus.write32(address + 4, static_cast<std::uint32_t>(static_cast<std::uint16_t>(z)) << 16);
    bus.write32(address + 8, (static_cast<std::uint32_t>(static_cast<std::uint16_t>(s)) << 16) | static_cast<std::uint16_t>(t));
    bus.write32(address + 12, rgba);
}

constexpr std::uint32_t kMatrix = 0x300000, kVertices = 0x301000, kViewport = 0x302000, kLights = 0x303000;

// 320x240 viewport, the libultra way: scale and translation in quarter pixels.
void write_viewport(Bus& bus) {
    const std::int16_t vp[8] = {640, 480, 511, 0, 640, 480, 511, 0};
    for (unsigned i = 0; i < 8; i += 2) {
        bus.write32(kViewport + i * 2, (static_cast<std::uint32_t>(static_cast<std::uint16_t>(vp[i])) << 16) |
                                           static_cast<std::uint16_t>(vp[i + 1]));
    }
}

// Display list prologue: identity projection and modelview, the viewport.
std::vector<std::uint64_t> camera_dl() {
    return {
        dl_cmd(0x01030040, kMatrix),  // G_MTX projection | load
        dl_cmd(0x01020040, kMatrix),  // G_MTX modelview | load
        dl_cmd(0x03800010, kViewport),  // G_MOVEMEM viewport
    };
}

void write_dl(Bus& bus, std::uint32_t address, const std::vector<std::uint64_t>& commands) {
    for (const auto command : commands) bus.write64(address, command), address += 8;
}

// G_VTX (F3DEX): first index * 2 in bits 23:16, count in bits 15:10, byte length - 1 below.
constexpr std::uint64_t g_vtx(unsigned first, unsigned count, std::uint32_t address) {
    return dl_cmd(0x04000000U | ((first * 2) << 16) | (count << 10) | (count * 16 - 1), address);
}

}  // namespace

TEST_CASE("G_VTX transforms through modelview, projection and viewport") {
    const auto n64 = N64::create(counting_rom("HLE VTX"));
    REQUIRE(n64);
    auto& bus = n64->bus();
    write_mtx(bus, kMatrix, kIdentity);
    write_viewport(bus);
    write_vtx(bus, kVertices, 0, 0, 0);
    write_vtx(bus, kVertices + 16, 0, 1, 0);   // top of the view volume
    write_vtx(bus, kVertices + 32, -1, 0, 0);  // left edge
    auto dl = camera_dl();
    dl.push_back(g_vtx(0, 3, kVertices));
    dl.push_back(kEndDl);
    write_dl(bus, kDisplayList, dl);
    F3dex f3dex;
    REQUIRE(f3dex.run_task(graphics_task(), GfxUcode::F3dex095, bus.rdram(), bus.rdp(), bus.mi()).ok);
    CHECK(f3dex.vertex(0).x == 160 * 4);
    CHECK(f3dex.vertex(0).y == 120 * 4);
    CHECK(f3dex.vertex(1).y == 0);       // screen Y grows downward
    CHECK(f3dex.vertex(2).x == 0);
    CHECK(f3dex.vertex(0).w == fx(1));
}

TEST_CASE("G_VTX lighting with one directional light and ambient") {
    const auto n64 = N64::create(counting_rom("HLE LIGHT"));
    REQUIRE(n64);
    auto& bus = n64->bus();
    write_mtx(bus, kMatrix, kIdentity);
    write_viewport(bus);
    // Light 1: color (0x80, 0x40, 0x20), direction +Z (s8 127). Ambient: (0x10, 0x10, 0x10).
    bus.write32(kLights, 0x80402000), bus.write32(kLights + 4, 0x80402000);
    bus.write32(kLights + 8, 0x00007F00), bus.write32(kLights + 12, 0);
    bus.write32(kLights + 16, 0x10101000), bus.write32(kLights + 20, 0x10101000);
    write_vtx(bus, kVertices, 0, 0, 0, 0, 0, 0x00007FFF);  // normal +Z, alpha 0xFF
    write_vtx(bus, kVertices + 16, 0, 0, 0, 0, 0, 0x000081FF);  // normal -Z: ambient only
    auto dl = camera_dl();
    dl.push_back(dl_cmd(0xBC000002, 0x80000040));  // G_MW_NUMLIGHT: one light (NUML(1) = 2 * 32)
    dl.push_back(dl_cmd(0x03860010, kLights));      // G_MV_L0
    dl.push_back(dl_cmd(0x03880010, kLights + 16));  // G_MV_L1: the ambient for one light
    dl.push_back(dl_cmd(0xB7000000, 0x00020000));    // G_SETGEOMETRYMODE G_LIGHTING
    dl.push_back(g_vtx(0, 2, kVertices));
    dl.push_back(kEndDl);
    write_dl(bus, kDisplayList, dl);
    F3dex f3dex;
    REQUIRE(f3dex.run_task(graphics_task(), GfxUcode::F3dex095, bus.rdram(), bus.rdp(), bus.mi()).ok);
    const auto& lit = f3dex.vertex(0);
    CHECK(lit.r >= 0x8E);  // ambient + nearly all of the light (127/128)
    CHECK(lit.r <= 0x90);
    CHECK(lit.g >= 0x4E);
    CHECK(lit.b >= 0x2E);
    CHECK(lit.a == 0xFF);
    const auto& dark = f3dex.vertex(1);
    CHECK(dark.r == 0x10);
    CHECK(dark.g == 0x10);
    CHECK(dark.b == 0x10);
}

TEST_CASE("G_VTX sets clip flags outside the guard band and behind the camera") {
    const auto n64 = N64::create(counting_rom("HLE CLIPFLAGS"));
    REQUIRE(n64);
    auto& bus = n64->bus();
    std::array<std::int32_t, 16> projection = kIdentity;
    projection[11] = fx(-1), projection[15] = 0, projection[14] = 0;  // w = -z: looking down -Z
    write_mtx(bus, kMatrix, projection);
    write_mtx(bus, kMatrix + 64, kIdentity);
    write_viewport(bus);
    write_vtx(bus, kVertices, 0, 0, -10);       // in front, centered
    write_vtx(bus, kVertices + 16, 100, 0, -10);  // far to the right: x/w = 10 > clip ratio 2
    write_vtx(bus, kVertices + 32, 0, 0, 10);     // behind the camera: w < 0
    const std::vector<std::uint64_t> dl = {
        dl_cmd(0x01030040, kMatrix), dl_cmd(0x01020040, kMatrix + 64), dl_cmd(0x03800010, kViewport),
        dl_cmd(0xBC000004, 2),  // G_MW_CLIP ratio 2 (written to all four planes by gSPClipRatio)
        g_vtx(0, 3, kVertices), kEndDl,
    };
    write_dl(bus, kDisplayList, dl);
    F3dex f3dex;
    REQUIRE(f3dex.run_task(graphics_task(), GfxUcode::F3dex095, bus.rdram(), bus.rdp(), bus.mi()).ok);
    CHECK(f3dex.vertex(0).clip == 0);
    CHECK((f3dex.vertex(1).clip & F3dex::kClipPosX) != 0);
    CHECK((f3dex.vertex(2).clip & F3dex::kClipNear) != 0);
}

TEST_CASE("G_VTX with W = 0 does not divide by zero") {
    const auto n64 = N64::create(counting_rom("HLE W0"));
    REQUIRE(n64);
    auto& bus = n64->bus();
    std::array<std::int32_t, 16> projection = kIdentity;
    projection[15] = 0;  // w = 0 for every vertex
    write_mtx(bus, kMatrix, projection);
    write_mtx(bus, kMatrix + 64, kIdentity);
    write_viewport(bus);
    write_vtx(bus, kVertices, 5, 5, 5);
    write_dl(bus, kDisplayList, {dl_cmd(0x01030040, kMatrix), dl_cmd(0x01020040, kMatrix + 64),
                                 dl_cmd(0x03800010, kViewport), g_vtx(0, 1, kVertices), kEndDl});
    F3dex f3dex;
    CHECK(f3dex.run_task(graphics_task(), GfxUcode::F3dex095, bus.rdram(), bus.rdp(), bus.mi()).ok);
    CHECK((f3dex.vertex(0).clip & F3dex::kClipNear) != 0);
}

namespace {

// Identity camera with three vertices at screen (160, 0), (0, 240) and (320, 240) pixels.
std::vector<std::uint64_t> triangle_scene(Bus& bus, std::uint32_t geometry_mode) {
    write_mtx(bus, kMatrix, kIdentity);
    write_viewport(bus);
    write_vtx(bus, kVertices, 0, 1, 0, 0, 0, 0xFF000080);
    write_vtx(bus, kVertices + 16, -1, -1, 0, 0, 0, 0x00FF0080);
    write_vtx(bus, kVertices + 32, 1, -1, 0, 0, 0, 0x0000FF80);
    auto dl = camera_dl();
    dl.push_back(dl_cmd(0xB7000000, geometry_mode));
    dl.push_back(g_vtx(0, 3, kVertices));
    return dl;
}

}  // namespace

TEST_CASE("F3DEX triangle edge coefficients for a known triangle") {
    const auto n64 = N64::create(counting_rom("HLE TRI"));
    REQUIRE(n64);
    auto& bus = n64->bus();
    auto dl = triangle_scene(bus, 0);
    dl.push_back(dl_cmd(0xBF000000, 0x00000204));  // G_TRI1 v0, v1, v2 (indices x 2)
    dl.push_back(kEndDl);
    write_dl(bus, kDisplayList, dl);
    const HleRun run = run_hle(*n64);
    REQUIRE(run.trace.size() == 4);
    CHECK(run.trace[0] == ((0xC8ULL << 56) | (960ULL << 32) | (960ULL << 16) | 0));  // right major, YL, YM, YH
    CHECK(run.trace[1] == 0);                                // XL = 0 at YM, flat bottom: no slope
    CHECK(run.trace[2] == ((0x00A00000ULL << 32) | 0xAAAA));  // XH = 160, DxHDy = 2/3
    CHECK(run.trace[3] == ((0x00A00000ULL << 32) | 0xFFFF5556));  // XM = 160, DxMDy = -2/3
}

TEST_CASE("F3DEX culls back faces when G_CULL_BACK is set") {
    const auto n64 = N64::create(counting_rom("HLE CULL"));
    REQUIRE(n64);
    auto& bus = n64->bus();
    auto dl = triangle_scene(bus, 0x2000);       // G_CULL_BACK
    dl.push_back(dl_cmd(0xBF000000, 0x00000204));  // counter-clockwise on screen: front
    dl.push_back(dl_cmd(0xBF000000, 0x00000402));  // the same triangle wound the other way: back
    dl.push_back(kEndDl);
    write_dl(bus, kDisplayList, dl);
    const HleRun both = run_hle(*n64);
    // Only the front one is drawn: the same words as drawing it alone without culling.
    auto alone = triangle_scene(bus, 0);
    alone.push_back(dl_cmd(0xBF000000, 0x00000204));
    alone.push_back(kEndDl);
    write_dl(bus, kDisplayList, alone);
    CHECK(both.trace == run_hle(*n64).trace);
}

TEST_CASE("G_TRI2 emits two triangles in order") {
    const auto n64 = N64::create(counting_rom("HLE TRI2"));
    REQUIRE(n64);
    auto& bus = n64->bus();
    auto dl = triangle_scene(bus, 0x4);           // G_SHADE: 4 edge words + 8 shade words each
    dl.push_back(dl_cmd(0xB1000204, 0x00000204));  // G_TRI2: the same triangle twice
    dl.push_back(kEndDl);
    write_dl(bus, kDisplayList, dl);
    const HleRun run = run_hle(*n64);
    REQUIRE(run.trace.size() == 24);
    CHECK((run.trace[0] >> 56) == 0xCC);
    CHECK((run.trace[12] >> 56) == 0xCC);
    CHECK((run.trace[4] >> 48) == 0x00FF);  // shade R at the top vertex = 0xFF
}

TEST_CASE("G_QUAD draws (v0, v1, v2) and (v0, v2, v3)") {
    const auto n64 = N64::create(counting_rom("HLE QUAD"));
    REQUIRE(n64);
    auto& bus = n64->bus();
    auto dl = triangle_scene(bus, 0);
    write_vtx(bus, kVertices + 48, 0, -1, 0);  // v3, bottom middle
    dl.back() = g_vtx(0, 4, kVertices);
    dl.push_back(dl_cmd(0xB5000000, 0x00020406));  // G_QUAD v0, v1, v2, v3
    dl.push_back(kEndDl);
    write_dl(bus, kDisplayList, dl);
    const HleRun run = run_hle(*n64);
    REQUIRE(run.trace.size() == 8);
    // First triangle: v0 (160, 0) to v1 (0, 240) and v2 (320, 240), as in the G_TRI1 test.
    CHECK(run.trace[0] == ((0xC8ULL << 56) | (960ULL << 32) | (960ULL << 16) | 0));
    // Second: v0, v2 and v3 (160, 240): the right half.
    CHECK(((run.trace[4] >> 16) & 0x3FFF) == 960);
}

namespace {

// Perspective camera looking down -Z: w = -z, and the screen is the 320x240 viewport.
std::vector<std::uint64_t> perspective_scene(Bus& bus) {
    std::array<std::int32_t, 16> projection = kIdentity;
    projection[11] = fx(-1), projection[10] = fx(-1), projection[14] = fx(-2), projection[15] = 0;  // w = -z, z' = -z - 2
    write_mtx(bus, kMatrix, projection);
    write_mtx(bus, kMatrix + 64, kIdentity);
    write_viewport(bus);
    return {dl_cmd(0x01030040, kMatrix), dl_cmd(0x01020040, kMatrix + 64), dl_cmd(0x03800010, kViewport),
            dl_cmd(0xBC000004, 2)};
}

unsigned count_triangles(const std::vector<std::uint64_t>& trace) {
    unsigned n = 0;
    for (std::size_t i = 0; i < trace.size();) {
        const unsigned op = static_cast<unsigned>(trace[i] >> 56) & 63;
        if (op >= 0x08 && op <= 0x0F) ++n;
        i += Rdp::words_in_command(op);
    }
    return n;
}

}  // namespace

TEST_CASE("Clipping a triangle that crosses the near plane yields a fan in front of it") {
    const auto n64 = N64::create(counting_rom("HLE NEARCLIP"));
    REQUIRE(n64);
    auto& bus = n64->bus();
    auto dl = perspective_scene(bus);
    write_vtx(bus, kVertices, -2, -1, -4);
    write_vtx(bus, kVertices + 16, 2, -1, -4);
    write_vtx(bus, kVertices + 32, 0, -1, 4);  // behind the camera
    dl.push_back(g_vtx(0, 3, kVertices));
    dl.push_back(dl_cmd(0xBF000000, 0x00000204));
    dl.push_back(kEndDl);
    write_dl(bus, kDisplayList, dl);
    const HleRun run = run_hle(*n64, GfxUcode::F3dex095);
    REQUIRE(count_triangles(run.trace) == 2);  // the quad left in front of the near plane
    for (std::size_t i = 0; i < run.trace.size(); i += 4) {
        const auto yh = static_cast<std::int16_t>((run.trace[i] & 0x3FFF) << 2) >> 2;
        const auto yl = static_cast<std::int16_t>(((run.trace[i] >> 32) & 0x3FFF) << 2) >> 2;
        CHECK(yh >= -8192);  // finite, within the RDP's coordinate range
        CHECK(yl <= 8191);
    }
}

TEST_CASE("F3DLX does not clip against the near plane") {
    const auto n64 = N64::create(counting_rom("HLE NONEAR"));
    REQUIRE(n64);
    auto& bus = n64->bus();
    auto dl = perspective_scene(bus);
    write_vtx(bus, kVertices, -2, -1, -4);
    write_vtx(bus, kVertices + 16, 2, -1, -4);
    write_vtx(bus, kVertices + 32, 0, -1, 4);
    dl.push_back(g_vtx(0, 3, kVertices));
    dl.push_back(dl_cmd(0xBF000000, 0x00000204));
    dl.push_back(kEndDl);
    write_dl(bus, kDisplayList, dl);
    CHECK(count_triangles(run_hle(*n64, GfxUcode::F3dlx095).trace) == 0);  // a vertex behind: dropped, not clipped
}

TEST_CASE("Triangles past the guard band are clipped to it") {
    const auto n64 = N64::create(counting_rom("HLE GUARD"));
    REQUIRE(n64);
    auto& bus = n64->bus();
    auto dl = perspective_scene(bus);
    write_vtx(bus, kVertices, -1, -1, -2);
    write_vtx(bus, kVertices + 16, 1000, -1, -2);  // x/w = 500, far past ratio 2
    write_vtx(bus, kVertices + 32, 0, 1, -2);
    dl.push_back(g_vtx(0, 3, kVertices));
    dl.push_back(dl_cmd(0xBF000000, 0x00000204));
    dl.push_back(kEndDl);
    write_dl(bus, kDisplayList, dl);
    const HleRun run = run_hle(*n64);
    REQUIRE(count_triangles(run.trace) >= 1);
    // No edge X beyond the guard band (ratio 2: 160 + 2 * 160 = 480 pixels).
    for (std::size_t i = 0; i < run.trace.size(); i += 4) {
        for (unsigned e = 1; e <= 3; ++e) {
            const auto x = static_cast<std::int32_t>(run.trace[i + e] >> 32) / 65536.0;
            CHECK(x <= 490.0);  // edges are evaluated at the start scanline: a little past the vertex
            CHECK(x >= -170.0);
        }
    }
}

TEST_CASE("G_CULLDL skips the rest of the list when all vertices are off one plane") {
    const auto n64 = N64::create(counting_rom("HLE CULLDL"));
    REQUIRE(n64);
    auto& bus = n64->bus();
    auto dl = perspective_scene(bus);
    for (unsigned i = 0; i < 3; ++i) write_vtx(bus, kVertices + i * 16, static_cast<std::int16_t>(100 + i), 0, -2);
    const std::uint64_t rect = rdp_rectangle(0xF6, 4, 4, 0, 0);
    dl.push_back(g_vtx(0, 3, kVertices));
    dl.push_back(dl_cmd(0xBE000000, 2 * 2));  // G_CULLDL vertices 0..2: all far right
    dl.push_back(rect);
    dl.push_back(kEndDl);
    write_dl(bus, kDisplayList, dl);
    CHECK(run_hle(*n64).trace.empty());
}

TEST_CASE("A save state taken while a HLE task is pending finishes it once after loading") {
    const auto n64 = N64::create(counting_rom("HLE STATE"));
    REQUIRE(n64);
    auto& bus = n64->bus();
    n64->set_hle_gfx(true);
    write_ucode_data(bus, "F3DEX         0.95");
    bus.write64(kDisplayList, kEndDl);
    write_os_task(bus, graphics_task());
    bus.write32(kMiMask, 2);  // unmask SP
    start_rsp(bus);
    n64->step();  // the task has started; its end is scheduled
    REQUIRE((bus.read32(kMiIntr) & 1) == 0);
    const auto state = n64->save_state();

    for (unsigned i = 0; i < 3000; ++i) n64->step();
    REQUIRE((bus.read32(kMiIntr) & 1) != 0);  // finished
    bus.write32(kSpStatus, 1U << 3);          // acknowledge the SP interrupt

    REQUIRE(n64->load_state(state));
    for (unsigned i = 0; i < 3000; ++i) n64->step();
    CHECK((bus.read32(kMiIntr) & 1) != 0);  // finished again after loading
    CHECK((bus.read32(kSpStatus) & (kSpHalt | kSpBroke | kSpSignal2)) == (kSpHalt | kSpBroke | kSpSignal2));
    bus.write32(kSpStatus, 1U << 3);
    for (unsigned i = 0; i < 3000; ++i) n64->step();
    CHECK((bus.read32(kMiIntr) & 1) == 0);  // and only once
}

TEST_CASE("A HLE task's Sync Full raises DP when the task ends, not when it starts") {
    const auto n64 = N64::create(counting_rom("HLE SYNCFULL"));
    REQUIRE(n64);
    auto& bus = n64->bus();
    n64->set_hle_gfx(true);
    write_ucode_data(bus, "F3DEX         0.95");
    write_dl(bus, kDisplayList, {0xE900000000000000ULL, kEndDl});  // gDPFullSync
    write_os_task(bus, graphics_task());
    bus.write32(kMiMask, 2 | (2U << 10));  // unmask SP and DP
    start_rsp(bus);
    n64->step();
    CHECK((bus.read32(kMiIntr) & (1U << 5)) == 0);  // DP not yet
    const auto state = n64->save_state();
    for (unsigned i = 0; i < 5000; ++i) n64->step();
    CHECK((bus.read32(kMiIntr) & (1U << 5)) != 0);
    CHECK((bus.read32(kMiIntr) & 1) != 0);

    REQUIRE(n64->load_state(state));  // the pending Sync Full survives a save state
    CHECK((bus.read32(kMiIntr) & (1U << 5)) == 0);
    for (unsigned i = 0; i < 5000; ++i) n64->step();
    CHECK((bus.read32(kMiIntr) & (1U << 5)) != 0);
}

TEST_CASE("A new HLE task first delivers the previous task's pending Sync Full") {
    const auto n64 = N64::create(counting_rom("HLE TWO TASKS"));
    REQUIRE(n64);
    auto& bus = n64->bus();
    n64->set_hle_gfx(true);
    write_ucode_data(bus, "F3DEX         0.95");
    write_dl(bus, kDisplayList, {0xE900000000000000ULL, kEndDl});  // gDPFullSync
    write_os_task(bus, graphics_task());
    bus.write32(kMiMask, 2 | (2U << 10));
    start_rsp(bus);
    n64->step();
    REQUIRE((bus.read32(kMiIntr) & (1U << 5)) == 0);
    // The game starts the next task as soon as SP says done, before the first one's DP.
    bus.write32(kSpStatus, 1 | 4 | 0x100);
    n64->step();
    CHECK((bus.read32(kMiIntr) & (1U << 5)) != 0);  // the first task's Sync Full was not lost
}

TEST_CASE("F3DEX state does not carry over from one task to the next") {
    const auto n64 = N64::create(counting_rom("HLE STATELESS"));
    REQUIRE(n64);
    auto& bus = n64->bus();
    auto dl = triangle_scene(bus, 0);
    dl.insert(dl.begin(), dl_cmd(0xBB000001, 0xFFFFFFFF));  // G_TEXTURE on
    dl.push_back(kEndDl);
    write_dl(bus, kDisplayList, dl);
    F3dex f3dex;
    f3dex.run_task(graphics_task(), GfxUcode::F3dex095, bus.rdram(), bus.rdp(), bus.mi());

    // Second task: same scene without G_TEXTURE: the triangle must not be textured.
    auto second = triangle_scene(bus, 0);
    second.push_back(dl_cmd(0xBF000000, 0x00000204));
    second.push_back(kEndDl);
    write_dl(bus, kDisplayList, second);
    std::vector<std::uint64_t> trace;
    bus.rdp().set_trace(&trace);
    f3dex.run_task(graphics_task(), GfxUcode::F3dex095, bus.rdram(), bus.rdp(), bus.mi());
    bus.rdp().set_trace(nullptr);
    REQUIRE_FALSE(trace.empty());
    CHECK((trace[0] >> 56) == 0xC8);  // not 0xCA
}
