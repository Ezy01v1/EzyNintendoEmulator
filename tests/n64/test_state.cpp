#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <initializer_list>
#include <memory>
#include <string>
#include <vector>

#include "common/state.hpp"
#include "mips_asm.hpp"
#include "n64/n64.hpp"

using namespace ezy::n64;

namespace {

// A ROM whose "IPL3" (at 0x40, run from SP DMEM at 0xA4000040) counts in t1 and stores
// the count to RDRAM forever.
std::vector<std::uint8_t> counting_rom(const std::string& title) {
    std::vector<std::uint8_t> rom(0x2000, 0);
    rom[0] = 0x80, rom[1] = 0x37, rom[2] = 0x12, rom[3] = 0x40;
    for (std::size_t i = 0; i < title.size() && i < 20; ++i) rom[0x20 + i] = static_cast<std::uint8_t>(title[i]);
    rom[0x3E] = 'E';
    using namespace mips;
    constexpr int t0 = 8, t1 = 9;
    const std::uint32_t program[] = {lui(t0, 0x8000), addiu(t1, t1, 1), sw(t1, t0, 0x100), j(0x02, 0xA4000044), nop()};
    std::size_t at = 0x40;
    for (const std::uint32_t w : program) {
        rom[at] = static_cast<std::uint8_t>(w >> 24);
        rom[at + 1] = static_cast<std::uint8_t>(w >> 16);
        rom[at + 2] = static_cast<std::uint8_t>(w >> 8);
        rom[at + 3] = static_cast<std::uint8_t>(w);
        at += 4;
    }
    return rom;
}

constexpr std::uint32_t vector_op(int function, int dest, int vs, int vt, int element = 0) {
    return mips::r(0x12, 16 | element, vt, vs, dest, function);
}

constexpr std::uint32_t cop2_transfer(int operation, int scalar, int vector, int element = 0) {
    return mips::r(0x12, operation, scalar, vector, 0, 0) | (static_cast<std::uint32_t>(element) << 7);
}

constexpr std::uint32_t vector_mem(bool store, int operation, int reg, int element, int offset) {
    const int immediate = (operation << 11) | (element << 7) | (offset & 127);
    return mips::i(store ? 0x3A : 0x32, 0, reg, immediate);
}

void write_rsp_program(Bus& bus, std::initializer_list<std::uint32_t> program) {
    std::uint32_t address = 0x04001000;
    for (const auto word : program) {
        bus.write32(address, word);
        address += 4;
    }
}

constexpr std::uint32_t kDpcStart = 0x04100000, kDpcEnd = 0x04100004, kDpcCurrent = 0x04100008,
                        kDpcStatus = 0x0410000C;
constexpr std::uint64_t kFillMode = 0x2F30000000000000ULL, kSyncFull = 0x2900000000000000ULL;
constexpr std::uint64_t kPointMode = (0x2FULL << 56) | (6ULL << 41) | (3ULL << 38);  // G_TC_FILT, RGB dither off
constexpr std::uint64_t kFilterMode = kPointMode | (1ULL << 45);

constexpr std::uint64_t rdp_rectangle(std::uint64_t opcode, unsigned hi_x, unsigned hi_y, unsigned lo_x, unsigned lo_y) {
    // Each coordinate is encoded in quarter pixels.
    return (opcode << 56) | (std::uint64_t{hi_x * 4} << 44) | (std::uint64_t{hi_y * 4} << 32) |
           (std::uint64_t{lo_x * 4} << 12) | (lo_y * 4);
}

constexpr std::uint64_t rdp_triangle_header(bool major_left, int yh, int ym, int yl) {
    return 0x0800000000000000ULL | (std::uint64_t{major_left} << 55) |
           (static_cast<std::uint64_t>(yl & 0x3FFF) << 32) |
           (static_cast<std::uint64_t>(ym & 0x3FFF) << 16) | static_cast<std::uint64_t>(yh & 0x3FFF);
}

constexpr std::uint64_t rdp_edge(std::int32_t x, std::int32_t slope) {
    return (std::uint64_t{static_cast<std::uint32_t>(x)} << 32) | static_cast<std::uint32_t>(slope);
}

constexpr std::uint64_t rdp_tile(unsigned format, unsigned size, unsigned tile, unsigned stride,
                                 unsigned base, unsigned sampler = 0) {
    return 0x3500000000000000ULL | (std::uint64_t{format} << 53) | (std::uint64_t{size} << 51) |
           (std::uint64_t{stride} << 41) | (std::uint64_t{base} << 32) | (std::uint64_t{tile} << 24) | sampler;
}

constexpr std::uint64_t rdp_texture_coords(int s, int t, int ds, int dt) {
    return (std::uint64_t{static_cast<std::uint16_t>(s)} << 48) |
           (std::uint64_t{static_cast<std::uint16_t>(t)} << 32) |
           (std::uint64_t{static_cast<std::uint16_t>(ds)} << 16) | static_cast<std::uint16_t>(dt);
}

constexpr std::uint64_t rdp_stw_halves(int s, int t, int w = 0) {
    return (std::uint64_t{static_cast<std::uint16_t>(s)} << 48) |
           (std::uint64_t{static_cast<std::uint16_t>(t)} << 32) |
           (std::uint64_t{static_cast<std::uint16_t>(w)} << 16);
}

}  // namespace

TEST_CASE("the console boots the IPL3 code and reaches VBlank") {
    const auto n64 = N64::create(counting_rom("STATE TEST"));
    REQUIRE(n64);
    n64->run_frame();
    CHECK(n64->cpu().gpr(9) > 1000);
    // The last count stored to RDRAM is the register or, mid-loop, one behind it.
    const std::uint32_t stored = n64->bus().read32(0x100);
    const auto count = static_cast<std::uint32_t>(n64->cpu().gpr(9));
    CHECK(count - stored <= 1);
}

TEST_CASE("N64 runs a DMA-loaded scalar program and takes its SP interrupt on the CPU") {
    using namespace mips;
    const auto n64 = N64::create(counting_rom("RSP TEST"));
    REQUIRE(n64);
    auto& bus = n64->bus();
    const std::uint32_t program[] = {
        addiu(0, 0, 50), addiu(1, 0, 42), beq(1, 1, 2), addiu(1, 1, -1),
        addiu(1, 1, -10), sw(1, 0, 0xFFF), lw(2, 0, 0xFFF), sw(2, 0, 0x100),
        special(0, 0, 0, 0, 0x0D), sw(0, 0, 0x100),
    };
    std::uint32_t address = 0x1000;
    for (const auto word : program) {
        bus.write32(address, word);
        address += 4;
    }
    bus.write32(0x04040000, 0x1000);  // IMEM
    bus.write32(0x04040004, 0x1000);
    bus.write32(0x04040008, sizeof(program) - 1);
    for (unsigned i = 0; i < 100 && bus.read32(0x04040018); ++i) n64->step();
    REQUIRE(bus.read32(0x04040018) == 0);
    CHECK(bus.read32(0x04001000) == program[0]);

    bus.write32(0x0430000C, 2);  // enable the SP interrupt in MI
    bus.write32(0x2000, sw(2, 1, 0x10));
    n64->cpu().set_gpr(1, 0xFFFFFFFFA4040000ULL);
    n64->cpu().set_gpr(2, 0x101);  // clear HALT and enable interrupt on BREAK
    n64->cpu().set_pc(0xFFFFFFFF80002000ULL);
    n64->cpu().cop0().raw(Cop0::kStatus) = 0x401;  // IE and IM2
    for (unsigned i = 0; i < 100 && n64->cpu().pc() != 0xFFFFFFFF80000180ULL; ++i) n64->step();
    CHECK(n64->cpu().pc() == 0xFFFFFFFF80000180ULL);
    CHECK((n64->cpu().cop0().cause() & 0x47C) == 0x400);
    CHECK(bus.read32(0x04300008) == 1);
    CHECK(bus.read32(0x04040010) == 0x43);
    CHECK(bus.read32(0x04080000) == 0x24);
    CHECK(bus.read32(0x04000100) == 41);
    CHECK(bus.read8(0x04000002) == 41);
    bus.write32(0x04040010, 8);
    CHECK_FALSE(bus.interrupt_line());
}

TEST_CASE("N64 save state preserves an RSP delay slot and an in-flight SP DMA") {
    using namespace mips;
    const auto n64 = N64::create(counting_rom("RSP STATE TEST"));
    REQUIRE(n64);
    auto& bus = n64->bus();
    bus.write32(0x04001000, j(0x02, 0x10));
    bus.write32(0x04001004, addiu(1, 0, 17));
    bus.write32(0x04001008, addiu(1, 0, 99));
    bus.write32(0x04001010, sw(1, 0, 0x200));
    bus.write32(0x04001014, special(0, 0, 0, 0, 0x0D));
    bus.write32(0x04040010, 0x41);  // single-step the jump
    n64->step();
    REQUIRE(bus.read32(0x04080000) == 4);
    REQUIRE(bus.read32(0x04040010) == 0x21);
    bus.write32(0x1800, 0xDEADBEEF);
    bus.write32(0x04040000, 0x300);
    bus.write32(0x04040004, 0x1800);
    bus.write32(0x04040008, 7);
    const auto state = n64->save_state();

    const auto resume = [&] {
        bus.write32(0x04040010, 0x21);  // clear single-step and HALT
        for (unsigned i = 0; i < 20; ++i) n64->step();
        CHECK(bus.read32(0x04000200) == 17);
        CHECK(bus.read32(0x04000300) == 0xDEADBEEF);
        CHECK(bus.read32(0x04040010) == 3);
    };
    resume();
    const auto end = n64->save_state();
    REQUIRE(n64->load_state(state));
    CHECK(bus.read32(0x04040018) == 1);
    resume();
    CHECK(n64->save_state() == end);
}

TEST_CASE("RSP vector arithmetic exposes carry, saturation and the raw accumulator") {
    const auto n64 = N64::create(counting_rom("VECTOR TEST"));
    REQUIRE(n64);
    auto& bus = n64->bus();
    bus.write64(0x04000100, 0xFFFF7FFF80000000ULL);
    bus.write32(0x04000110, 0x00010000);
    write_rsp_program(bus, {
        vector_mem(false, 4, 1, 0, 0x10),           // LQV v1 from 0x100
        vector_mem(false, 4, 2, 0, 0x11),           // LQV v2 from 0x110
        vector_op(0x14, 3, 1, 2, 8),                // VADDC v3, v1, v2[0]
        cop2_transfer(2, 3, 0),                     // CFC2 r3, VCO
        mips::sw(3, 0, 0x220),
        vector_op(0x10, 4, 1, 2, 8),                // VADD v4, v1, v2[0]
        vector_op(0x1D, 5, 0, 0, 10),               // VSAR v5, accumulator low
        vector_mem(true, 4, 4, 0, 0x20),            // SQV v4 to 0x200
        vector_mem(true, 4, 5, 0, 0x21),            // SQV v5 to 0x210
        cop2_transfer(2, 3, 0),
        mips::sw(3, 0, 0x224),
        vector_mem(false, 3, 6, 14, 0x20),          // LDV v6[14], truncates after two bytes
        vector_mem(true, 3, 6, 14, 0x30),           // SDV v6[14] wraps to element zero
        mips::special(0, 0, 0, 0, 0x0D),
    });
    bus.write32(0x04040010, 1);
    for (unsigned i = 0; i < 100; ++i) n64->step();
    CHECK(bus.read32(0x04040010) == 3);
    CHECK(bus.read32(0x04000220) == 1);
    CHECK(bus.read32(0x04000224) == 0);
    CHECK(bus.read64(0x04000200) == 0x00017FFF80010001ULL);
    CHECK(bus.read64(0x04000210) == 0x0001800080010001ULL);
    CHECK(bus.read64(0x04000180) == 0xFFFF000000000000ULL);
}

TEST_CASE("N64 save state preserves the RSP vector registers and pending reciprocal half") {
    const auto n64 = N64::create(counting_rom("VECTOR STATE TEST"));
    REQUIRE(n64);
    auto& bus = n64->bus();
    write_rsp_program(bus, {
        mips::addiu(1, 0, 1),
        cop2_transfer(4, 1, 1),                     // MTC2 r1, v1[0]
        cop2_transfer(4, 1, 2, 6),                  // MTC2 r1, v2[6]
        vector_op(0x32, 2, 0, 1),                   // VRCPH captures 0x0001 as the high half
        mips::special(0, 0, 0, 0, 0x0D),
        vector_op(0x31, 2, 1, 0),                   // VRCPL v2[1], v0[0]
        vector_mem(true, 4, 2, 0, 0x20),
        vector_op(0x2C, 2, 2, 2),                   // VXOR clobbers v2 before restoring
        mips::special(0, 0, 0, 0, 0x0D),
    });
    bus.write32(0x04040010, 1);
    for (unsigned i = 0; i < 20; ++i) n64->step();
    REQUIRE(bus.read32(0x04080000) == 20);
    const auto state = n64->save_state();

    const auto resume = [&] {
        bus.write32(0x04040010, 5);  // clear HALT and BROKE
        for (unsigned i = 0; i < 20; ++i) n64->step();
        CHECK(bus.read64(0x04000200) == 0x00007FFF00000001ULL);
        CHECK(bus.read32(0x04040010) == 3);
    };
    resume();
    const auto end = n64->save_state();
    REQUIRE(n64->load_state(state));
    resume();
    CHECK(n64->save_state() == end);
}

TEST_CASE("RDP runs a frozen RDRAM list on resume and the VI displays the clipped fill") {
    const auto n64 = N64::create(counting_rom("RDP FILL"));
    REQUIRE(n64);
    auto& bus = n64->bus();
    const std::uint64_t commands[] = {
        0x3F10000400004000ULL,             // 16-bit color image at 0x4000, stride 5
        rdp_rectangle(0x2D, 1, 1, 4, 3),  // scissor limits are exclusive
        kFillMode,
        0x37000000F80107C1ULL,             // packed red and green pixels
        rdp_rectangle(0x36, 3, 2, 0, 0),  // fill lower-right pixel is inclusive
        kSyncFull,
    };
    std::uint32_t end = 0x1000;
    for (const auto command : commands) {
        bus.write64(end, command);
        end += 8;
    }

    bus.write32(0x0430000C, 1U << 11);  // unmask DP
    bus.write32(kDpcStatus, 8);         // freeze
    bus.write32(kDpcStart, 0xAB001003);
    bus.write32(kDpcStart, 0x2000);     // ignored while START_VALID is set
    CHECK(bus.read32(kDpcStart) == 0x1000);
    CHECK(bus.read32(kDpcCurrent) == 0);
    CHECK((bus.read32(kDpcStatus) & 0x400) != 0);
    bus.write32(kDpcEnd, end | 7);
    for (unsigned i = 0; i < 20; ++i) n64->step();
    CHECK(bus.read32(kDpcEnd) == end);
    CHECK(bus.read32(kDpcCurrent) == 0x1000);
    CHECK(bus.read32(kDpcStatus) == 0x82);
    CHECK_FALSE(bus.interrupt_line());
    CHECK(bus.read32(0x400C) == 0);

    bus.write32(kDpcStatus, 4);  // resume
    for (unsigned i = 0; i < 20; ++i) n64->step();
    CHECK(bus.read32(kDpcCurrent) == end);
    CHECK(bus.read32(kDpcStatus) == 0x80);
    CHECK(bus.interrupt_line());
    CHECK(bus.read32(0x04300008) == (1U << 5));
    bus.write32(0x04300000, 1U << 11);  // acknowledge DP through MI_MODE
    CHECK_FALSE(bus.interrupt_line());

    bus.write32(0x04400000, 2);      // VI reads RGBA5551
    bus.write32(0x04400004, 0x4000);
    bus.write32(0x04400008, 5);
    bus.write32(0x04400024, 5);      // H_VIDEO
    bus.write32(0x04400028, 6);      // V_VIDEO
    bus.write32(0x04400030, 1024);   // X_SCALE
    bus.write32(0x04400034, 16);     // Y_SCALE
    const auto image = n64->image();
    REQUIRE(image.width == 5);
    REQUIRE(image.height == 3);
    CHECK(image.argb == std::vector<std::uint32_t>{
        0xFF000000, 0xFF000000, 0xFF000000, 0xFF000000, 0xFF000000,
        0xFF000000, 0xFFFF0000, 0xFF00FF00, 0xFFFF0000, 0xFF000000,
        0xFF000000, 0xFF00FF00, 0xFFFF0000, 0xFF00FF00, 0xFF000000,
    });
}

TEST_CASE("RSP submits an RDP list through COP0 and XBUS wraps inside DMEM") {
    using namespace mips;
    const auto n64 = N64::create(counting_rom("RDP XBUS"));
    REQUIRE(n64);
    auto& bus = n64->bus();
    bus.write64(0x04000FF8, 0x3F18000300005000ULL);  // 32-bit color image, stride 4
    bus.write64(0x04000000, rdp_rectangle(0x2D, 0, 0, 4, 2));
    bus.write64(0x04000008, kFillMode);
    bus.write64(0x04000010, 0x37000000123456FFULL);
    bus.write64(0x04000018, rdp_rectangle(0x36, 2, 1, 2, 1));
    bus.write64(0x04000020, kSyncFull);
    bus.write64(0x04001000, 0xFFFFFFFFFFFFFFFFULL);  // XBUS must never read this IMEM.
    // Put the RSP program after the first IMEM word so it cannot mask an incorrect DMEM fetch.
    const std::uint32_t program[] = {
        addiu(1, 0, 2), mtc0(1, 11), addiu(1, 0, 0xFF8), mtc0(1, 8),
        addiu(1, 0, 0x1028), mtc0(1, 9),
        mfc0(2, 10), bne(1, 2, -2), nop(), special(0, 0, 0, 0, 0x0D),
    };
    std::uint32_t address = 0x04001008;
    for (const auto word : program) {
        bus.write32(address, word);
        address += 4;
    }
    bus.write32(0x04080000, 8);
    bus.write32(0x04040010, 1);
    for (unsigned i = 0; i < 100; ++i) n64->step();
    CHECK(bus.read32(kDpcCurrent) == 0x1028);
    CHECK(bus.read32(kDpcStatus) == 0x81);
    CHECK(bus.read32(0x04040010) == 3);
    CHECK(bus.read32(0x5014) == 0);
    CHECK(bus.read32(0x5018) == 0x123456FF);
    CHECK(bus.read32(0x501C) == 0);
    CHECK((bus.read32(0x04300008) & (1U << 5)) != 0);
}

TEST_CASE("N64 save state resumes an extended RDP list within a multiword command") {
    const auto n64 = N64::create(counting_rom("RDP STATE"));
    REQUIRE(n64);
    auto& bus = n64->bus();
    bus.write64(0x1000, 0x2500000000000000ULL);  // unsupported flipped rectangle header
    bus.write64(0x1008, kSyncFull);              // payload that must not decode as a command
    bus.write64(0x1010, kSyncFull);
    bus.write32(kDpcStart, 0x1000);
    bus.write32(kDpcEnd, 0x1008);
    for (unsigned i = 0; i < 10; ++i) n64->step();
    REQUIRE(bus.read32(kDpcCurrent) == 0x1008);
    bus.write32(kDpcEnd, 0x1018);
    const auto state = n64->save_state();

    // Run from a cached region so each CPU instruction advances the scheduler one cycle.
    const auto resume = [&] {
        n64->cpu().set_pc(0xFFFFFFFF80002000ULL);
        for (unsigned i = 0; i < 10 && bus.read32(kDpcCurrent) < 0x1010; ++i) n64->step();
        CHECK(bus.read32(kDpcCurrent) == 0x1010);
        CHECK((bus.read32(0x04300008) & (1U << 5)) == 0);
        for (unsigned i = 0; i < 10 && bus.read32(kDpcCurrent) < 0x1018; ++i) n64->step();
        CHECK(bus.read32(kDpcCurrent) == 0x1018);
        CHECK((bus.read32(0x04300008) & (1U << 5)) != 0);
    };
    resume();
    const auto end = n64->save_state();
    REQUIRE(n64->load_state(state));
    resume();
    CHECK(n64->save_state() == end);
}

TEST_CASE("RDP resumes a saved partial fill triangle and clips both minor edges") {
    const auto n64 = N64::create(counting_rom("RDP TRIANGLE STATE"));
    REQUIRE(n64);
    auto& bus = n64->bus();
    constexpr int pixel = 65536;  // X coordinates and slopes use 16 fractional bits.
    constexpr std::uint32_t list = 0x1000, framebuffer = 0x4000, fill_color = 0x123456FF;
    constexpr std::array commands{
        std::uint64_t{0x3F18000700004000ULL},      // RGBA8888, stride 8
        rdp_rectangle(0x2D, 2, 1, 4, 4),
        kFillMode,
        std::uint64_t{0x37000000123456FFULL},
        rdp_triangle_header(true, 1, 8, 16),       // y = 0.25, 2, 4
        rdp_edge(9 * pixel / 2, -7 * pixel / 4),  // lower minor edge starts at y = 2
        rdp_edge(pixel, 0),                        // major edge x = 1
        rdp_edge(pixel / 2, 2 * pixel),           // upper minor edge extrapolated to y = 0
        kSyncFull,
    };
    for (unsigned i = 0; i < commands.size(); ++i) bus.write64(list + 8 * i, commands[i]);

    bus.write32(kDpcStart, list);
    bus.write32(kDpcEnd, list + 6 * 8);
    for (unsigned i = 0; i < 20; ++i) n64->step();
    REQUIRE(bus.read32(kDpcCurrent) == list + 6 * 8);
    CHECK(bus.read32(framebuffer + (8 + 2) * 4) == 0);
    CHECK((bus.read32(0x04300008) & 0x20) == 0);
    // The first two triangle words have already left RDRAM and must survive this overwrite.
    bus.write64(list + 4 * 8, kSyncFull);
    bus.write64(list + 5 * 8, 0);
    const auto state = n64->save_state();

    const auto resume = [&] {
        bus.write32(kDpcEnd, list + 9 * 8);
        for (unsigned i = 0; i < 20; ++i) n64->step();
        CHECK(bus.read32(kDpcCurrent) == list + 9 * 8);
        CHECK((bus.read32(0x04300008) & 0x20) != 0);
        constexpr std::array rows{0U, 0b00011100U, 0b00011100U, 0b00000100U, 0U};
        for (unsigned y = 0; y < rows.size(); ++y) {
            for (unsigned x = 0; x < 8; ++x) {
                CAPTURE(x);
                CAPTURE(y);
                CHECK(bus.read32(framebuffer + 4 * (8 * y + x)) == ((rows[y] & (1U << x)) ? fill_color : 0));
            }
        }
    };
    resume();
    const auto end = n64->save_state();
    REQUIRE(n64->load_state(state));
    resume();
    CHECK(n64->save_state() == end);
}

TEST_CASE("RDP fills a right-major triangle with signed edges from a wrapped XBUS packet") {
    const auto n64 = N64::create(counting_rom("RDP TRIANGLE XBUS"));
    REQUIRE(n64);
    auto& bus = n64->bus();
    constexpr int pixel = 65536;
    constexpr std::array commands{
        std::uint64_t{0x3F10000600005000ULL},    // RGBA5551, stride 7
        rdp_rectangle(0x2D, 0, 0, 6, 4),
        kFillMode,
        std::uint64_t{0x37000000F80107C1ULL},
        rdp_triangle_header(false, 3, 3, 12),   // first row has one valid subscanline
        rdp_edge(-5 * pixel / 8, pixel / 2),   // negative X on the lower minor edge
        rdp_edge(7 * pixel / 2, -pixel),
        rdp_edge(0, 0),                         // no upper segment (YH == YM)
        kSyncFull,
    };
    constexpr std::uint32_t start = 0xFD8;
    for (unsigned i = 0; i < commands.size(); ++i) {
        bus.write64(0x04000000 | ((start + 8 * i) & 0xFFF), commands[i]);
    }
    bus.write32(kDpcStatus, 2);  // XBUS
    bus.write32(kDpcStart, start);
    bus.write32(kDpcEnd, start + 9 * 8);
    for (unsigned i = 0; i < 20; ++i) n64->step();
    CHECK(bus.read32(kDpcCurrent) == start + 9 * 8);
    CHECK(bus.read32(kDpcStatus) == 0x81);
    CHECK((bus.read32(0x04300008) & 0x20) != 0);
    constexpr std::array row_widths{3U, 3U, 2U, 0U};
    for (unsigned y = 0; y < row_widths.size(); ++y) {
        for (unsigned x = 0; x < 7; ++x) {
            CAPTURE(x);
            CAPTURE(y);
            const unsigned index = 7 * y + x;
            const unsigned color = index & 1 ? 0x07C1 : 0xF801;
            CHECK(bus.read16(0x5000 + 2 * index) == (x < row_widths[y] ? color : 0));
        }
    }
}

TEST_CASE("RDP one-cycle triangle uses the blender color and the half-open edge without antialiasing") {
    const auto n64 = N64::create(counting_rom("RDP SOLID BLEND"));
    REQUIRE(n64);
    auto& bus = n64->bus();
    constexpr int pixel = 65536;
    constexpr std::array commands{
        std::uint64_t{0x3F18000700004000ULL},   // RGBA8888, stride 8
        rdp_rectangle(0x2D, 0, 0, 8, 8),
        std::uint64_t{0x2F000000804C0200ULL},   // one cycle, blend RGB, coverage zap
        std::uint64_t{0x39000000FF000012ULL},   // red blend color
        rdp_triangle_header(true, 4, 4, 20),   // y = 1..5
        rdp_edge(5 * pixel, -pixel),           // right edge slopes from x = 5 to x = 1
        rdp_edge(5 * pixel / 4, 0),           // x = 1.25, so x = 1 misses the primary sample
        rdp_edge(0, 0),
        kSyncFull,
    };
    std::uint32_t end = 0x1000;
    for (const auto command : commands) {
        bus.write64(end, command);
        end += 8;
    }
    bus.write32(kDpcStart, 0x1000);
    bus.write32(kDpcEnd, end);
    for (unsigned i = 0; i < 20; ++i) n64->step();
    CHECK(bus.read32(kDpcCurrent) == end);
    CHECK((bus.read32(0x04300008) & 0x20) != 0);
    constexpr std::array right{0U, 5U, 4U, 3U, 2U, 0U};
    for (unsigned y = 0; y < right.size(); ++y) {
        for (unsigned x = 0; x < 8; ++x) {
            CAPTURE(x);
            CAPTURE(y);
            CHECK(bus.read32(0x4000 + 4 * (8 * y + x)) == ((x >= 2 && x < right[y]) ? 0xFF0000E0U : 0));
        }
    }
}

TEST_CASE("RDP one-cycle shaded triangle interpolates and clamps per-vertex color") {
    const auto n64 = N64::create(counting_rom("RDP SHADE"));
    REQUIRE(n64);
    auto& bus = n64->bus();
    constexpr int pixel = 65536;
    const auto rgba = [](int r, int g, int b, int a) {
        return (std::uint64_t{static_cast<std::uint16_t>(r)} << 48) |
               (std::uint64_t{static_cast<std::uint16_t>(g)} << 32) |
               (std::uint64_t{static_cast<std::uint16_t>(b)} << 16) | static_cast<std::uint16_t>(a);
    };
    constexpr std::array commands{
        std::uint64_t{0x3F18000300004000ULL},   // RGBA8888, stride 4
        rdp_rectangle(0x2D, 0, 0, 4, 4),
        std::uint64_t{0x2F0000C000000200ULL},   // one cycle, RGB dither off, coverage zap
        std::uint64_t{0x3C00001F00000100ULL},   // combiner cycle 1: RGB = shade
        rdp_triangle_header(true, 0, 0, 12) | (0x0CULL << 56) ^ (0x08ULL << 56),  // y = 0..3
        rdp_edge(4 * pixel, 0),
        rdp_edge(0, 0),
        rdp_edge(0, 0),
    };
    const std::array shade{
        rgba(0, 255, 0, 255),    // start color
        rgba(96, 0, 0, 0),       // per pixel: R += 96
        rgba(0, 0, 0, 0),
        rgba(0, 0, 0, 0),
        rgba(0, 0, 120, 0),      // per scanline: B += 120
        rgba(0, 0, 0, 0),
        rgba(0, 0, 0, 0),
        rgba(0, 0, 0, 0),
    };
    std::uint32_t end = 0x1000;
    for (const auto command : commands) bus.write64(end, command), end += 8;
    for (const auto word : shade) bus.write64(end, word), end += 8;
    bus.write64(end, kSyncFull), end += 8;
    bus.write32(kDpcStart, 0x1000);
    bus.write32(kDpcEnd, end);
    for (unsigned i = 0; i < 30; ++i) n64->step();
    CHECK(bus.read32(kDpcCurrent) == end);
    for (unsigned y = 0; y < 3; ++y) {
        for (unsigned x = 0; x < 4; ++x) {
            CAPTURE(x);
            CAPTURE(y);
            const unsigned r = std::min(96U * x, 255U), b = std::min(120U * y, 255U);
            CHECK(bus.read32(0x4000 + 4 * (4 * y + x)) == ((r << 24) | (0xFFU << 16) | (b << 8) | 0xE0U));
        }
    }
}

TEST_CASE("RDP shaded textured triangle modulates texels by the interpolated shade") {
    const auto n64 = N64::create(counting_rom("RDP SHADE TEXTURE"));
    REQUIRE(n64);
    auto& bus = n64->bus();
    constexpr int pixel = 65536;
    constexpr std::uint32_t source = 0x2000;
    for (std::uint32_t at = source; at < source + 32; at += 8) bus.write64(at, ~0ULL);  // white RGBA16 4x4
    const auto rgba = [](int r, int g, int b, int a) {
        return (std::uint64_t{static_cast<std::uint16_t>(r)} << 48) |
               (std::uint64_t{static_cast<std::uint16_t>(g)} << 32) |
               (std::uint64_t{static_cast<std::uint16_t>(b)} << 16) | static_cast<std::uint16_t>(a);
    };
    std::vector<std::uint64_t> commands{
        0x3F18000300004000ULL,             // RGBA8888, stride 4
        rdp_rectangle(0x2D, 0, 0, 4, 4),
        kPointMode | (1U << 9),             // coverage zap
        0x3C0000240F0001C0ULL,             // cycle 1: RGB = TEXEL0 * SHADE
        0x3D10000300000000ULL | source,
        rdp_tile(0, 2, 0, 1, 0),
        rdp_rectangle(0x34, 0, 0, 3, 3),
        rdp_triangle_header(true, 0, 0, 12) | (0x06ULL << 56),  // 0x0E, y = 0..3
        rdp_edge(4 * pixel, 0),
        rdp_edge(0, 0),
        rdp_edge(0, 0),
        rgba(0, 255, 0, 255), rgba(96, 0, 0, 0), 0, 0,   // shade: color, d/dx, fractions
        rgba(0, 0, 120, 0), 0, 0, 0,                     // shade: d/de, d/dy, fractions
        0, 0, 0, 0, 0, 0, 0, 0,                          // S = T = 0
        kSyncFull,
    };
    std::uint32_t end = 0x1000;
    for (const auto command : commands) bus.write64(end, command), end += 8;
    bus.write32(kDpcStart, 0x1000);
    bus.write32(kDpcEnd, end);
    for (unsigned i = 0; i < 60; ++i) n64->step();
    CHECK(bus.read32(kDpcCurrent) == end);
    const auto modulate = [](unsigned shade) { return (std::min(shade, 255U) * 255 + 128) >> 8; };
    for (unsigned y = 0; y < 3; ++y) {
        for (unsigned x = 0; x < 4; ++x) {
            CAPTURE(x);
            CAPTURE(y);
            const unsigned expected = (modulate(96 * x) << 24) | (modulate(255) << 16) | (modulate(120 * y) << 8) | 0xE0;
            CHECK(bus.read32(0x4000 + 4 * (4 * y + x)) == expected);
        }
    }
}

TEST_CASE("RDP combiner evaluates (A - B) * C + D with shade alpha as the multiplier") {
    const auto n64 = N64::create(counting_rom("RDP COMBINER LERP"));
    REQUIRE(n64);
    auto& bus = n64->bus();
    constexpr int pixel = 65536;
    const std::vector<std::uint64_t> commands{
        0x3F18000300004000ULL,   // RGBA8888, stride 4
        rdp_rectangle(0x2D, 0, 0, 4, 4),
        0x2F0000C000000200ULL,   // one cycle, dither off, coverage zap
        0x3C00006B05FC017EULL,   // RGB = (PRIM - ENV) * SHADE_ALPHA + ENV, alpha = 1
        0x3A000000FF0000FFULL,   // red
        0x3B0000000000FFFFULL,   // blue
        rdp_triangle_header(true, 0, 0, 4) | (0x04ULL << 56),  // 0x0C, row 0
        rdp_edge(4 * pixel, 0),
        rdp_edge(0, 0),
        rdp_edge(0, 0),
        0, std::uint64_t{85} << 0, 0, 0,  // shade alpha += 85 per pixel
        0, 0, 0, 0,
        kSyncFull,
    };
    std::uint32_t end = 0x1000;
    for (const auto command : commands) bus.write64(end, command), end += 8;
    bus.write32(kDpcStart, 0x1000);
    bus.write32(kDpcEnd, end);
    for (unsigned i = 0; i < 30; ++i) n64->step();
    CHECK(bus.read32(kDpcCurrent) == end);
    for (unsigned x = 0; x < 4; ++x) {
        CAPTURE(x);
        const int alpha = static_cast<int>(std::min(85U * x, 255U));
        const auto red = static_cast<unsigned>((255 * alpha + 0x80) >> 8);
        const auto blue = static_cast<unsigned>((-255 * alpha + (255 << 8) + 0x80) >> 8);
        CHECK(bus.read32(0x4000 + 4 * x) == ((red << 24) | (blue << 8) | 0xE0U));
    }
}

namespace {

// Set Combine fields: RGB A, B, C, D and alpha A, B, C, D for one cycle.
struct CombinerCycle {
    unsigned a, b, c, d, alpha_a, alpha_b, alpha_c, alpha_d;
};

constexpr std::uint64_t set_combine(CombinerCycle first, CombinerCycle second) {
    using U = std::uint64_t;
    return (U{0x3C} << 56) | (U{first.a} << 52) | (U{first.c} << 47) | (U{first.alpha_a} << 44) |
           (U{first.alpha_c} << 41) | (U{second.a} << 37) | (U{second.c} << 32) | (U{first.b} << 28) |
           (U{second.b} << 24) | (U{second.alpha_a} << 21) | (U{second.alpha_c} << 18) | (U{first.d} << 15) |
           (U{first.alpha_b} << 12) | (U{first.alpha_d} << 9) | (U{second.d} << 6) | (U{second.alpha_b} << 3) |
           U{second.alpha_d};
}

// Draws a shaded one-row triangle (x = 0..3) with constant shade into an RGBA8888 framebuffer.
void draw_two_cycle_row(N64& n64, std::uint64_t modes, std::uint64_t combine, std::uint64_t shade_rgba,
                        std::uint64_t primitive = 0x3A000000C86432FFULL) {
    auto& bus = n64.bus();
    constexpr int pixel = 65536;
    const std::vector<std::uint64_t> commands{
        0x3F18000300004000ULL,
        rdp_rectangle(0x2D, 0, 0, 4, 4),
        modes,
        combine,
        primitive,               // default (200, 100, 50)
        0x3B00000080808080ULL,   // environment 128
        0x3800000000FF0000ULL,   // fog: green
        rdp_triangle_header(true, 0, 0, 4) | (0x04ULL << 56),
        rdp_edge(4 * pixel, 0),
        rdp_edge(0, 0),
        rdp_edge(0, 0),
        shade_rgba, 0, 0, 0, 0, 0, 0, 0,
        kSyncFull,
    };
    std::uint32_t end = 0x1000;
    for (const auto command : commands) bus.write64(end, command), end += 8;
    bus.write32(kDpcStart, 0x1000);
    bus.write32(kDpcEnd, end);
    for (unsigned i = 0; i < 30; ++i) n64.step();
    CHECK(bus.read32(kDpcCurrent) == end);
}

constexpr std::uint64_t kTwoCycle = (0x2FULL << 56) | (1ULL << 52) | (3ULL << 38) | (1U << 9);  // dither off, zap

}  // namespace

TEST_CASE("RDP two-cycle combiner feeds cycle 0 into COMBINED for cycle 1") {
    const auto n64 = N64::create(counting_rom("RDP TWO CYCLE COMBINE"));
    REQUIRE(n64);
    // Cycle 0: PRIM * SHADE, alpha 1. Cycle 1: COMBINED * ENV, alpha COMBINED.
    const auto combine = set_combine({3, 15, 4, 7, 7, 7, 7, 6}, {0, 15, 5, 7, 7, 7, 7, 0});
    draw_two_cycle_row(*n64, kTwoCycle, combine, 0x0080008000800080ULL);
    const auto scale = [](unsigned value, unsigned factor) { return (value * factor + 0x80) >> 8; };
    const unsigned r = scale(scale(200, 128), 128), g = scale(scale(100, 128), 128), b = scale(scale(50, 128), 128);
    for (unsigned x = 0; x < 4; ++x) {
        CAPTURE(x);
        CHECK(n64->bus().read32(0x4000 + 4 * x) == ((r << 24) | (g << 16) | (b << 8) | 0xE0U));
    }
}

TEST_CASE("RDP two-cycle blender mixes fog by shade alpha in its first cycle") {
    const auto n64 = N64::create(counting_rom("RDP TWO CYCLE FOG"));
    REQUIRE(n64);
    // Both combiner cycles pass the primitive color: D = PRIM, then D = COMBINED.
    const auto combine = set_combine({15, 15, 31, 3, 7, 7, 7, 6}, {15, 15, 31, 0, 7, 7, 7, 0});
    // Blender cycle 0: FOG * SHADE_ALPHA + PIXEL * (1 - A). Cycle 1 passes the result.
    draw_two_cycle_row(*n64, kTwoCycle | (3ULL << 30) | (2ULL << 26), combine, 0x0000000000000080ULL);
    // A = 0x80 >> 3 = 16, so fog and pixel each get half.
    const unsigned r = (200 * 16) >> 5, g = (255 * 16 + 100 * 16) >> 5, b = (50 * 16) >> 5;
    for (unsigned x = 0; x < 4; ++x) {
        CAPTURE(x);
        CHECK(n64->bus().read32(0x4000 + 4 * x) == ((r << 24) | (g << 16) | (b << 8) | 0xE0U));
    }
}

namespace {

constexpr std::uint64_t kOneCycle = (0x2FULL << 56) | (3ULL << 38);  // dither off, clamp coverage
// RGB D = PRIM, alpha D = SHADE.
constexpr auto kPrimShadeAlpha = set_combine({15, 15, 31, 3, 7, 7, 7, 4}, {15, 15, 31, 3, 7, 7, 7, 4});

void set_blend_color(N64& n64, std::uint32_t rgba) {
    auto& bus = n64.bus();
    bus.write64(0x2000, 0x3900000000000000ULL | rgba);
    bus.write64(0x2008, kSyncFull);
    bus.write32(kDpcStart, 0x2000);
    bus.write32(kDpcEnd, 0x2010);
    for (unsigned i = 0; i < 10; ++i) n64.step();
}

}  // namespace

TEST_CASE("RDP alpha compare drops pixels below the blend color alpha") {
    const auto n64 = N64::create(counting_rom("RDP ALPHA COMPARE"));
    REQUIRE(n64);
    set_blend_color(*n64, 0x00000080);
    draw_two_cycle_row(*n64, kOneCycle | 1U, kPrimShadeAlpha, 0x0000000000000040ULL);
    for (unsigned x = 0; x < 4; ++x) CHECK(n64->bus().read32(0x4000 + 4 * x) == 0);
    draw_two_cycle_row(*n64, kOneCycle | 1U, kPrimShadeAlpha, 0x00000000000000C0ULL);
    for (unsigned x = 0; x < 4; ++x) CHECK(n64->bus().read32(0x4000 + 4 * x) == 0xC86432E0U);
}

TEST_CASE("RDP coverage times alpha scales coverage and feeds it to alpha compare") {
    const auto n64 = N64::create(counting_rom("RDP CVG X ALPHA"));
    REQUIRE(n64);
    // Alpha 0x80 halves full coverage (8 -> 4); alpha coverage select makes alpha (0x80 * 8 + 4) >> 5 = 32.
    set_blend_color(*n64, 0x00000020);
    constexpr std::uint64_t modes = kOneCycle | (1U << 12) | (1U << 13) | 1U;
    draw_two_cycle_row(*n64, modes, kPrimShadeAlpha, 0x0000000000000080ULL);
    // Clamp mode stores the overflowing pixel's own coverage minus one.
    for (unsigned x = 0; x < 4; ++x) CHECK(n64->bus().read32(0x4000 + 4 * x) == 0xC8643260U);
    set_blend_color(*n64, 0x00000021);
    draw_two_cycle_row(*n64, modes, kPrimShadeAlpha, 0x0000000000000080ULL);
    // Alpha 32 is now below 33, so memory keeps the previous pixels.
    for (unsigned x = 0; x < 4; ++x) CHECK(n64->bus().read32(0x4000 + 4 * x) == 0xC8643260U);
}

TEST_CASE("RDP chroma key turns the distance from the key center into alpha") {
    const auto n64 = N64::create(counting_rom("RDP CHROMA KEY"));
    REQUIRE(n64);
    auto& bus = n64->bus();
    const auto run = [&](std::initializer_list<std::uint64_t> words) {
        std::uint32_t end = 0x2000;
        for (const auto word : words) bus.write64(end, word), end += 8;
        bus.write64(end, kSyncFull), end += 8;
        bus.write32(kDpcStart, 0x2000);
        bus.write32(kDpcEnd, end);
        for (unsigned i = 0; i < 10; ++i) n64->step();
    };
    // RGB = (PRIM - KEY_CENTER) * KEY_SCALE; the primitive is (200, 100, 50).
    const auto combine = set_combine({3, 6, 6, 7, 7, 7, 7, 6}, {3, 6, 6, 7, 7, 7, 7, 6});
    constexpr std::uint64_t modes = kOneCycle | (1ULL << 40) | 1U;  // chroma key, alpha compare

    // Key center on the primitive with zero widths: every channel is inside, alpha 0.
    run({0x2B000000000000C8ULL | (0xC8ULL << 8), 0x2A00000064FF32FFULL, 0x3900000000000001ULL});
    draw_two_cycle_row(*n64, modes, combine, 0);
    for (unsigned x = 0; x < 4; ++x) CHECK(bus.read32(0x4000 + 4 * x) == 0);

    // Red keyed from 0 at full scale: R = 199 against a 1.0 width (256) leaves alpha 57.
    run({0x2B000000001000FFULL, 0x2A01001064FF32FFULL, 0x3900000000000039ULL});
    draw_two_cycle_row(*n64, modes, combine, 0);
    for (unsigned x = 0; x < 4; ++x) CHECK(bus.read32(0x4000 + 4 * x) == 0xC70000E0U);
    for (unsigned x = 0; x < 4; ++x) bus.write32(0x4000 + 4 * x, 0);
    run({0x390000000000003AULL});  // threshold 58 is above alpha 57
    draw_two_cycle_row(*n64, modes, combine, 0);
    for (unsigned x = 0; x < 4; ++x) CHECK(bus.read32(0x4000 + 4 * x) == 0);
}

TEST_CASE("RDP combiner reads K4, K5 and the primitive LOD fraction") {
    const auto n64 = N64::create(counting_rom("RDP CONVERT K4 K5"));
    REQUIRE(n64);
    auto& bus = n64->bus();
    bus.write64(0x2000, 0x2C00000000006480ULL);  // Set Convert: K4 = 50, K5 = 128
    bus.write64(0x2008, kSyncFull);
    bus.write32(kDpcStart, 0x2000);
    bus.write32(kDpcEnd, 0x2010);
    for (unsigned i = 0; i < 10; ++i) n64->step();
    // RGB = (PRIM - K4) * K5; alpha = PRIM_ALPHA * PRIM_LOD_FRAC.
    const auto combine = set_combine({3, 7, 15, 7, 3, 7, 6, 7}, {3, 7, 15, 7, 3, 7, 6, 7});
    // Set Primitive Color with an LOD fraction of 0x40.
    draw_two_cycle_row(*n64, kOneCycle, combine, 0, 0x3A000040C86432FFULL);
    const unsigned r = (150 * 128 + 0x80) >> 8, g = (50 * 128 + 0x80) >> 8;
    for (unsigned x = 0; x < 4; ++x) CHECK(bus.read32(0x4000 + 4 * x) == ((r << 24) | (g << 16) | 0xE0U));
    // The blender sees the alpha, so check it through alpha compare: (255 * 0x40 + 0x80) >> 8 = 64.
    for (unsigned x = 0; x < 4; ++x) bus.write32(0x4000 + 4 * x, 0);
    bus.write64(0x2000, 0x3900000000000041ULL);  // threshold 65
    bus.write32(kDpcStart, 0x2000);
    bus.write32(kDpcEnd, 0x2010);
    for (unsigned i = 0; i < 10; ++i) n64->step();
    draw_two_cycle_row(*n64, kOneCycle | 1U, combine, 0, 0x3A000040C86432FFULL);
    for (unsigned x = 0; x < 4; ++x) CHECK(bus.read32(0x4000 + 4 * x) == 0);
}

TEST_CASE("RDP combiner noise input is a 9-bit value with random top bits") {
    const auto n64 = N64::create(counting_rom("RDP NOISE"));
    REQUIRE(n64);
    // RGB = NOISE * PRIM_ALPHA (255), since C = 6 is KEY_SCALE.
    const auto combine = set_combine({7, 15, 10, 7, 7, 7, 7, 6}, {7, 15, 10, 7, 7, 7, 7, 6});
    draw_two_cycle_row(*n64, kOneCycle, combine, 0);
    for (unsigned x = 0; x < 4; ++x) {
        CAPTURE(x);
        const auto pixel = n64->bus().read32(0x4000 + 4 * x);
        const unsigned r = pixel >> 24;
        // Positive noise 0x20, 0x60, 0xA0, 0xE0 times 255/256; negative values clamp to 0.
        CHECK((r == 0 || r == 32 || r == 96 || r == 159 || r == 223));
        CHECK(((pixel >> 16) & 255) == r);  // the same noise feeds all three channels
    }
}

TEST_CASE("RDP mipmapped triangle picks tiles by LOD and lerps them by LOD_FRACTION") {
    constexpr std::uint64_t kWhite = ~0ULL, kGray = 0x8421842184218421ULL;  // gray 16/31 expands to 132
    constexpr std::uint64_t kSharpen = 1ULL << 49, kDetail = 1ULL << 50;
    struct Case {
        int step;               // texels per pixel along S, in 10.5
        std::uint64_t extra;    // sharpen / detail mode bits
        unsigned max_level;
        std::uint64_t level0;   // texels of tile 0; tile 1 is red
        std::uint32_t expected;
    };
    const auto lerp = [](int from, int to, int fraction) {
        return static_cast<std::uint32_t>(std::clamp(((to - from) * fraction + (from << 8) + 0x80) >> 8, 0, 255));
    };
    // Gray (132) toward red (255, 0, 0) with a negative fraction moves away from red.
    const std::uint32_t sharp_r = lerp(132, 255, -128), sharp_gb = lerp(132, 0, -128);
    const Case cases[] = {
        // 1.5 texels: level 0 with fraction 0x80 halfway to level 1 (white to red).
        {48, 0, 1, kWhite, 0xFF8080E0U},
        // 3 texels: past the last level, so both texels come from level 1 (red).
        {96, 0, 1, kWhite, 0xFF0000E0U},
        // 0.5 texels magnifies: plain mipmapping clamps the fraction to 0 and keeps level 0.
        {16, 0, 1, kGray, 0x848484E0U},
        // Sharpen makes the magnified fraction negative (0x80 - 0x100), extrapolating away from level 1.
        {16, kSharpen, 1, kGray, (sharp_r << 24) | (sharp_gb << 16) | (sharp_gb << 8) | 0xE0U},
        // Detail while magnifying blends the detail tile (0) with level 0 (tile 1) by the raw fraction.
        {16, kDetail, 0, kWhite, 0xFF8080E0U},
    };
    for (const auto& [step, extra, max_level, level0, expected] : cases) {
        CAPTURE(step);
        CAPTURE(extra);
        const auto n64 = N64::create(counting_rom("RDP MIPMAP"));
        REQUIRE(n64);
        auto& bus = n64->bus();
        constexpr int pixel = 65536;
        constexpr std::uint32_t white = 0x2000, red = 0x2100;
        for (std::uint32_t at = 0; at < 32; at += 8) {
            bus.write64(white + at, level0);
            bus.write64(red + at, 0xF801F801F801F801ULL);
        }
        const std::uint64_t modes = (0x2FULL << 56) | (1ULL << 52) | (1ULL << 48) | (6ULL << 41) |
                                    (3ULL << 38) | (1U << 9) | extra;  // two cycles, LOD, zap
        const std::vector<std::uint64_t> commands{
            0x3F18000300004000ULL,
            rdp_rectangle(0x2D, 0, 0, 4, 4),
            modes,
            // Cycle 0: (TEXEL1 - TEXEL0) * LOD_FRACTION + TEXEL0; cycle 1 passes COMBINED.
            set_combine({2, 1, 13, 1, 7, 7, 7, 6}, {15, 15, 31, 0, 7, 7, 7, 0}),
            0x3D10000300000000ULL | white,
            rdp_tile(0, 2, 0, 1, 0),
            rdp_rectangle(0x34, 0, 0, 3, 3),
            0x3D10000300000000ULL | red,
            rdp_tile(0, 2, 1, 1, 4),
            rdp_rectangle(0x34, 0, 0, 3, 3) | (1ULL << 24),
            rdp_triangle_header(true, 0, 0, 4) | (0x02ULL << 56) | (std::uint64_t{max_level} << 51),  // 0x0A
            rdp_edge(4 * pixel, 0),
            rdp_edge(0, 0),
            rdp_edge(0, 0),
            0, std::uint64_t(step) << 48, 0, 0, 0, 0, 0, 0,  // S = T = 0, DsDx = step
            kSyncFull,
        };
        std::uint32_t end = 0x1000;
        for (const auto command : commands) bus.write64(end, command), end += 8;
        bus.write32(kDpcStart, 0x1000);
        bus.write32(kDpcEnd, end);
        for (unsigned i = 0; i < 60; ++i) n64->step();
        CHECK(bus.read32(kDpcCurrent) == end);
        for (unsigned x = 0; x < 4; ++x) {
            CAPTURE(x);
            CHECK(bus.read32(0x4000 + 4 * x) == expected);
        }
    }
}

TEST_CASE("RDP textured rectangle with LOD uses level 0 and detail blending") {
    // Rectangles have no max level, so plain LOD is always at the last level (fraction 0xFF,
    // both texels from tile 0). Detail while magnifying blends tile 0 with tile 1 by the raw fraction.
    for (const auto [extra, expected] : {std::pair{0ULL, 0xFFFFFFE0U}, std::pair{1ULL << 50, 0xFF8080E0U}}) {
        CAPTURE(extra);
        const auto n64 = N64::create(counting_rom("RDP RECT LOD"));
        REQUIRE(n64);
        auto& bus = n64->bus();
        constexpr std::uint32_t white = 0x2000, red = 0x2100;
        for (std::uint32_t at = 0; at < 32; at += 8) {
            bus.write64(white + at, ~0ULL);
            bus.write64(red + at, 0xF801F801F801F801ULL);
        }
        const std::uint64_t modes = (0x2FULL << 56) | (1ULL << 52) | (1ULL << 48) | (6ULL << 41) |
                                    (3ULL << 38) | (1U << 9) | extra;
        const std::vector<std::uint64_t> commands{
            0x3F18000300004000ULL,
            rdp_rectangle(0x2D, 0, 0, 4, 4),
            modes,
            set_combine({2, 1, 13, 1, 7, 7, 7, 6}, {15, 15, 31, 0, 7, 7, 7, 0}),
            0x3D10000300000000ULL | white,
            rdp_tile(0, 2, 0, 1, 0),
            rdp_rectangle(0x34, 0, 0, 3, 3),
            0x3D10000300000000ULL | red,
            rdp_tile(0, 2, 1, 1, 4),
            rdp_rectangle(0x34, 0, 0, 3, 3) | (1ULL << 24),
            rdp_rectangle(0x24, 4, 1, 0, 0),
            rdp_texture_coords(0, 0, 512, 512),  // half a texel per pixel: magnified, fraction 0x80
            kSyncFull,
        };
        std::uint32_t end = 0x1000;
        for (const auto command : commands) bus.write64(end, command), end += 8;
        bus.write32(kDpcStart, 0x1000);
        bus.write32(kDpcEnd, end);
        for (unsigned i = 0; i < 60; ++i) n64->step();
        CHECK(bus.read32(kDpcCurrent) == end);
        for (unsigned x = 0; x < 4; ++x) {
            CAPTURE(x);
            CHECK(bus.read32(0x4000 + 4 * x) == expected);
        }
    }
}

TEST_CASE("RDP two-cycle TEXEL1 in cycle 1 is the next pixel's TEXEL0") {
    const auto n64 = N64::create(counting_rom("RDP NEXT TEXEL"));
    REQUIRE(n64);
    auto& bus = n64->bus();
    constexpr std::uint32_t source = 0x2000;
    bus.write64(source, 0xF80107C1003FFFFFULL);  // RGBA16 row: red, green, blue, white
    const std::uint64_t modes = (0x2FULL << 56) | (1ULL << 52) | (6ULL << 41) | (3ULL << 38) | (1U << 9);
    const std::vector<std::uint64_t> commands{
        0x3F18000300004000ULL,
        rdp_rectangle(0x2D, 0, 0, 4, 4),
        modes,
        // Cycle 0 reads TEXEL0; cycle 1 outputs its TEXEL1.
        set_combine({15, 15, 31, 1, 7, 7, 7, 6}, {15, 15, 31, 2, 7, 7, 7, 0}),
        0x3D10000300000000ULL | source,
        rdp_tile(0, 2, 0, 1, 0),
        rdp_rectangle(0x34, 0, 0, 3, 0),
        rdp_rectangle(0x24, 4, 1, 0, 0),
        rdp_texture_coords(0, 0, 1024, 1024),  // one texel per pixel
        kSyncFull,
    };
    std::uint32_t end = 0x1000;
    for (const auto command : commands) bus.write64(end, command), end += 8;
    bus.write32(kDpcStart, 0x1000);
    bus.write32(kDpcEnd, end);
    for (unsigned i = 0; i < 40; ++i) n64->step();
    CHECK(bus.read32(kDpcCurrent) == end);
    // Pixel x shows texel x + 1.
    CHECK(bus.read32(0x4000) == 0x00FF00E0U);
    CHECK(bus.read32(0x4004) == 0x0000FFE0U);
    CHECK(bus.read32(0x4008) == 0xFFFFFFE0U);
}

TEST_CASE("RDP one-cycle fill rectangle goes through the combiner and blender") {
    const auto n64 = N64::create(counting_rom("RDP FLAT RECT"));
    REQUIRE(n64);
    auto& bus = n64->bus();
    for (std::uint32_t at = 0x4000; at < 0x4020; at += 4) bus.write32(at, 0xFFFFFFFF);  // white
    const std::vector<std::uint64_t> commands{
        0x3F18000300004000ULL,
        rdp_rectangle(0x2D, 0, 0, 8, 4),
        // P = COMBINED * combined alpha + MEMORY * (1 - A), image read, force blend.
        (0x2FULL << 56) | (3ULL << 38) | 0x00504240ULL,
        set_combine({15, 15, 31, 3, 7, 7, 7, 3}, {15, 15, 31, 3, 7, 7, 7, 3}),  // RGBA = PRIM
        0x3A00000000000080ULL,                 // black at half alpha: a fade
        rdp_rectangle(0x36, 4, 1, 0, 0),       // x 0..4, y 0..1, lower-right edge excluded
        kSyncFull,
    };
    std::uint32_t end = 0x1000;
    for (const auto command : commands) bus.write64(end, command), end += 8;
    bus.write32(kDpcStart, 0x1000);
    bus.write32(kDpcEnd, end);
    for (unsigned i = 0; i < 20; ++i) n64->step();
    CHECK(bus.read32(kDpcCurrent) == end);
    // A = 0x80 >> 3 = 16: half black, half white memory.
    for (unsigned x = 0; x < 4; ++x) {
        CAPTURE(x);
        CHECK(bus.read32(0x4000 + 4 * x) == 0x7F7F7FE0U);
    }
    CHECK(bus.read32(0x4010) == 0xFFFFFFFFU);  // x = 4 is outside
}

TEST_CASE("RDP combiner multiplies a texel by its own alpha") {
    const auto n64 = N64::create(counting_rom("RDP COMBINER TEXEL ALPHA"));
    REQUIRE(n64);
    auto& bus = n64->bus();
    constexpr int pixel = 65536;
    constexpr std::uint32_t source = 0x2000;
    bus.write64(source, 0xF801F800F801F800ULL);  // red texels, alpha bit set on even ones
    const std::vector<std::uint64_t> commands{
        0x3F18000300004000ULL,
        rdp_rectangle(0x2D, 0, 0, 4, 4),
        kPointMode | (1U << 9),
        0x3C0000280FFC01FEULL,   // RGB = TEXEL0 * TEXEL0_ALPHA, alpha = 1
        0x3D10000300000000ULL | source,
        rdp_tile(0, 2, 0, 1, 0),
        rdp_rectangle(0x34, 0, 0, 3, 0),
        rdp_triangle_header(true, 0, 0, 4) | (0x02ULL << 56),  // 0x0A, row 0
        rdp_edge(4 * pixel, 0),
        rdp_edge(0, 0),
        rdp_edge(0, 0),
        0, rdp_stw_halves(32, 0), 0, 0,  // S advances one texel per pixel
        0, 0, 0, 0,
        kSyncFull,
    };
    std::uint32_t end = 0x1000;
    for (const auto command : commands) bus.write64(end, command), end += 8;
    bus.write32(kDpcStart, 0x1000);
    bus.write32(kDpcEnd, end);
    for (unsigned i = 0; i < 40; ++i) n64->step();
    CHECK(bus.read32(kDpcCurrent) == end);
    for (unsigned x = 0; x < 4; ++x) {
        CAPTURE(x);
        CHECK(bus.read32(0x4000 + 4 * x) == (x % 2 == 0 ? 0xFE0000E0U : 0x000000E0U));
    }
}

TEST_CASE("RDP force blend mixes a translucent primitive with framebuffer memory") {
    const auto n64 = N64::create(counting_rom("RDP BLEND"));
    REQUIRE(n64);
    auto& bus = n64->bus();
    constexpr int pixel = 65536;
    for (std::uint32_t at = 0x4000; at < 0x4010; at += 4) bus.write32(at, 0x0000FFE0);  // blue, full coverage
    bus.write16(0x5000, 0x003F);  // RGBA5551 blue, alpha bit set
    const auto run = [&](std::uint64_t color_image) {
        constexpr std::uint64_t modes = 0x2F0000C000404240ULL;  // P*A + memory*(1-A), image read, force blend
        const std::array commands{
            color_image,
            rdp_rectangle(0x2D, 0, 0, 4, 4),
            modes,
            std::uint64_t{0x3C00001F001C00C3ULL},  // cycle 1: RGB = primitive, alpha = primitive
            std::uint64_t{0x3A000000FF000080ULL},  // half-transparent red
            rdp_triangle_header(true, 0, 0, 4),   // row 0
            rdp_edge(4 * pixel, 0),
            rdp_edge(0, 0),
            rdp_edge(0, 0),
            kSyncFull,
        };
        std::uint32_t end = 0x1000;
        for (const auto command : commands) bus.write64(end, command), end += 8;
        bus.write32(kDpcStart, 0x1000);
        bus.write32(kDpcEnd, end);
        for (unsigned i = 0; i < 20; ++i) n64->step();
        CHECK(bus.read32(kDpcCurrent) == end);
    };
    run(0x3F18000300004000ULL);  // RGBA8888, stride 4
    // A = 0x80 >> 3 = 16 and 1 - A = (0x7F >> 3) + 1 = 16, so each input gets half weight.
    for (unsigned x = 0; x < 4; ++x) {
        CAPTURE(x);
        CHECK(bus.read32(0x4000 + 4 * x) == 0x7F007FE0U);
    }
    run(0x3F10000000005000ULL);  // RGBA5551, stride 1
    CHECK(bus.read16(0x5000) == ((0x0FU << 11) | (0x0FU << 1) | 1U));
}

TEST_CASE("RDP depth triangles hide farther pixels and store compressed Z") {
    const auto n64 = N64::create(counting_rom("RDP ZBUFFER"));
    REQUIRE(n64);
    auto& bus = n64->bus();
    constexpr int pixel = 65536;
    for (std::uint32_t at = 0x6000; at < 0x6020; at += 4) bus.write32(at, 0xFFFCFFFC);  // farthest depth
    // Z-buffered triangle (0x09) covering x = left..4 on rows 0 and 1, at a constant depth.
    // Z has 10 fractional bits below the 18 the depth unit uses.
    const auto triangle = [](unsigned left, std::uint32_t z) {
        return std::array{
            rdp_triangle_header(true, 0, 0, 8) | (0x01ULL << 56),
            rdp_edge(4 * pixel, 0),
            rdp_edge(static_cast<std::int32_t>(left) * pixel, 0),
            rdp_edge(0, 0),
            std::uint64_t{z} << 32,  // Z and DzDx
            std::uint64_t{0},        // DzDe and DzDy
        };
    };
    std::vector<std::uint64_t> commands{
        0x3F18000300004000ULL,   // RGBA8888, stride 4
        0x3E00000000006000ULL,   // depth buffer
        rdp_rectangle(0x2D, 0, 0, 4, 4),
        0x2F0000C000000230ULL,   // one cycle, dither off, coverage zap, Z compare and update
        0x3C00001F000000C0ULL,   // combiner cycle 1: RGB = primitive
        0x3A000000FF0000FFULL,
    };
    const auto add = [&](unsigned left, std::uint32_t z, std::uint32_t color) {
        commands.push_back(0x3A00000000000000ULL | color);
        for (const auto word : triangle(left, z)) commands.push_back(word);
    };
    add(0, 0x4000U << 10, 0xFF0000FF);  // red, middle depth
    add(2, 0x8000U << 10, 0x00FF00FF);  // green, behind red
    add(3, 0x2000U << 10, 0x0000FFFF);  // blue, in front of red
    commands.push_back(kSyncFull);
    std::uint32_t end = 0x1000;
    for (const auto command : commands) bus.write64(end, command), end += 8;
    bus.write32(kDpcStart, 0x1000);
    bus.write32(kDpcEnd, end);
    for (unsigned i = 0; i < 40; ++i) n64->step();
    CHECK(bus.read32(kDpcCurrent) == end);
    for (unsigned y = 0; y < 2; ++y) {
        for (unsigned x = 0; x < 4; ++x) {
            CAPTURE(x);
            CAPTURE(y);
            CHECK(bus.read32(0x4000 + 16 * y + 4 * x) == (x == 3 ? 0x0000FFE0U : 0xFF0000E0U));
            // 0x4000 compresses to exponent 0, mantissa 0x100; 0x2000 to mantissa 0x080.
            CHECK(bus.read16(0x6000 + 8 * y + 2 * x) == (x == 3 ? 0x0200 : 0x0400));
        }
    }
    CHECK(bus.read32(0x6010) == 0xFFFCFFFC);  // row 2 untouched
}

TEST_CASE("RDP decal Z mode only draws pixels coplanar with the depth buffer") {
    const auto n64 = N64::create(counting_rom("RDP ZDECAL"));
    REQUIRE(n64);
    auto& bus = n64->bus();
    constexpr int pixel = 65536;
    for (std::uint32_t at = 0x6000; at < 0x6020; at += 4) bus.write32(at, 0xFFFCFFFC);
    std::vector<std::uint64_t> commands{
        0x3F18000300004000ULL,
        0x3E00000000006000ULL,
        rdp_rectangle(0x2D, 0, 0, 4, 4),
        0x2F0000C000000230ULL,   // opaque Z, compare and update
        0x3C00001F000000C0ULL,
    };
    const auto add = [&](unsigned left, std::uint32_t z, std::uint32_t color) {
        commands.push_back(0x3A00000000000000ULL | color);
        for (const auto word : {rdp_triangle_header(true, 0, 0, 8) | (0x01ULL << 56), rdp_edge(4 * pixel, 0),
                                rdp_edge(static_cast<std::int32_t>(left) * pixel, 0), rdp_edge(0, 0),
                                std::uint64_t{z} << 32, std::uint64_t{0}}) {
            commands.push_back(word);
        }
    };
    add(0, 0x4000U << 10, 0xFF0000FF);           // red base surface
    commands.push_back(0x2F0000C000000E10ULL);   // decal Z, compare only
    // Flat memory DZ at a low exponent widens to 16, so the coplanar range is 16 << 3 = 128.
    add(2, (0x4000U - 0x40) << 10, 0x00FF00FF);  // green decal, within the DZ range
    add(3, (0x4000U - 0x100) << 10, 0x0000FFFF); // blue is nearer than the range, so not coplanar
    commands.push_back(kSyncFull);
    std::uint32_t end = 0x1000;
    for (const auto command : commands) bus.write64(end, command), end += 8;
    bus.write32(kDpcStart, 0x1000);
    bus.write32(kDpcEnd, end);
    for (unsigned i = 0; i < 40; ++i) n64->step();
    CHECK(bus.read32(kDpcCurrent) == end);
    for (unsigned x = 0; x < 4; ++x) {
        CAPTURE(x);
        CHECK(bus.read32(0x4000 + 4 * x) == (x >= 2 ? 0x00FF00E0U : 0xFF0000E0U));
        CHECK(bus.read16(0x6000 + 2 * x) == 0x0400);
    }
}

TEST_CASE("RDP depth writes the DZ code from the Z slopes and uses it for non-overflowing pixels") {
    const auto n64 = N64::create(counting_rom("RDP ZDZ"));
    REQUIRE(n64);
    auto& bus = n64->bus();
    constexpr int pixel = 65536;
    for (std::uint32_t at = 0x6000; at < 0x6020; at += 4) bus.write32(at, 0xFFFCFFFC);
    // Constant Z with DzDy = 0x0100'0000: DZ = 0x100, code 8, stored top bits 2.
    const std::vector<std::uint64_t> commands{
        0x3F18000300004000ULL,
        0x3E00000000006000ULL,
        rdp_rectangle(0x2D, 0, 0, 4, 4),
        0x2F0000C000000230ULL,
        0x3C00001F000000C0ULL,
        0x3A000000FF0000FFULL,
        rdp_triangle_header(true, 0, 0, 8) | (0x01ULL << 56), rdp_edge(4 * pixel, 0), rdp_edge(0, 0),
        rdp_edge(0, 0), std::uint64_t{0x4000U << 10} << 32, 0x01000000ULL,
        kSyncFull,
    };
    std::uint32_t end = 0x1000;
    for (const auto command : commands) bus.write64(end, command), end += 8;
    bus.write32(kDpcStart, 0x1000);
    bus.write32(kDpcEnd, end);
    for (unsigned i = 0; i < 30; ++i) n64->step();
    CHECK(bus.read32(kDpcCurrent) == end);
    for (unsigned x = 0; x < 4; ++x) CHECK(bus.read16(0x6000 + 2 * x) == 0x0402);
}

namespace {

// Draws two one-row Z triangles over x = 0..3: a base surface (compare and update), then a
// second one with other modes. Returns the framebuffer row.
std::array<std::uint32_t, 4> draw_depth_pair(N64& n64, std::uint32_t base_dzdy, std::uint64_t second_modes,
                                             std::uint32_t second_z) {
    auto& bus = n64.bus();
    constexpr int pixel = 65536;
    for (std::uint32_t at = 0x6000; at < 0x6020; at += 4) bus.write32(at, 0xFFFCFFFC);
    std::vector<std::uint64_t> commands{
        0x3F18000300004000ULL,
        0x3E00000000006000ULL,
        rdp_rectangle(0x2D, 0, 0, 4, 4),
        0x2F0000C000000230ULL,
        0x3C00001F000000C0ULL,
    };
    const auto add = [&](std::uint32_t z, std::uint32_t dzdy, std::uint32_t color) {
        commands.push_back(0x3A00000000000000ULL | color);
        for (const auto word : {rdp_triangle_header(true, 0, 0, 4) | (0x01ULL << 56), rdp_edge(4 * pixel, 0),
                                rdp_edge(0, 0), rdp_edge(0, 0), std::uint64_t{z} << 32, std::uint64_t{dzdy}}) {
            commands.push_back(word);
        }
    };
    add(0x4000U << 10, base_dzdy, 0xFF0000FF);  // red base surface
    commands.push_back(second_modes);
    add(second_z << 10, 0, 0x00FF00FF);         // green
    commands.push_back(kSyncFull);
    std::uint32_t end = 0x1000;
    for (const auto command : commands) bus.write64(end, command), end += 8;
    bus.write32(kDpcStart, 0x1000);
    bus.write32(kDpcEnd, end);
    for (unsigned i = 0; i < 40; ++i) n64.step();
    CHECK(bus.read32(kDpcCurrent) == end);
    std::array<std::uint32_t, 4> row{};
    for (unsigned x = 0; x < 4; ++x) row[x] = bus.read32(0x4000 + 4 * x);
    return row;
}

}  // namespace

TEST_CASE("RDP keeps the low DZ bits in RDRAM hidden bits") {
    const auto n64 = N64::create(counting_rom("RDP HIDDEN DZ"));
    REQUIRE(n64);
    // DzDy 0x700 rounds to DZ 0x400, code 10: top bits 2 in RDRAM, low bits 2 hidden.
    // Memory DZ 0x400 widens to 0x800, so the decal range is 0x800 << 3 = 0x4000; losing the
    // hidden bits would leave 0x100 -> 0x200 and a range of only 0x1000.
    const auto row = draw_depth_pair(*n64, 0x07000000U, 0x2F0000C000000E10ULL, 0x4000 - 0x2000);
    for (unsigned x = 0; x < 4; ++x) {
        CAPTURE(x);
        CHECK(row[x] == 0x00FF00E0U);
        CHECK(n64->bus().read16(0x6000 + 2 * x) == 0x0402);
    }
    // Hidden bits are part of the save state.
    const auto state = n64->save_state();
    const auto restored = N64::create(counting_rom("RDP HIDDEN DZ"));
    REQUIRE(restored);
    REQUIRE(restored->load_state(state));
    CHECK(restored->save_state() == state);
}

TEST_CASE("RDP interpenetrating Z scales coverage where surfaces intersect") {
    const auto n64 = N64::create(counting_rom("RDP INTERPENETRATE"));
    REQUIRE(n64);
    // Base DZ 0x100 widens to 0x200 (shift 9). The green pixel is 0x400 in front, inside the
    // range: coverage becomes ((0x4000 >> 9) - (0x3C00 >> 9)) * 8 >> 3 = 2, stored as 1 by clamp.
    const auto row = draw_depth_pair(*n64, 0x01000000U, 0x2F0000C000000430ULL, 0x4000 - 0x400);
    for (unsigned x = 0; x < 4; ++x) {
        CAPTURE(x);
        CHECK(row[x] == 0x00FF0020U);
    }
}

TEST_CASE("RDP alpha compare keeps the depth buffer for dropped pixels") {
    const auto n64 = N64::create(counting_rom("RDP ZALPHA"));
    REQUIRE(n64);
    auto& bus = n64->bus();
    constexpr int pixel = 65536;
    for (std::uint32_t at = 0x6000; at < 0x6020; at += 4) bus.write32(at, 0xFFFCFFFC);
    const std::vector<std::uint64_t> commands{
        0x3F18000300004000ULL,
        0x3E00000000006000ULL,
        rdp_rectangle(0x2D, 0, 0, 4, 4),
        0x3900000000000080ULL,   // blend alpha threshold 0x80
        0x2F0000C000000231ULL,   // opaque Z, compare and update, alpha compare
        0x3C00001F000000C0ULL,   // alpha D = primitive alpha
        0x3A000000FF000040ULL,   // primitive alpha 0x40 is below the threshold
        rdp_triangle_header(true, 0, 0, 8) | (0x01ULL << 56), rdp_edge(4 * pixel, 0), rdp_edge(0, 0),
        rdp_edge(0, 0), std::uint64_t{0x4000U << 10} << 32, 0,
        kSyncFull,
    };
    std::uint32_t end = 0x1000;
    for (const auto command : commands) bus.write64(end, command), end += 8;
    bus.write32(kDpcStart, 0x1000);
    bus.write32(kDpcEnd, end);
    for (unsigned i = 0; i < 30; ++i) n64->step();
    CHECK(bus.read32(kDpcCurrent) == end);
    for (unsigned x = 0; x < 4; ++x) {
        CHECK(bus.read32(0x4000 + 4 * x) == 0);
        CHECK(bus.read16(0x6000 + 2 * x) == 0xFFFC);
    }
}

TEST_CASE("RDP one-cycle primitive color writes the coverage of fractional edges into RGBA8888") {
    const auto n64 = N64::create(counting_rom("RDP SOLID COVERAGE"));
    REQUIRE(n64);
    auto& bus = n64->bus();
    constexpr int pixel = 65536;
    constexpr std::array commands{
        std::uint64_t{0x3F18000300004000ULL},        // RGBA8888, stride 4
        rdp_rectangle(0x2D, 0, 0, 0, 2) | (9ULL << 12),  // scissor right = 2.25
        std::uint64_t{0x2F0000C000000008ULL},        // antialias, coverage clamp, RGB dither off
        std::uint64_t{0x3A000000316395FFULL},
        std::uint64_t{0x3C00001F000000C0ULL},        // combiner cycle 1: RGB = primitive
        rdp_triangle_header(true, 2, 2, 6),         // y = 0.5..1.5
        rdp_edge(5 * pixel / 2, 0),
        rdp_edge(pixel / 4 + 2, 0),                 // just beyond the sample at x = 0.25
        rdp_edge(0, 0),
        kSyncFull,
    };
    std::uint32_t end = 0x1000;
    for (const auto command : commands) {
        bus.write64(end, command);
        end += 8;
    }
    bus.write32(kDpcStart, 0x1000);
    bus.write32(kDpcEnd, end);
    for (unsigned i = 0; i < 20; ++i) n64->step();
    for (const auto row : {0x4000U, 0x4010U}) {
        CHECK(bus.read32(row) == 0x31639520);      // 2 samples, with the sticky edge bit
        CHECK(bus.read32(row + 4) == 0x31639560);  // 4 samples
        CHECK(bus.read32(row + 8) == 0x31639500);  // 1 sample, at x = 2
        CHECK(bus.read32(row + 12) == 0);
    }
    CHECK(bus.read64(0x4020) == 0);
}

TEST_CASE("N64 save state restores the one-cycle color mux and dither for an RGBA5551 triangle") {
    const auto n64 = N64::create(counting_rom("RDP SOLID STATE"));
    REQUIRE(n64);
    auto& bus = n64->bus();
    constexpr int pixel = 65536;
    constexpr std::array commands{
        std::uint64_t{0x3F10000300004000ULL},  // RGBA5551, stride 4
        rdp_rectangle(0x2D, 0, 0, 4, 1),
        std::uint64_t{0x2F00000000000200ULL},  // coverage zap, magic square RGB dither
        std::uint64_t{0x3B0000000F0010FFULL},  // environment color
        std::uint64_t{0x3C00001F00000140ULL},  // combiner cycle 1: RGB = environment
        rdp_triangle_header(true, 0, 0, 4),
        rdp_edge(4 * pixel, 0),
        rdp_edge(0, 0),
        rdp_edge(0, 0),
        std::uint64_t{0x3B000000FFFFFFFFULL},  // clobber color and mode after drawing
        kFillMode,
        kSyncFull,
    };
    std::uint32_t end = 0x1000;
    for (const auto command : commands) {
        bus.write64(end, command);
        end += 8;
    }
    bus.write32(kDpcStart, 0x1000);
    bus.write32(kDpcEnd, 0x1000 + 6 * 8);
    for (unsigned i = 0; i < 20; ++i) n64->step();
    REQUIRE(bus.read32(kDpcCurrent) == 0x1000 + 6 * 8);
    CHECK(bus.read64(0x4000) == 0);
    const auto state = n64->save_state();

    const auto resume = [&] {
        bus.write32(kDpcEnd, end);
        for (unsigned i = 0; i < 20; ++i) n64->step();
        CHECK(bus.read64(0x4000) == 0x1005100510050805ULL);
    };
    resume();
    const auto final_state = n64->save_state();
    REQUIRE(n64->load_state(state));
    resume();
    CHECK(n64->save_state() == final_state);
}

TEST_CASE("RDP copies a Load Tile subregion from saved TMEM with RGBA5551 alpha testing") {
    const auto n64 = N64::create(counting_rom("RDP COPY TILE"));
    REQUIRE(n64);
    auto& bus = n64->bus();
    constexpr std::uint32_t source = 0x2000, framebuffer = 0x4000, list = 0x1000;
    constexpr std::uint16_t background = 0x5295;
    constexpr std::array<std::uint16_t, 16> texels{
        0xF801, 0x07C1, 0x003E, 0xFFFF, 0x07C0, 0xF83F, 0x0001, 0xBDF7,
        0x001F, 0x0000, 0x7C01, 0x7FFE, 0x35A7, 0x03E1, 0xF800, 0xFF01,
    };
    for (unsigned y = 0; y < 2; ++y) {
        for (unsigned x = 0; x < 8; ++x) {
            bus.write16(source + 2 * (12 * (y + 1) + x + 2), texels[y * 8 + x]);
        }
    }
    for (unsigned i = 0; i < 48; ++i) bus.write16(framebuffer + 2 * i, background);

    constexpr std::uint64_t tile_select = 3ULL << 24;
    constexpr std::uint64_t load_tile = rdp_rectangle(0x34, 2, 1, 9, 2) | tile_select;
    constexpr std::array commands{
        std::uint64_t{0x3F10000B00004000ULL},  // RGBA5551 framebuffer, stride 12
        rdp_rectangle(0x2D, 0, 0, 12, 4),
        std::uint64_t{0x2F20000000000001ULL},  // COPY, alpha compare
        std::uint64_t{0x3D10000B00002000ULL},  // texture image, stride 12
        rdp_tile(0, 2, 3, 2, 16),
        load_tile,
        rdp_rectangle(0x24, 9, 2, 2, 1) | tile_select,
        rdp_texture_coords(2 * 32, 32, 4 * 1024, 1024),
        load_tile,  // overwrites TMEM from the now-cleared texture after the draw
        kSyncFull,
    };
    for (unsigned i = 0; i < commands.size(); ++i) bus.write64(list + 8 * i, commands[i]);
    bus.write32(kDpcStart, list);
    bus.write32(kDpcEnd, list + 7 * 8);  // stop between the rectangle's two words
    for (unsigned i = 0; i < 20; ++i) n64->step();
    REQUIRE(bus.read32(kDpcCurrent) == list + 7 * 8);
    CHECK(bus.read16(framebuffer + 2 * (12 + 2)) == background);
    for (unsigned i = 0; i < 36; ++i) bus.write16(source + 2 * i, 0);
    const auto state = n64->save_state();

    const auto resume = [&] {
        bus.write32(kDpcEnd, list + 10 * 8);
        for (unsigned i = 0; i < 20; ++i) n64->step();
        CHECK(bus.read32(kDpcCurrent) == list + 10 * 8);
        CHECK((bus.read32(0x04300008) & 0x20) != 0);
        for (unsigned y = 0; y < 4; ++y) {
            for (unsigned x = 0; x < 12; ++x) {
                CAPTURE(x);
                CAPTURE(y);
                std::uint16_t expected = background;
                if (y >= 1 && y <= 2 && x >= 2 && x <= 9) {
                    const auto texel = texels[(y - 1) * 8 + x - 2];
                    if (texel & 1) expected = texel;
                }
                CHECK(bus.read16(framebuffer + 2 * (y * 12 + x)) == expected);
            }
        }
    };
    resume();
    const auto end_state = n64->save_state();
    REQUIRE(n64->load_state(state));
    resume();
    CHECK(n64->save_state() == end_state);
}

TEST_CASE("RDP Load Block wraps TMEM and copies mirrored groups from a wrapped XBUS rectangle") {
    const auto n64 = N64::create(counting_rom("RDP COPY BLOCK"));
    REQUIRE(n64);
    auto& bus = n64->bus();
    bus.write64(0x2008, 0x0002100320054007ULL);  // first loaded row (texture T = 1)
    bus.write64(0x2010, 0x8001A003C005F007ULL);  // second row, swapped halves in TMEM
    constexpr std::uint64_t render_tile = 5ULL << 24;
    constexpr std::array commands{
        std::uint64_t{0x3F10000700004000ULL},  // framebuffer stride 8
        rdp_rectangle(0x2D, 0, 0, 8, 2),
        std::uint64_t{0x2F20000000000000ULL},  // COPY, all texels including zero alpha
        std::uint64_t{0x3D10000300002000ULL},  // texture stride 4
        rdp_tile(0, 2, 7, 0, 0x1FF),
        std::uint64_t{0x3300000107007800ULL},  // Load Block, integer T = 1, 8 texels, DXT = 0x800
        rdp_tile(0, 2, 5, 1, 0x1FF, 0x121),  // mirror S, mask 2, shift right 1
        rdp_rectangle(0x32, 0, 0, 3, 1) | render_tile,
        rdp_rectangle(0x24, 7, 1, 0, 0) | render_tile,
        rdp_texture_coords(-8 * 32, 0, 0, 1024),  // repeat the same four-texel group
        kSyncFull,
    };
    constexpr unsigned start = 0xFF8 - 8 * 8;  // rectangle header at DMEM 0xFF8, coefficients at 0
    for (unsigned i = 0; i < commands.size(); ++i) {
        bus.write64(0x04000000 | ((start + 8 * i) & 0xFFF), commands[i]);
    }
    bus.write32(kDpcStatus, 2);
    bus.write32(kDpcStart, start);
    const auto end = static_cast<std::uint32_t>(start + 8 * commands.size());
    bus.write32(kDpcEnd, end);
    for (unsigned i = 0; i < 20; ++i) n64->step();
    CHECK(bus.read32(kDpcCurrent) == end);
    CHECK(bus.read32(kDpcStatus) == 0x81);
    CHECK(bus.read64(0x4000) == 0x4007200510030002ULL);
    CHECK(bus.read64(0x4008) == 0x4007200510030002ULL);
    CHECK(bus.read64(0x4010) == 0xF007C005A0038001ULL);
    CHECK(bus.read64(0x4018) == 0xF007C005A0038001ULL);
}

TEST_CASE("RDP COPY uses the selected CI4 palette and restores it from TMEM") {
    const auto n64 = N64::create(counting_rom("RDP COPY CI4"));
    REQUIRE(n64);
    auto& bus = n64->bus();
    constexpr std::uint32_t source = 0x2000, palette = 0x3000, framebuffer = 0x4000, list = 0x1000;
    constexpr std::uint16_t background = 0xBDEF;
    constexpr unsigned palette_number = 11, texture_base = 13;
    constexpr std::array<std::uint16_t, 16> colors{
        0x0001, 0xF801, 0x07C1, 0x003E, 0xFFFF, 0x07C0, 0xF83F, 0x7FFE,
        0xBDF7, 0x001F, 0x0000, 0x7C01, 0x35A7, 0x03E1, 0xF800, 0xFF01,
    };
    for (unsigned i = 0; i < colors.size(); ++i) bus.write16(palette + 2 * i, colors[i]);
    for (unsigned i = 0; i < 16; ++i) bus.write16(framebuffer + 2 * i, background);
    bus.write64(source + 13, 0x0123456789ABCDEFULL);  // byte X = 1, Y = 1, pitch = 12
    bus.write64(source + 25, 0xFEDCBA9876543210ULL);

    constexpr std::uint64_t load_tile = 7ULL << 24, render_tile = 2ULL << 24;
    constexpr auto palette_image = 0x3D10000000000000ULL | palette;
    constexpr auto palette_tile = rdp_tile(0, 0, 7, 0, 0x100 + palette_number * 16);
    constexpr auto load_palette = rdp_rectangle(0x30, 0, 0, 15, 0) | load_tile;
    constexpr std::array<std::uint64_t, 16> commands{
        0x3F10000700000000ULL | framebuffer,
        rdp_rectangle(0x2D, 0, 0, 8, 2),
        0x2F20800000000001ULL,  // COPY, TLUT, alpha compare
        palette_image,
        palette_tile,
        load_palette,
        0x3D48000B00000000ULL | source,  // CI8 upload of packed CI4, 12 bytes per row
        rdp_tile(2, 1, 7, 1, texture_base),
        rdp_rectangle(0x34, 1, 1, 8, 2) | load_tile,
        rdp_tile(2, 0, 2, 1, texture_base, palette_number << 20),
        rdp_rectangle(0x32, 0, 0, 15, 1) | render_tile,
        rdp_rectangle(0x24, 7, 1, 0, 0) | render_tile,
        rdp_texture_coords(7 * 32, 0, 4 * 1024, 1024),
        palette_image,
        palette_tile,
        load_palette,  // clobber the palette in TMEM after drawing
    };
    const auto end = list + static_cast<std::uint32_t>(8 * commands.size());
    constexpr auto rectangle_midpoint = list + 8 * 12;
    for (unsigned i = 0; i < commands.size(); ++i) bus.write64(list + 8 * i, commands[i]);
    bus.write32(kDpcStart, list);
    bus.write32(kDpcEnd, rectangle_midpoint);
    for (unsigned i = 0; i < 40; ++i) n64->step();
    REQUIRE(bus.read32(kDpcCurrent) == rectangle_midpoint);
    for (unsigned i = 0; i < colors.size(); ++i) bus.write16(palette + 2 * i, 0);
    const auto state = n64->save_state();

    const auto draw = [&] {
        bus.write32(kDpcEnd, end);
        for (unsigned i = 0; i < 40; ++i) n64->step();
        CHECK(bus.read32(kDpcCurrent) == end);
        for (unsigned i = 0; i < 8; ++i) {
            CAPTURE(i);
            const auto top = colors[7 + i], bottom = colors[8 - i];
            CHECK(bus.read16(framebuffer + i * 2) == ((top & 1) ? top : background));
            CHECK(bus.read16(framebuffer + (8 + i) * 2) == ((bottom & 1) ? bottom : background));
        }
    };
    draw();
    const auto end_state = n64->save_state();
    REQUIRE(n64->load_state(state));
    draw();
    CHECK(n64->save_state() == end_state);
}

TEST_CASE("RDP COPY samples all CI8 index bits and reads each TLUT bank from TMEM") {
    const auto n64 = N64::create(counting_rom("RDP COPY CI8"));
    REQUIRE(n64);
    auto& bus = n64->bus();
    constexpr std::uint32_t source = 0x2000, palette = 0x3000, framebuffer = 0x4000, list = 0x1000;
    constexpr auto palette_entry = [](unsigned index) { return static_cast<std::uint16_t>(0x5000 + index); };
    for (unsigned i = 0; i < 256; ++i) bus.write16(palette + 2 * i, palette_entry(i));
    bus.write64(source,     0xFFFFFFFF0080FEFFULL);
    bus.write64(source + 8, 0x807FFF010C20FF80ULL);
    bus.write64(source + 16, 0xF80007C1003EFFFFULL);  // four distinct halfwords in palette entry 255

    constexpr std::uint64_t load_tile = 7ULL << 24, render_tile = 2ULL << 24;
    constexpr std::array<std::uint64_t, 15> commands{
        0x3F10000700000000ULL | framebuffer,
        rdp_rectangle(0x2D, 0, 0, 8, 2),
        0x2F20C00000000000ULL,  // COPY, IA16 TLUT, alpha compare disabled
        0x3D10000000000000ULL | palette,
        rdp_tile(0, 0, 7, 0, 0x100),
        rdp_rectangle(0x30, 0, 0, 255, 0) | load_tile,
        0x3D10000000000000ULL | (source + 16),
        rdp_tile(0, 2, 7, 0, 0x1FF),
        0x3300000007003000ULL,  // replace palette entry 255 via ordinary TMEM upload
        0x3D50000000000000ULL | source,  // packed CI8 uploaded as 16-bit units
        rdp_tile(2, 2, 7, 0, 8),
        0x3300000007007800ULL,  // eight 16-bit units, one 64-bit group per row
        rdp_tile(0, 1, 2, 1, 8, 13 << 20),  // TLUT with an RGBA tile and an ignored palette field
        rdp_rectangle(0x24, 7, 1, 0, 0) | render_tile,
        rdp_texture_coords(0, 0, 4 * 1024, 1024),
    };
    const auto end = list + static_cast<std::uint32_t>(8 * commands.size());
    for (unsigned i = 0; i < commands.size(); ++i) bus.write64(list + 8 * i, commands[i]);
    bus.write32(kDpcStart, list);
    bus.write32(kDpcEnd, end);
    for (unsigned i = 0; i < 40; ++i) n64->step();
    REQUIRE(bus.read32(kDpcCurrent) == end);
    constexpr std::array<std::uint16_t, 16> expected{
        0xF800, 0x07C1, 0x003E, 0xFFFF, palette_entry(0), palette_entry(0x80), palette_entry(0xFE), 0xFFFF,
        palette_entry(0x80), palette_entry(0x7F), 0x003E, palette_entry(1),
        palette_entry(0x0C), palette_entry(0x20), 0x003E, palette_entry(0x80),
    };
    for (unsigned i = 0; i < expected.size(); ++i) {
        CAPTURE(i);
        CHECK(bus.read16(framebuffer + i * 2) == expected[i]);
    }
}

TEST_CASE("RDP one-cycle rectangle point samples RGBA5551 with scaling and unclipped texture origin") {
    const auto n64 = N64::create(counting_rom("RDP POINT RGBA"));
    REQUIRE(n64);
    auto& bus = n64->bus();
    constexpr std::uint32_t source = 0x2000, framebuffer = 0x4000, list = 0x1000;
    constexpr std::uint32_t background = 0x12345678;
    for (unsigned i = 0; i < 32; ++i) bus.write32(framebuffer + i * 4, background);
    bus.write64(source,     0xF80007C18843003FULL);
    bus.write64(source + 8, 0x003F8D55FFFF0001ULL);

    constexpr std::uint64_t load_tile = 7ULL << 24, render_tile = 2ULL << 24;
    constexpr std::array<std::uint64_t, 10> commands{
        0x3F18000700000000ULL | framebuffer,  // RGBA8888, eight pixels per row
        rdp_rectangle(0x2D, 2, 1, 6, 4),
        kPointMode,
        0x3C00001F00000041ULL,  // direct TEXEL0 RGB and alpha in cycle 1
        0x3D10000300000000ULL | source,
        rdp_tile(0, 2, 7, 1, 20),
        rdp_rectangle(0x34, 0, 0, 3, 1) | load_tile,
        rdp_tile(0, 2, 2, 1, 20),
        rdp_rectangle(0x32, 4, 6, 7, 7) | render_tile,
        rdp_rectangle(0x24, 5, 3, 1, 0) | render_tile,
    };
    const auto rectangle_midpoint = list + static_cast<std::uint32_t>(8 * commands.size());
    const auto end = rectangle_midpoint + 8;
    for (unsigned i = 0; i < commands.size(); ++i) bus.write64(list + i * 8, commands[i]);
    bus.write64(rectangle_midpoint, rdp_texture_coords(4 * 32 + 8, 8 * 32, 512, -1024));
    bus.write32(kDpcStart, list);
    bus.write32(kDpcEnd, rectangle_midpoint);
    for (unsigned i = 0; i < 40; ++i) n64->step();
    REQUIRE(bus.read32(kDpcCurrent) == rectangle_midpoint);
    CHECK(bus.read32(framebuffer + (8 + 2) * 4) == background);
    const auto state = n64->save_state();

    const auto draw = [&] {
        bus.write32(kDpcEnd, end);
        for (unsigned i = 0; i < 8; ++i) n64->step();
        CHECK(bus.read32(kDpcCurrent) == end);
        constexpr std::array<std::uint32_t, 6> pixels{
            0x0000FFE0, 0x8CAD52E0, 0x8CAD52E0,
            0xFF0000E0, 0x00FF00E0, 0x00FF00E0,
        };
        for (unsigned y = 0; y < 4; ++y) {
            for (unsigned x = 0; x < 8; ++x) {
                CAPTURE(x);
                CAPTURE(y);
                const auto expected = y >= 1 && y <= 2 && x >= 2 && x <= 4
                    ? pixels[(y - 1) * 3 + x - 2] : background;
                CHECK(bus.read32(framebuffer + (y * 8 + x) * 4) == expected);
            }
        }
    };
    draw();
    const auto end_state = n64->save_state();
    REQUIRE(n64->load_state(state));
    draw();
    CHECK(n64->save_state() == end_state);
}

TEST_CASE("RDP one-cycle indexed rectangles modulate color and write fractional coverage") {
    for (unsigned size : {0U, 1U}) {
        CAPTURE(size);
        const auto n64 = N64::create(counting_rom("RDP POINT CI"));
        REQUIRE(n64);
        auto& bus = n64->bus();
        constexpr std::uint32_t source = 0x2000, palette = 0x3000, framebuffer = 0x4000, list = 0x1000;
        bus.write64(source, size == 0 ? 0x0123000000000000ULL : 0xF0F1F2F300000000ULL);
        bus.write64(palette, 0xFFFF07C1003F0001ULL);
        constexpr std::uint64_t load_tile = 7ULL << 24, render_tile = 2ULL << 24;
        constexpr std::uint64_t quarter_rectangle = 0x2400D00700001001ULL;  // (0.25, 0.25) to (3.25, 1.75)
        const std::array<std::uint64_t, 15> commands{
            (size == 0 ? 0x3F10000300000000ULL : 0x3F18000300000000ULL) | framebuffer,
            rdp_rectangle(0x2D, 0, 0, 4, 2),
            kPointMode | ((size == 0 ? 2ULL : 3ULL) << 46) | (1U << 3),  // RGBA16 or IA16 TLUT, AA enabled
            0x3C000023080001C1ULL,  // (TEXEL0 - 0) * PRIMITIVE + 0
            0x3A0000008040FFFFULL,
            0x3D10000000000000ULL | palette,
            rdp_tile(0, 0, 7, 0, 0x1F0),
            rdp_rectangle(0x30, 0, 0, 3, 0) | load_tile,
            0x3D50000300000000ULL | source,
            rdp_tile(2, 2, 7, 1, 15),
            rdp_rectangle(0x34, 0, 0, 3, 0) | load_tile,
            rdp_tile(2, size, 2, 1, 15, 15 << 20),
            rdp_rectangle(0x32, 0, 0, 3, 0) | render_tile,
            quarter_rectangle | render_tile,
            rdp_texture_coords(0, 0, 1024, 0),
        };
        const auto end = list + static_cast<std::uint32_t>(8 * commands.size());
        for (unsigned i = 0; i < commands.size(); ++i) bus.write64(list + i * 8, commands[i]);
        bus.write32(kDpcStart, list);
        bus.write32(kDpcEnd, end);
        for (unsigned i = 0; i < 40; ++i) n64->step();
        REQUIRE(bus.read32(kDpcCurrent) == end);
        if (size == 0) {
            CHECK(bus.read64(framebuffer) == 0x823F823F0201003EULL);
            CHECK(bus.read64(framebuffer + 8) == 0x823E823F0201003EULL);
        } else {
            CHECK(bus.read64(framebuffer) == 0x8040FE808040FEA0ULL);
            CHECK(bus.read64(framebuffer + 8) == 0x040207A000000000ULL);
            CHECK(bus.read64(framebuffer + 16) == 0x8040FE608040FEA0ULL);
            CHECK(bus.read64(framebuffer + 24) == 0x040207A000000020ULL);
        }
    }
}

TEST_CASE("RDP one-cycle rectangle selects both filter triangles and averages the midpoint") {
    const auto n64 = N64::create(counting_rom("RDP FILTER RGBA"));
    REQUIRE(n64);
    auto& bus = n64->bus();
    constexpr std::uint32_t source = 0x2000, framebuffer = 0x4000, list = 0x1000;
    constexpr std::uint32_t background = 0xAABBCCDD;
    for (unsigned i = 0; i < 15; ++i) bus.write32(framebuffer + i * 4, background);
    bus.write64(source, 0xF80107C1003FFFFFULL);  // red, green / blue, white

    constexpr std::uint64_t load_tile = 7ULL << 24, render_tile = 2ULL << 24;
    constexpr std::array<std::uint64_t, 17> commands{
        0x3F18000400000000ULL | framebuffer,
        rdp_rectangle(0x2D, 0, 0, 5, 3),
        kFilterMode,
        0x3C00001F00000041ULL,
        0x3D10000100000000ULL | source,
        rdp_tile(0, 2, 7, 1, 20),
        rdp_rectangle(0x34, 0, 0, 1, 1) | load_tile,
        rdp_tile(0, 2, 2, 1, 20),
        rdp_rectangle(0x32, 0, 0, 1, 1) | render_tile,
        rdp_rectangle(0x24, 3, 3, 0, 0) | render_tile,
        rdp_texture_coords(8, 8, 512, 512),  // 1/4, 3/4, then clamp on each axis
        kFilterMode | (1ULL << 44),
        rdp_rectangle(0x24, 5, 1, 3, 0) | render_tile,
        rdp_texture_coords(16, 16, 256, 0),  // average at the center, three taps off-center
        kFilterMode,
        rdp_rectangle(0x24, 4, 2, 3, 1) | render_tile,
        rdp_texture_coords(16, 16, 0, 0),  // diagonal interpolation with averaging disabled
    };
    const auto end = list + static_cast<std::uint32_t>(8 * commands.size());
    for (unsigned i = 0; i < commands.size(); ++i) bus.write64(list + i * 8, commands[i]);
    bus.write32(kDpcStart, list);
    bus.write32(kDpcEnd, end);
    for (unsigned i = 0; i < 50; ++i) n64->step();
    REQUIRE(bus.read32(kDpcCurrent) == end);
    constexpr std::array<std::uint32_t, 15> expected{
        0x804040E0, 0x00BF40E0, 0x40FF40E0, 0x808080E0, 0x40BF80E0,
        0x0040BFE0, 0x80BFBFE0, 0xBFFFBFE0, 0x008080E0, background,
        0x4040FFE0, 0xBFBFFFE0, 0xFFFFFFE0, background, background,
    };
    for (unsigned i = 0; i < expected.size(); ++i) {
        CAPTURE(i);
        CHECK(bus.read32(framebuffer + i * 4) == expected[i]);
    }
}

TEST_CASE("RDP one-cycle indexed filtering wraps S, mirrors T, and selects TLUT banks") {
    for (unsigned size : {0U, 1U}) {
        CAPTURE(size);
        const auto n64 = N64::create(counting_rom("RDP FILTER CI"));
        REQUIRE(n64);
        auto& bus = n64->bus();
        constexpr std::uint32_t source = 0x2000, palette = 0x3000, framebuffer = 0x4000, list = 0x1000;
        bus.write64(source, size == 0 ? 0x0100000000000000ULL : 0xF0F1000000000000ULL);
        bus.write64(source + 8, size == 0 ? 0x1000000000000000ULL : 0xF1F0000000000000ULL);
        bus.write16(palette, 0x2001);
        bus.write16(palette + 2, 0xA001);
        bus.write64(palette + 8, 0x200140018001E001ULL);  // four intensities for index 0xF0

        constexpr std::uint64_t load_tile = 7ULL << 24, render_tile = 2ULL << 24;
        constexpr unsigned sampler = (15U << 20) | (1U << 18) | (1U << 14) | (1U << 4);
        const std::array<std::uint64_t, 16> commands{
            0x3F18000100000000ULL | framebuffer,
            rdp_rectangle(0x2D, 0, 0, 2, 2),
            kFilterMode | (3ULL << 46),
            0x3C00001F00000041ULL,
            0x3D10000000000000ULL | palette,
            rdp_tile(0, 0, 7, 0, 0x1F0),
            rdp_rectangle(0x30, 0, 0, 1, 0) | load_tile,
            0x3D10000000000000ULL | (palette + 8),
            rdp_tile(0, 2, 7, 0, 0x1F0),
            0x3300000007003000ULL,  // replace the four banks of entry 0xF0
            0x3D48000700000000ULL | source,
            rdp_tile(2, 1, 7, 1, 15),
            rdp_rectangle(0x34, 0, 0, 7, 1) | load_tile,
            rdp_tile(2, size, 2, 1, 15, sampler),
            rdp_rectangle(0x24, 2, 2, 0, 0) | render_tile,
            rdp_texture_coords(40, 40, 512, 512),
        };
        const auto end = list + static_cast<std::uint32_t>(8 * commands.size());
        for (unsigned i = 0; i < commands.size(); ++i) bus.write64(list + i * 8, commands[i]);
        bus.write32(kDpcStart, list);
        bus.write32(kDpcEnd, end);
        for (unsigned i = 0; i < 50; ++i) n64->step();
        REQUIRE(bus.read32(kDpcCurrent) == end);
        CHECK(bus.read64(framebuffer) == 0x585858E0888888E0ULL);
        CHECK(bus.read64(framebuffer + 8) == 0x585858E0888888E0ULL);
    }
}

TEST_CASE("RDP textured triangle restores a partial packet and keeps its texture origin after scissoring") {
    const auto n64 = N64::create(counting_rom("RDP TRIANGLE ST"));
    REQUIRE(n64);
    auto& bus = n64->bus();
    constexpr std::uint32_t source = 0x2000, framebuffer = 0x4000, list = 0x1000;
    constexpr int pixel = 65536;
    bus.write64(source,      0xFFFFFFFFFFFFFFFFULL);
    bus.write64(source + 8,  0xF80107C1003FFFFFULL);
    bus.write64(source + 16, 0xF801F83F7FFEFFFFULL);
    bus.write64(source + 24, 0xF801FFC17FFEFFFFULL);

    constexpr std::array<std::uint64_t, 20> commands{
        0x3F18000500000000ULL | framebuffer,
        rdp_rectangle(0x2D, 1, 1, 6, 5),
        kPointMode,
        0x3C00001F00000041ULL,
        0x3D10000300000000ULL | source,
        rdp_tile(0, 2, 3, 1, 9),
        rdp_rectangle(0x34, 0, 0, 3, 3) | (3ULL << 24),
        rdp_triangle_header(true, 0, 0, 16) | (1ULL << 57) | (3ULL << 48),  // 0x0A, tile 3
        rdp_edge(5 * pixel, -pixel),
        rdp_edge(pixel / 4, 0),
        rdp_edge(0, 0),
        rdp_stw_halves(8, -16),  // S = 8.5, T = -16, in 10.5 coordinate units
        rdp_stw_halves(31, 0),   // dS/dX = 31.5
        rdp_stw_halves(0x8000, 0),
        rdp_stw_halves(0x8000, 0),
        rdp_stw_halves(0, 48),   // dT/dE = 48
        0,
        0,
        kSyncFull,  // ignored dS/dY fraction on this edge, never executed as a command
        kSyncFull,
    };
    constexpr auto midpoint = list + 8 * 16;
    constexpr auto triangle_end = list + 8 * 19;
    constexpr auto end = list + 8 * static_cast<std::uint32_t>(commands.size());
    for (unsigned i = 0; i < commands.size(); ++i) bus.write64(list + 8 * i, commands[i]);
    bus.write32(kDpcStart, list);
    bus.write32(kDpcEnd, midpoint);
    for (unsigned i = 0; i < 60; ++i) n64->step();
    REQUIRE(bus.read32(kDpcCurrent) == midpoint);
    CHECK(bus.read32(framebuffer + 7 * 4) == 0);
    // Replace coefficients already fetched from RDRAM before saving.
    for (unsigned i = 11; i < 16; ++i) bus.write64(list + 8 * i, 0);
    const auto state = n64->save_state();

    const auto resume = [&] {
        bus.write32(kDpcEnd, triangle_end);
        for (unsigned i = 0; i < 20; ++i) n64->step();
        CHECK(bus.read32(kDpcCurrent) == triangle_end);
        CHECK((bus.read32(0x04300008) & 0x20) == 0);
        constexpr std::array<std::array<std::uint32_t, 6>, 5> pixels{{
            {0, 0, 0, 0, 0, 0},
            {0, 0x00FF00E0, 0x00FF00E0, 0x0000FF60, 0, 0},
            {0, 0xFF00FFE0, 0xFF00FF60, 0, 0, 0},
            {0, 0xFFFF0060, 0, 0, 0, 0},
            {0, 0, 0, 0, 0, 0},
        }};
        for (unsigned y = 0; y < pixels.size(); ++y) {
            for (unsigned x = 0; x < pixels[y].size(); ++x) {
                CAPTURE(x);
                CAPTURE(y);
                CHECK(bus.read32(framebuffer + (6 * y + x) * 4) == pixels[y][x]);
            }
        }
        bus.write32(kDpcEnd, end);
        for (unsigned i = 0; i < 20; ++i) n64->step();
        CHECK(bus.read32(kDpcCurrent) == end);
        CHECK((bus.read32(0x04300008) & 0x20) != 0);
    };
    resume();
    const auto end_state = n64->save_state();
    REQUIRE(n64->load_state(state));
    resume();
    CHECK(n64->save_state() == end_state);
}

TEST_CASE("RDP filtered indexed triangle uses the right-major scanline latch and primitive color") {
    const auto n64 = N64::create(counting_rom("RDP TRIANGLE FILTER"));
    REQUIRE(n64);
    auto& bus = n64->bus();
    constexpr std::uint32_t source = 0x2000, palette = 0x3000, framebuffer = 0x4000, list = 0x1000;
    constexpr int pixel = 65536;
    bus.write64(source,     0x0001000000000000ULL);
    bus.write64(source + 8, 0x0203000000000000ULL);
    bus.write64(palette,    0xF80107C1003FFFFFULL);

    constexpr std::array<std::uint64_t, 25> commands{
        0x3F18000300000000ULL | framebuffer,
        rdp_rectangle(0x2D, 0, 0, 4, 2),
        kFilterMode | (1ULL << 47) | (1U << 9),  // RGBA16 palette, full stored coverage
        0x3C000023080001C1ULL,                  // (TEXEL0 - 0) * PRIMITIVE + 0
        0x3A000000808080FFULL,
        0x3D10000000000000ULL | palette,
        rdp_tile(0, 0, 7, 0, 0x100),
        rdp_rectangle(0x30, 0, 0, 3, 0) | (7ULL << 24),
        0x3D48000700000000ULL | source,
        rdp_tile(2, 1, 7, 1, 9),
        rdp_rectangle(0x34, 0, 0, 7, 1) | (7ULL << 24),
        rdp_tile(2, 1, 5, 1, 9),
        rdp_rectangle(0x32, 0, 0, 1, 1) | (5ULL << 24),
        rdp_triangle_header(false, 0, 0, 8) | (1ULL << 57) | (5ULL << 48),
        rdp_edge(0, 0),
        rdp_edge(2 * pixel, pixel / 2),
        rdp_edge(0, 0),
        rdp_stw_halves(40, -4),
        rdp_stw_halves(16, 0),
        0, 0,
        rdp_stw_halves(8, 16),  // the late latch applies 3/4 of the edge gradient minus the Y gradient
        0, 0, 0,
    };
    constexpr auto end = list + 8 * static_cast<std::uint32_t>(commands.size());
    for (unsigned i = 0; i < commands.size(); ++i) bus.write64(list + 8 * i, commands[i]);
    bus.write32(kDpcStart, list);
    bus.write32(kDpcEnd, end);
    for (unsigned i = 0; i < 60; ++i) n64->step();
    REQUIRE(bus.read32(kDpcCurrent) == end);
    CHECK(bus.read64(framebuffer) == 0x402020E0006020E0ULL);
    CHECK(bus.read64(framebuffer + 8) == 0);
    CHECK(bus.read64(framebuffer + 16) == 0x002060E0406060E0ULL);
    CHECK(bus.read64(framebuffer + 24) == 0x608060E000000000ULL);
}

TEST_CASE("RDP perspective triangle samples using the hardware reciprocal and W gradients") {
    for (const bool perspective : {true, false}) {
        CAPTURE(perspective);
        const auto n64 = N64::create(counting_rom("RDP TRIANGLE W"));
        REQUIRE(n64);
        auto& bus = n64->bus();
        constexpr std::uint32_t source = 0x2000, framebuffer = 0x4000, list = 0x1000;
        constexpr int pixel = 65536;
        bus.write64(source,      0xF80107C1003FFFFFULL);
        bus.write64(source + 8,  0xFFFF0001003FF83FULL);
        bus.write64(source + 16, 0xFFFF0001003FFFC1ULL);

        const std::array<std::uint64_t, 19> commands{
            0x3F18000300000000ULL | framebuffer,
            rdp_rectangle(0x2D, 0, 0, 4, 3),
            kPointMode | (std::uint64_t{perspective} << 51) | (1U << 9),
            0x3C00001F00000041ULL,
            0x3D10000300000000ULL | source,
            rdp_tile(0, 2, 3, 1, 9),
            rdp_rectangle(0x34, 0, 0, 3, 2) | (3ULL << 24),
            rdp_triangle_header(true, 0, 0, 24) | (1ULL << 57) | (3ULL << 48),
            rdp_edge(6 * pixel, -pixel),
            rdp_edge(0, 0),
            rdp_edge(0, 0),
            rdp_stw_halves(35, 12, 0x6600),
            rdp_stw_halves(0, 0, -0x2000),
            0, 0,
            rdp_stw_halves(0, 0, -0x0600),
            rdp_stw_halves(0, 0, -0x0600),
            0, 0,
        };
        const auto end = list + 8 * static_cast<std::uint32_t>(commands.size());
        for (unsigned i = 0; i < commands.size(); ++i) bus.write64(list + 8 * i, commands[i]);
        bus.write32(kDpcStart, list);
        bus.write32(kDpcEnd, end);
        for (unsigned i = 0; i < 60; ++i) n64->step();
        REQUIRE(bus.read32(kDpcCurrent) == end);
        constexpr std::uint32_t green = 0x00FF00E0, blue = 0x0000FFE0, magenta = 0xFF00FFE0, yellow = 0xFFFF00E0;
        constexpr std::array projected{
            green, green, magenta, yellow,  // S/W is 63 at W = 0x4600, one below exact division.
            green, blue, magenta, yellow,   // W is zero at the right edge.
            green, blue, magenta, yellow,   // W is negative at the right edge.
        };
        for (unsigned i = 0; i < projected.size(); ++i) {
            CAPTURE(i);
            CHECK(bus.read32(framebuffer + i * 4) == (perspective ? projected[i] : green));
        }
    }
}

TEST_CASE("RDP perspective triangle filters fractional W and saturates signed texture coordinates") {
    const auto n64 = N64::create(counting_rom("RDP TRIANGLE W FILTER"));
    REQUIRE(n64);
    auto& bus = n64->bus();
    constexpr std::uint32_t source = 0x2000, framebuffer = 0x4000, list = 0x1000;
    constexpr int pixel = 65536;
    bus.write64(source, 0xF80107C1003FFFFFULL);
    constexpr std::array<std::uint64_t, 19> commands{
        0x3F18000100000000ULL | framebuffer,
        rdp_rectangle(0x2D, 0, 0, 2, 2),
        kFilterMode | (1ULL << 51),
        0x3C00001F00000041ULL,
        0x3D10000100000000ULL | source,
        rdp_tile(0, 2, 3, 1, 9),
        rdp_rectangle(0x34, 0, 0, 1, 1) | (3ULL << 24),
        rdp_triangle_header(true, 0, 0, 8) | (1ULL << 57) | (3ULL << 48),
        rdp_edge(2 * pixel, 0),
        rdp_edge(0, 0),
        rdp_edge(0, 0),
        rdp_stw_halves(12, -12, 0x4000),
        rdp_stw_halves(0, 0, -0x1000),
        rdp_stw_halves(0, 0, 0xC000),  // W starts at 0x4000 + 0.75 in accumulator units.
        rdp_stw_halves(0, 0, 0x8000),  // dW/dX = -0x1000 + 0.5 in accumulator units.
        rdp_stw_halves(0, 0, -0x3FFF),
        rdp_stw_halves(0, 0, -0x3FFF),
        0, 0,
    };
    constexpr auto end = list + 8 * static_cast<std::uint32_t>(commands.size());
    for (unsigned i = 0; i < commands.size(); ++i) bus.write64(list + 8 * i, commands[i]);
    bus.write32(kDpcStart, list);
    bus.write32(kDpcEnd, end);
    for (unsigned i = 0; i < 60; ++i) n64->step();
    REQUIRE(bus.read32(kDpcCurrent) == end);
    CHECK(bus.read64(framebuffer) == 0x40BF00E008F700E0ULL);  // S/W = 24 and 31.
    CHECK(bus.read64(framebuffer + 8) == 0x00FF00E0FFFFFFE0ULL);
}

TEST_CASE("RDP resumes a version 7 state already skipping a textured triangle") {
    // Version 7 stored four draw words and skipped the remaining 11 words after a 0x0A header.
    std::array<std::uint32_t, 5> dpc{0, 8, 8, 0x81, 11};
    std::array<std::uint64_t, 2> clocks{};
    std::array<std::uint32_t, 7> counters_and_color_image{};
    std::uint64_t scissor = 0;
    std::uint32_t draw_words = 0;
    std::array<std::uint64_t, 4> draw_packet{};
    std::array<std::uint64_t, 2> modes_and_combiner{};
    std::array<std::uint32_t, 5> colors{};
    std::array<std::uint64_t, 17> texture_image_and_tiles{};
    std::array<std::uint16_t, 2048> tmem{};
    ezy::StateWriter writer;
    writer(dpc), writer(clocks), writer(counters_and_color_image), writer(scissor);
    writer(draw_words), writer(draw_packet), writer(modes_and_combiner), writer(colors);
    writer(texture_image_and_tiles), writer(tmem);
    const auto legacy = writer.take();

    Rdp rdp;
    ezy::StateReader reader(legacy);
    rdp.serialize(reader, 7);
    REQUIRE_FALSE(reader.failed());
    REQUIRE(reader.at_end());
    Scheduler scheduler;
    Mi mi;
    std::array<std::uint8_t, 0x1000> dmem{};
    for (unsigned offset = 8; offset <= 96; offset += 8) dmem[offset] = 0x29;  // Sync Full
    rdp.write(4, 104, scheduler, 0);
    for (unsigned offset = 8; offset <= 96; offset += 8) {
        scheduler.advance(3);
        const auto due = scheduler.pop_due();
        REQUIRE(due);
        rdp.step(mi, scheduler, {}, dmem, due->when);
        CHECK(rdp.read(8, due->when) == offset + 8);
        CHECK(((mi.read(8) & 0x20) != 0) == (offset == 96));
    }
}

TEST_CASE("save state round trip resumes identically") {
    const auto n64 = N64::create(counting_rom("STATE TEST"));
    REQUIRE(n64);
    n64->run_frame();
    const std::vector<std::uint8_t> state = n64->save_state();
    n64->run_frame();
    n64->run_frame();
    const std::uint64_t t1_a = n64->cpu().gpr(9);
    const std::uint64_t cycles_a = n64->cycles();
    REQUIRE(n64->load_state(state));
    n64->run_frame();
    n64->run_frame();
    CHECK(n64->cpu().gpr(9) == t1_a);
    CHECK(n64->cycles() == cycles_a);
}

TEST_CASE("save state mid-DMA resumes at the same cycle") {
    const auto n64 = N64::create(counting_rom("STATE TEST"));
    REQUIRE(n64);
    n64->bus().write32(0x04600000, 0x10000);      // PI_DRAM_ADDR
    n64->bus().write32(0x04600004, 0x10000000);   // PI_CART_ADDR
    n64->bus().write32(0x0460000C, 0x1FFF);       // 8 KB
    REQUIRE(n64->bus().pi().busy());
    const std::vector<std::uint8_t> state = n64->save_state();
    while (n64->bus().pi().busy()) n64->step();
    const std::uint64_t end_a = n64->cycles();
    REQUIRE(n64->load_state(state));
    CHECK(n64->bus().pi().busy());
    while (n64->bus().pi().busy()) n64->step();
    CHECK(n64->cycles() == end_a);
}

TEST_CASE("state of another ROM is refused") {
    const auto a = N64::create(counting_rom("GAME A"));
    const auto b = N64::create(counting_rom("GAME B"));
    REQUIRE(a);
    REQUIRE(b);
    a->run_frame();
    const std::uint64_t t1 = b->cpu().gpr(9);
    CHECK_FALSE(b->load_state(a->save_state()));
    CHECK(b->cpu().gpr(9) == t1);
}

TEST_CASE("a truncated state is refused and leaves the machine as it was") {
    const auto n64 = N64::create(counting_rom("STATE TEST"));
    n64->run_frame();
    std::vector<std::uint8_t> state = n64->save_state();
    state.resize(state.size() / 2);
    const std::uint64_t t1 = n64->cpu().gpr(9);
    CHECK_FALSE(n64->load_state(state));
    CHECK(n64->cpu().gpr(9) == t1);
}

TEST_CASE("create refuses a file that isn't an N64 ROM") {
    const std::vector<std::uint8_t> junk(0x2000, 0x55);
    CHECK_FALSE(N64::create(junk));
}
