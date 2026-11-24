#include <doctest/doctest.h>

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

#include "disc_builder.hpp"
#include "gc/gamecube.hpp"
#include "gc/gekko/gekko.hpp"
#include "gc/gx_fifo.hpp"
#include "gc/gx_vertex.hpp"
#include "gc/memory.hpp"
#include "gc/pi.hpp"
#include "gc/vi.hpp"

using namespace ezy::gc;

namespace {
// CP registers (Dolphin's CommandProcessor.h), 16 bits each; the FIFO ones are LO/HI pairs.
constexpr std::uint32_t kCpStatus = 0x00, kCpCtrl = 0x02, kCpClear = 0x04, kCpBase = 0x20, kCpEnd = 0x24,
                        kCpHiMark = 0x28, kCpLoMark = 0x2C, kCpDistance = 0x30, kCpWptr = 0x34, kCpRptr = 0x38,
                        kCpBreakpoint = 0x3C;
// CTRL bits (UCPCtrlReg) and STATUS bits (UCPStatusReg).
constexpr std::uint16_t kGpRead = 0x01, kBpEnable = 0x02, kOverflowInt = 0x04, kUnderflowInt = 0x08,
                        kGpLink = 0x10, kBpInt = 0x20;
constexpr std::uint16_t kHiMark = 0x01, kLoMark = 0x02, kReadIdle = 0x04, kCommandIdle = 0x08, kAtBreakpoint = 0x10;
// PE registers (Dolphin's PixelEngine.h) and CTRL bits (UPECtrlReg).
constexpr std::uint32_t kPeCtrl = 0x0A, kPeToken = 0x0E;
constexpr std::uint16_t kTokenEnable = 0x1, kFinishEnable = 0x2, kTokenAck = 0x4, kFinishAck = 0x8;
// PI registers.
constexpr std::uint32_t kPiMask = 0x04, kPiFifoBase = 0x0C, kPiFifoEnd = 0x10, kPiFifoWptr = 0x14, kPiFifoReset = 0x18;
// The opcode no command uses (Dolphin's OpcodeDecoding.h): filler that stops a mis-sized parse.
constexpr std::uint8_t kJunk = 0x18;

// A command stream, as the SDK's GX macros write it to the pipe.
struct Cmds {
    std::vector<std::uint8_t> b;
    Cmds& u8(std::uint32_t v) {
        b.push_back(static_cast<std::uint8_t>(v));
        return *this;
    }
    Cmds& u16(std::uint32_t v) { return u8(v >> 8).u8(v); }
    Cmds& u32(std::uint32_t v) { return u16(v >> 16).u16(v); }
    Cmds& nop(int n = 1) {
        for (int i = 0; i < n; ++i) u8(0x00);
        return *this;
    }
    Cmds& junk(int n) {
        for (int i = 0; i < n; ++i) u8(kJunk);
        return *this;
    }
    Cmds& cp(std::uint8_t reg, std::uint32_t value) { return u8(0x08).u8(reg).u32(value); }
    Cmds& bp(std::uint8_t reg, std::uint32_t value) { return u8(0x61).u32(std::uint32_t{reg} << 24 | (value & 0xFFFFFF)); }
    Cmds& xf(std::uint16_t address, int count) {
        u8(0x10).u32(std::uint32_t(count - 1) << 16 | address);
        for (int i = 0; i < count; ++i) u32(0x18181818);  // junk opcodes: mis-sizing shows
        return *this;
    }
    Cmds& call_dl(std::uint32_t address, std::uint32_t size) { return u8(0x40).u32(address).u32(size); }
    // A primitive whose vertex data is `bytes` junk bytes.
    Cmds& primitive(std::uint8_t opcode, std::uint16_t count, int bytes) { return u8(opcode).u16(count).junk(bytes); }
    // NOPs up to a multiple of 32 bytes (GXFlush pads with 32 zero bytes the same way).
    Cmds& pad() {
        while (b.size() % 32 != 0) u8(0x00);
        return *this;
    }
};

std::uint64_t count(const GxFifo& gx, GxCommand kind) { return gx.counters().commands[static_cast<std::size_t>(kind)]; }

// Pipe, PI and CP over a plain MEM1, without a CPU.
struct GxRig {
    Pi pi;
    std::vector<std::uint8_t> ram = std::vector<std::uint8_t>(Memory::kMem1Size, 0);
    GxFifo gx{pi, ram};

    void cp_write32(std::uint32_t reg, std::uint32_t value) {
        gx.cp().write16(reg, static_cast<std::uint16_t>(value));        // LO
        gx.cp().write16(reg + 2, static_cast<std::uint16_t>(value >> 16));  // HI
    }
    std::uint32_t cp_read32(std::uint32_t reg) {
        return std::uint32_t{gx.cp().read16(reg)} | std::uint32_t{gx.cp().read16(reg + 2)} << 16;
    }
    std::uint16_t status() { return gx.cp().read16(kCpStatus); }
    // One FIFO from `base` to `end` (the address of its last 32-byte block) that the CPU
    // writes and the CP reads, linked: what GXSetCPUFifo and GXSetGPFifo with one FIFO do.
    void linked_fifo(std::uint32_t base, std::uint32_t end, std::uint16_t extra_ctrl = 0) {
        pi.write32(kPiFifoBase, base);
        pi.write32(kPiFifoEnd, end);
        pi.write32(kPiFifoWptr, base);
        gx.cp().write16(kCpCtrl, 0);
        cp_write32(kCpBase, base);
        cp_write32(kCpEnd, end);
        cp_write32(kCpWptr, base);
        cp_write32(kCpRptr, base);
        cp_write32(kCpDistance, 0);
        cp_write32(kCpHiMark, end - base);
        cp_write32(kCpLoMark, 0);
        gx.cp().write16(kCpCtrl, kGpRead | kGpLink | extra_ctrl);
    }
    void send(const Cmds& cmds) { gx.write(cmds.b); }
    void put(std::uint32_t address, const Cmds& cmds) { std::copy(cmds.b.begin(), cmds.b.end(), ram.begin() + address); }
};

// The YUV bytes of an XFB row of `pixels` pixels (Y U Y V per pair).
std::vector<std::uint8_t> yuv_row(Yuv c, int pixels) {
    std::vector<std::uint8_t> row;
    for (int x = 0; x < pixels; x += 2) row.insert(row.end(), {c.y, c.u, c.y, c.v});
    return row;
}
bool row_is(const std::vector<std::uint8_t>& ram, std::uint32_t at, const std::vector<std::uint8_t>& bytes) {
    return std::equal(bytes.begin(), bytes.end(), ram.begin() + at);
}
bool all_bytes(const std::vector<std::uint8_t>& ram, std::uint32_t at, std::uint32_t n, std::uint8_t v) {
    return std::all_of(ram.begin() + at, ram.begin() + at + n, [v](std::uint8_t x) { return x == v; });
}
}  // namespace

TEST_CASE("pipe writes reach the FIFO in 32-byte bursts and wrap from end to base") {
    Memory mem;
    Pi pi;
    GxFifo gx(pi, mem.mem1());
    mem.set_write_gather_pipe(&gx);
    // A three-block FIFO; the CP is not reading (CTRL 0), so the data stays in RAM.
    pi.write32(kPiFifoBase, 0x10000);
    pi.write32(kPiFifoEnd, 0x10040);
    pi.write32(kPiFifoWptr, 0x10000);
    auto ram_at = [&](std::uint32_t a) { return mem.mem1()[a]; };

    // 31 bytes, every store size the CPU has: nothing reaches RAM yet.
    std::uint32_t k = 0;
    auto byte = [&] { return static_cast<std::uint8_t>(k++); };
    mem.write8(Memory::kPipeBase, byte());
    for (int i = 0; i < 3; ++i) {
        const std::uint16_t hi = byte();
        mem.write16(Memory::kPipeBase, static_cast<std::uint16_t>(hi << 8 | byte()));
    }
    for (int i = 0; i < 2; ++i) {
        std::uint32_t w = 0;
        for (int j = 0; j < 4; ++j) w = w << 8 | byte();
        mem.write32(Memory::kPipeBase, w);
    }
    for (int i = 0; i < 2; ++i) {
        std::uint64_t d = 0;
        for (int j = 0; j < 8; ++j) d = d << 8 | byte();
        mem.write64(Memory::kPipeBase + 0x10, d);  // any address in the pipe's page
    }
    REQUIRE(k == 31);
    CHECK(ram_at(0x10000) == 0);
    CHECK(ram_at(0x10001) == 0);
    CHECK(pi.fifo_write_pointer() == 0x10000);
    CHECK(gx.counters().fifo_bytes == 0);

    // The 32nd byte sends the burst, in order, and moves the write pointer one block on.
    mem.write8(Memory::kPipeBase, byte());
    for (std::uint32_t i = 0; i < 32; ++i) CHECK(ram_at(0x10000 + i) == i);
    CHECK(pi.fifo_write_pointer() == 0x10020);
    CHECK(gx.counters().fifo_bytes == 32);

    // Two more blocks: the second lands at the end register's address, then the pointer wraps.
    for (int i = 0; i < 16; ++i) {
        std::uint32_t w = 0;
        for (int j = 0; j < 4; ++j) w = w << 8 | byte();
        mem.write32(Memory::kPipeBase, w);
    }
    for (std::uint32_t i = 32; i < 96; ++i) CHECK(ram_at(0x10000 + i) == i);
    CHECK(pi.fifo_write_pointer() == 0x10000);
    for (int i = 0; i < 32; ++i) mem.write8(Memory::kPipeBase, byte());
    for (std::uint32_t i = 0; i < 32; ++i) CHECK(ram_at(0x10000 + i) == 96 + i);  // back at the base
    CHECK(ram_at(0x10060) == 0);                                                     // never past the end
    CHECK(pi.fifo_write_pointer() == 0x10020);
    CHECK(gx.counters().fifo_bytes == 128);

    SUBCASE("linked, the CP's write pointer and distance follow each burst within the CP's FIFO") {
        gx.cp().write16(kCpBase, 0x0000);
        gx.cp().write16(kCpBase + 2, 0x0001);
        gx.cp().write16(kCpEnd, 0x0040);
        gx.cp().write16(kCpEnd + 2, 0x0001);
        gx.cp().write16(kCpWptr, 0x0020);
        gx.cp().write16(kCpWptr + 2, 0x0001);
        gx.cp().write16(kCpCtrl, kGpLink);  // linked, not reading
        for (int i = 0; i < 32; ++i) mem.write8(Memory::kPipeBase, 0);
        CHECK(gx.cp().read16(kCpWptr) == 0x0040);
        CHECK(gx.cp().read16(kCpDistance) == 32);
        for (int i = 0; i < 32; ++i) mem.write8(Memory::kPipeBase, 0);
        CHECK(gx.cp().read16(kCpWptr) == 0x0000);  // wrapped to the CP's base
        CHECK(gx.cp().read16(kCpWptr + 2) == 0x0001);
        CHECK(gx.cp().read16(kCpDistance) == 64);
        CHECK(pi.fifo_write_pointer() == 0x10000);  // the PI's own pointer wrapped as well
    }
    SUBCASE("a burst outside MEM1 stops with its address") {
        pi.write32(kPiFifoBase, 0x01800000);
        pi.write32(kPiFifoEnd, 0x01800020);
        pi.write32(kPiFifoWptr, 0x01800000);
        for (int i = 0; i < 31; ++i) mem.write8(Memory::kPipeBase, 0);
        try {
            mem.write8(Memory::kPipeBase, 0);
            FAIL("no error");
        } catch (const BusError& e) {
            CHECK(e.word_or_address == 0x01800000);
            CHECK(std::string(e.what()).find("0x01800000") != std::string::npos);
        }
    }
}

TEST_CASE("primitive sizes from VCD/VAT") {
    constexpr std::uint32_t kDirect = 1, kIndex8 = 2, kIndex16 = 3;
    auto pos = [](std::uint32_t t) { return t << 9; };
    auto nrm = [](std::uint32_t t) { return t << 11; };
    auto col0 = [](std::uint32_t t) { return t << 13; };
    auto col1 = [](std::uint32_t t) { return t << 15; };
    auto tex = [](unsigned n, std::uint32_t t) { return t << (2 * n); };
    // VAT_A fields.
    constexpr std::uint32_t kXyz = 1, kNbt = 1u << 9, kTex0St = 1u << 21, kIndex3 = 1u << 31;
    auto pos_fmt = [](std::uint32_t f) { return f << 1; };
    auto nrm_fmt = [](std::uint32_t f) { return f << 10; };
    auto col0_fmt = [](std::uint32_t f) { return f << 14; };
    auto col1_fmt = [](std::uint32_t f) { return f << 18; };
    auto tex0_fmt = [](std::uint32_t f) { return f << 22; };
    constexpr std::uint32_t kU8 = 0, kS8 = 1, kU16 = 2, kS16 = 3, kF32 = 4;

    // The VAT group used is 3; the others hold values that would give other sizes.
    auto size = [](std::uint32_t lo, std::uint32_t hi, std::uint32_t a, std::uint32_t b = 0, std::uint32_t c = 0) {
        VertexFormat f;
        f.vcd_lo = lo;
        f.vcd_hi = hi;
        f.vat_a.fill(0xFFFFFFFF);
        f.vat_b.fill(0xFFFFFFFF);
        f.vat_c.fill(0xFFFFFFFF);
        f.vat_a[3] = a;
        f.vat_b[3] = b;
        f.vat_c[3] = c;
        return f.vertex_size(3);
    };

    CHECK(size(0, 0, 0) == 0u);
    // Position: direct, 2 or 3 components of the format's size; indexed, the index.
    CHECK(size(pos(kDirect), 0, kXyz | pos_fmt(kF32)) == 12u);
    CHECK(size(pos(kDirect), 0, pos_fmt(kS16)) == 4u);
    CHECK(size(pos(kDirect), 0, pos_fmt(kU8)) == 2u);
    CHECK(size(pos(kDirect), 0, kXyz | pos_fmt(kS8)) == 3u);
    CHECK(size(pos(kDirect), 0, kXyz | pos_fmt(5)) == 12u);  // invalid formats read as floats
    CHECK(size(pos(kIndex8), 0, kXyz | pos_fmt(kF32)) == 1u);
    CHECK(size(pos(kIndex16), 0, kXyz | pos_fmt(kF32)) == 2u);
    // Normal: N is 3 components, NBT 9; indexed NBT takes 3 indices only with NormalIndex3.
    CHECK(size(nrm(kDirect), 0, nrm_fmt(kS8)) == 3u);
    CHECK(size(nrm(kDirect), 0, kNbt | nrm_fmt(kS16)) == 18u);
    CHECK(size(nrm(kDirect), 0, kNbt | nrm_fmt(kF32) | kIndex3) == 36u);
    CHECK(size(nrm(kIndex8), 0, kNbt | kIndex3) == 3u);
    CHECK(size(nrm(kIndex16), 0, kNbt | kIndex3) == 6u);
    CHECK(size(nrm(kIndex16), 0, kNbt) == 2u);
    CHECK(size(nrm(kIndex8), 0, kIndex3) == 1u);
    // Colors: by format; indexed, the index; formats 6-7 do not exist.
    const std::uint32_t color_bytes[6] = {2, 3, 4, 2, 3, 4};
    for (std::uint32_t f = 0; f < 6; ++f) {
        CHECK(size(col0(kDirect), 0, col0_fmt(f)) == color_bytes[f]);
        CHECK(size(col1(kDirect), 0, col1_fmt(f)) == color_bytes[f]);
    }
    CHECK(size(col0(kIndex16), 0, col0_fmt(7)) == 2u);
    CHECK(size(col1(kIndex8), 0, 0) == 1u);
    CHECK_FALSE(size(col0(kDirect), 0, col0_fmt(6)).has_value());
    CHECK_FALSE(size(col1(kDirect), 0, col1_fmt(7)).has_value());
    CHECK(size(0, 0, col0_fmt(7)) == 0u);  // an absent attribute's format does not matter
    // Texture coordinates: s or st, of the format's size, each from its own VAT word.
    CHECK(size(0, tex(0, kDirect), kTex0St | tex0_fmt(kF32)) == 8u);
    CHECK(size(0, tex(0, kDirect), tex0_fmt(kU16)) == 2u);
    CHECK(size(0, tex(1, kDirect), 0, kU8 << 1) == 1u);                       // VAT_B bits 0-3
    CHECK(size(0, tex(2, kDirect), 0, 1u << 9 | kS16 << 10) == 4u);           // VAT_B bits 9-12
    CHECK(size(0, tex(3, kDirect), 0, 1u << 18 | kF32 << 19) == 8u);          // VAT_B bits 18-21
    CHECK(size(0, tex(4, kDirect), 0, 1u << 27 | kS16 << 28) == 4u);          // VAT_B bits 27-30
    CHECK(size(0, tex(5, kDirect), 0, 0, 1u << 5 | kU16 << 6) == 4u);         // VAT_C bits 5-8
    CHECK(size(0, tex(6, kDirect), 0, 0, kS8 << 15) == 1u);                   // VAT_C bits 14-17
    CHECK(size(0, tex(7, kDirect), 0, 0, 1u << 23 | kF32 << 24) == 8u);       // VAT_C bits 23-26
    CHECK(size(0, tex(6, kIndex16), 0, 0, 1u << 14 | kF32 << 15) == 2u);
    // Matrix indices: one byte each.
    CHECK(size(1u | 1u << 1 | 1u << 8, 0, 0) == 3u);
    // Everything together: position matrix + xyz f32 + N s8 + RGBA8888 + st s16 + s u8.
    CHECK(size(1u | pos(kDirect) | nrm(kDirect) | col0(kDirect), tex(0, kDirect) | tex(1, kDirect),
               kXyz | pos_fmt(kF32) | nrm_fmt(kS8) | col0_fmt(5) | kTex0St | tex0_fmt(kS16), kU8 << 1) ==
          1u + 12 + 3 + 4 + 4 + 1);

    // A triangle list of 3 vertices through the FIFO: xyz f32 + RGBA8888 + st s16 = 20 bytes
    // each, so the command is 3 + 60 bytes and the token after it is parsed where it is.
    GxRig rig;
    rig.linked_fifo(0x20000, 0x20FE0);
    Cmds cmds;
    cmds.cp(0x50, pos(kDirect) | col0(kDirect))
        .cp(0x60, tex(0, kDirect))
        .cp(0x71, kXyz | pos_fmt(kF32) | col0_fmt(5) | kTex0St | tex0_fmt(kS16))  // VAT_A, group 1
        .primitive(0x91, 3, 60)                                                     // triangles, VAT 1
        .bp(0x47, 0xBEEF)
        .pad();
    REQUIRE(cmds.b.size() == 96);  // 18 + 63 + 5 = 86, then 10 NOPs
    rig.send(cmds);
    CHECK(rig.gx.vertex_format().vat_a[1] == (kXyz | pos_fmt(kF32) | col0_fmt(5) | kTex0St | tex0_fmt(kS16)));
    CHECK(count(rig.gx, GxCommand::Primitive) == 1);
    CHECK(rig.gx.counters().primitives == 1);
    CHECK(rig.gx.counters().vertices == 3);
    CHECK(count(rig.gx, GxCommand::LoadCpReg) == 3);
    CHECK(count(rig.gx, GxCommand::LoadBpReg) == 1);
    CHECK(count(rig.gx, GxCommand::Nop) == 10);
    CHECK(rig.gx.pe().read16(kPeToken) == 0xBEEF);
    CHECK(rig.cp_read32(kCpDistance) == 0);
    CHECK(rig.cp_read32(kCpRptr) == 0x20060);

    // A vertex format the hardware does not define stops at the primitive.
    Cmds bad;
    bad.cp(0x72, col0_fmt(6)).primitive(0x92, 1, 0).pad();
    CHECK_THROWS_AS(rig.send(bad), BusError);
}

TEST_CASE("command crosses the FIFO end") {
    GxRig rig;
    rig.linked_fifo(0x20000, 0x20020);  // two blocks
    Cmds cmds;
    cmds.nop(58).xf(0x1000, 4).bp(0x47, 0x5A5A).pad();  // the XF load: bytes 58-78
    REQUIRE(cmds.b.size() == 96);
    // The first two blocks: the XF load's first 6 bytes are in the block at the end; it waits.
    rig.gx.write(std::span(cmds.b).first(64));
    CHECK(count(rig.gx, GxCommand::LoadXfReg) == 0);
    CHECK(count(rig.gx, GxCommand::Nop) == 58);
    CHECK(rig.cp_read32(kCpRptr) == 0x20000);  // the read pointer wrapped too
    CHECK(rig.cp_read32(kCpDistance) == 0);
    // The rest arrives at the base: the load runs once, whole, and the token after it.
    rig.gx.write(std::span(cmds.b).subspan(64));
    CHECK(count(rig.gx, GxCommand::LoadXfReg) == 1);
    CHECK(rig.gx.pe().read16(kPeToken) == 0x5A5A);
    CHECK(count(rig.gx, GxCommand::Nop) == 58 + 12);
    CHECK(rig.cp_read32(kCpRptr) == 0x20020);
    CHECK(rig.pi.fifo_write_pointer() == 0x20020);

    // A count field past 16 words is not a command the hardware takes.
    Cmds bad;
    bad.u8(0x10).u32(0x00101000).pad();
    CHECK_THROWS_AS(rig.send(bad), BusError);
}

TEST_CASE("CALL_DL parses a display list in RAM and returns") {
    GxRig rig;
    rig.linked_fifo(0x20000, 0x20FE0);
    rig.send(Cmds{}.cp(0x50, 1u << 9).cp(0x70, 0).pad());  // position direct, xy u8: 2 bytes

    SUBCASE("the list runs where the call is") {
        Cmds list;
        list.bp(0x47, 0x1111).primitive(0xB8, 2, 4).pad();  // a token, 2 points
        REQUIRE(list.b.size() == 32);
        rig.put(0x30000, list);
        rig.send(Cmds{}.call_dl(0x30000, 32).bp(0x47, 0x2222).pad());
        CHECK(rig.gx.pe().read16(kPeToken) == 0x2222);  // the list's token came first
        CHECK(count(rig.gx, GxCommand::CallDisplayList) == 1);
        CHECK(count(rig.gx, GxCommand::LoadBpReg) == 2);
        CHECK(rig.gx.counters().primitives == 1);
        CHECK(rig.gx.counters().vertices == 2);
        CHECK(count(rig.gx, GxCommand::Nop) == 20 + 20 + 18);  // the setup's, the list's, the call's
    }
    SUBCASE("address and size are 32-byte aligned") {
        rig.put(0x30000, Cmds{}.bp(0x47, 0x3333).pad());
        rig.send(Cmds{}.call_dl(0x3001F, 0x3F).pad());  // = 0x30000, 32 bytes
        CHECK(rig.gx.pe().read16(kPeToken) == 0x3333);
    }
    SUBCASE("a list that ends inside a command stops, naming the list") {
        Cmds list;
        list.nop(28).primitive(0xB8, 10, 1);  // 20 vertex bytes, only 1 in the list
        rig.put(0x30000, list);
        try {
            rig.send(Cmds{}.call_dl(0x30000, 32).pad());
            FAIL("no error");
        } catch (const BusError& e) {
            CHECK(e.word_or_address == 0x3001C);
            CHECK(std::string(e.what()).find("0x00030000") != std::string::npos);
        }
    }
    SUBCASE("a list inside a list stops") {
        rig.put(0x30000, Cmds{}.call_dl(0x30020, 32).pad());
        CHECK_THROWS_AS(rig.send(Cmds{}.call_dl(0x30000, 32).pad()), BusError);
    }
    SUBCASE("a list outside MEM1 stops with its address") {
        try {
            rig.send(Cmds{}.call_dl(0x017FFFE0, 64).pad());
            FAIL("no error");
        } catch (const BusError& e) {
            CHECK(e.word_or_address == 0x017FFFE0);
        }
    }
}

TEST_CASE("BP token and finish raise their PE interrupts") {
    GxRig rig;
    rig.linked_fifo(0x20000, 0x20FE0);
    rig.pi.write32(kPiMask, 1u << Pi::kPeToken | 1u << Pi::kPeFinish);
    auto& pe = rig.gx.pe();
    auto pending = [&](unsigned bit) { return (rig.pi.cause() >> bit & 1) != 0; };
    pe.write16(kPeCtrl, kTokenEnable | kFinishEnable);
    CHECK(pe.read16(kPeCtrl) == (kTokenEnable | kFinishEnable));

    // A token with interrupt: the token register and PI cause 9.
    rig.send(Cmds{}.bp(0x48, 0x1234).pad());
    CHECK(pe.read16(kPeToken) == 0x1234);
    CHECK(pending(Pi::kPeToken));
    CHECK(rig.pi.line());
    CHECK(pe.read16(kPeCtrl) == (kTokenEnable | kFinishEnable));  // the signal bits read 0
    pe.write16(kPeCtrl, kTokenEnable | kFinishEnable | kTokenAck);
    CHECK_FALSE(pending(Pi::kPeToken));
    CHECK_FALSE(rig.pi.line());

    // A token without interrupt only sets the register.
    rig.send(Cmds{}.bp(0x47, 0x5678).pad());
    CHECK(pe.read16(kPeToken) == 0x5678);
    CHECK_FALSE(pending(Pi::kPeToken));

    // Draw done: PI cause 10, acknowledged through bit 3.
    rig.send(Cmds{}.bp(0x45, 0x000002).pad());
    CHECK(pending(Pi::kPeFinish));
    CHECK_FALSE(pending(Pi::kPeToken));
    pe.write16(kPeCtrl, kTokenEnable | kFinishEnable | kFinishAck);
    CHECK_FALSE(pending(Pi::kPeFinish));

    // With the enables off the signal waits; enabling it raises the cause.
    pe.write16(kPeCtrl, 0);
    rig.send(Cmds{}.bp(0x48, 0x9999).bp(0x45, 0x02).pad());
    CHECK_FALSE(pending(Pi::kPeToken));
    CHECK_FALSE(pending(Pi::kPeFinish));
    pe.write16(kPeCtrl, kTokenEnable);
    CHECK(pending(Pi::kPeToken));
    CHECK_FALSE(pending(Pi::kPeFinish));
    pe.write16(kPeCtrl, kTokenEnable | kFinishEnable | kTokenAck | kFinishAck);  // both cleared at once
    CHECK_FALSE(pending(Pi::kPeToken));
    CHECK_FALSE(pending(Pi::kPeFinish));
    CHECK(rig.pi.interrupts(Pi::kPeToken) == 2);
    CHECK(rig.pi.interrupts(Pi::kPeFinish) == 1);

    // The BP mask (0xFE) applies to the next BP load only.
    rig.send(Cmds{}.bp(0xFE, 0x0000FF).bp(0x47, 0xABCD).pad());
    CHECK(pe.read16(kPeToken) == 0x56CD);  // 0x5678, the last 0x47 load, kept outside the mask
    CHECK(rig.gx.bp(0xFE) == 0xFFFFFF);
    rig.send(Cmds{}.bp(0x47, 0xABCD).pad());
    CHECK(pe.read16(kPeToken) == 0xABCD);
    CHECK(rig.gx.bp(0x47) == 0xABCD);

    // Draw done with another value is not modeled.
    CHECK_THROWS_AS(rig.send(Cmds{}.bp(0x45, 0x03).pad()), BusError);
}

TEST_CASE("EFB copy with clear writes the clear color to the XFB as YUV") {
    // RGB <-> YUV (BT.601): black and white come back exact.
    CHECK(rgb_to_yuv(0, 0, 0).y == 16);
    CHECK(rgb_to_yuv(0, 0, 0).u == 128);
    CHECK(rgb_to_yuv(0, 0, 0).v == 128);
    CHECK(rgb_to_yuv(255, 255, 255).y == 235);
    CHECK(rgb_to_yuv(255, 255, 255).u == 128);
    CHECK(rgb_to_yuv(255, 255, 255).v == 128);
    CHECK(yuv_to_argb(16, 128, 128) == 0xFF000000u);
    CHECK(yuv_to_argb(235, 128, 128) == 0xFFFFFFFFu);
    const Yuv red = rgb_to_yuv(255, 0, 0);
    CHECK(red.y == 82);
    CHECK(red.u == 90);
    CHECK(red.v == 240);

    GxRig rig;
    rig.linked_fifo(0x20000, 0x20FE0);
    std::fill(rig.ram.begin() + 0x40000, rig.ram.begin() + 0x40200, std::uint8_t{0xAA});
    constexpr std::uint32_t kToXfb = 1u << 14, kClear = 1u << 11, kScaleInvert = 1u << 10;
    // An 8x3 rectangle at (0, 0) to the XFB at 0x40000, 32 bytes per row, y scale 1.
    auto setup = [](std::uint32_t r, std::uint32_t g, std::uint32_t b) {
        Cmds c;
        c.bp(0x4F, 0xFF00 | r).bp(0x50, g << 8 | b).bp(0x49, 0).bp(0x4A, 7 | 2u << 10).bp(0x4B, 0x40000 >> 5).bp(0x4D, 1).bp(0x4E, 0x100);
        return c;
    };

    SUBCASE("white") {
        rig.send(setup(255, 255, 255).bp(0x52, kToXfb | kClear).pad());
        for (std::uint32_t row = 0; row < 3; ++row) {
            CHECK(row_is(rig.ram, 0x40000 + row * 32, yuv_row(Yuv{235, 128, 128}, 8)));
            CHECK(all_bytes(rig.ram, 0x40000 + row * 32 + 16, 16, 0xAA));  // past the width
        }
        CHECK(all_bytes(rig.ram, 0x40060, 32, 0xAA));  // past the height
        CHECK(yuv_to_argb(rig.ram[0x40000], rig.ram[0x40001], rig.ram[0x40003]) == 0xFFFFFFFFu);
    }
    SUBCASE("red, at an offset in the EFB") {
        Cmds c = setup(255, 0, 0);
        c.bp(0x49, 600 | 10u << 10).bp(0x4A, 63 | 0u << 10);  // x 600 + 64 wide: clamped to 40
        rig.send(c.bp(0x52, kToXfb | kClear).pad());
        CHECK(row_is(rig.ram, 0x40000, yuv_row(red, 40)));
        CHECK(all_bytes(rig.ram, 0x40000 + 80, 16, 0xAA));  // one row (height 1), 40 pixels
    }
    SUBCASE("the y scale stretches the rows") {
        // GXSetDispCopyYScale(2.0): 0x4E = 256 / 2 with scale_invert, so 1 + 2 * 2 rows.
        rig.send(setup(0, 0, 0).bp(0x4E, 0x80).bp(0x52, kToXfb | kClear | kScaleInvert).pad());
        for (std::uint32_t row = 0; row < 5; ++row) CHECK(row_is(rig.ram, 0x40000 + row * 32, yuv_row(Yuv{16, 128, 128}, 8)));
        CHECK(all_bytes(rig.ram, 0x400A0, 32, 0xAA));
    }
    SUBCASE("copies that do not clear, to a texture, or with no stride write nothing") {
        rig.send(setup(255, 255, 255).bp(0x52, kToXfb).bp(0x52, kClear).bp(0x4D, 0).bp(0x52, kToXfb | kClear).pad());
        CHECK(all_bytes(rig.ram, 0x40000, 0x200, 0xAA));
        CHECK(count(rig.gx, GxCommand::LoadBpReg) == 11);
    }
    SUBCASE("an XFB area past MEM1 stops with its address") {
        try {
            rig.send(setup(255, 255, 255).bp(0x4B, 0x017FFFC0 >> 5).bp(0x52, kToXfb | kClear).pad());
            FAIL("no error");
        } catch (const BusError& e) {
            CHECK(e.word_or_address == 0x017FFFC0);
        }
        CHECK(all_bytes(rig.ram, 0x017FFFC0, 0x40, 0x00));  // nothing written
    }
}

TEST_CASE("an unknown command opcode stops with its address") {
    GxRig rig;
    rig.linked_fifo(0x50000, 0x50FE0);
    try {
        rig.send(Cmds{}.nop(5).u8(kJunk).pad());
        FAIL("no error");
    } catch (const BusError& e) {
        CHECK(e.word_or_address == 0x50005);
        CHECK(std::string(e.what()).find("0x18") != std::string::npos);
        CHECK(std::string(e.what()).find("0x00050005") != std::string::npos);
    }

    // In a display list, the address is the list's byte.
    GxRig rig2;
    rig2.linked_fifo(0x50000, 0x50FE0);
    rig2.put(0x31000, Cmds{}.nop(3).u8(0x44).pad());  // Dolphin's "unknown metrics" is not modeled
    try {
        rig2.send(Cmds{}.call_dl(0x31000, 32).pad());
        FAIL("no error");
    } catch (const BusError& e) {
        CHECK(e.word_or_address == 0x31003);
    }
}

TEST_CASE("CP status, watermarks and the breakpoint interrupt") {
    GxRig rig;
    rig.linked_fifo(0x60000, 0x607E0);  // 64 blocks
    rig.pi.write32(kPiMask, 1u << Pi::kCp);
    auto cp_pending = [&] { return (rig.pi.cause() >> Pi::kCp & 1) != 0; };
    auto& cp = rig.gx.cp();
    CHECK(rig.status() == (kReadIdle | kCommandIdle));
    CHECK(cp.read16(kCpCtrl) == (kGpRead | kGpLink));
    CHECK(rig.cp_read32(kCpBase) == 0x60000);
    CHECK(rig.cp_read32(kCpEnd) == 0x607E0);
    // The pointers are physical and 32-byte aligned (LO & 0xFFE0, HI & 0x03FF).
    rig.cp_write32(kCpHiMark, 0xFFFFFFFF);
    CHECK(rig.cp_read32(kCpHiMark) == 0x03FFFFE0);
    rig.cp_write32(kCpHiMark, 0x40);

    SUBCASE("the low watermark: distance below it, interrupt when enabled") {
        rig.cp_write32(kCpLoMark, 0x40);
        CHECK(rig.status() == (kLoMark | kReadIdle | kCommandIdle));
        CHECK_FALSE(cp_pending());
        cp.write16(kCpCtrl, kGpRead | kGpLink | kUnderflowInt);
        CHECK(cp_pending());
        cp.write16(kCpClear, 0x3);  // accepted; the condition still holds
        CHECK(cp_pending());
        cp.write16(kCpCtrl, kGpRead | kGpLink);
        CHECK_FALSE(cp_pending());
    }
    SUBCASE("the breakpoint stops the reads, the high watermark rises, releasing drains it") {
        rig.cp_write32(kCpBreakpoint, 0x60040);
        cp.write16(kCpCtrl, kGpRead | kGpLink | kBpEnable | kBpInt | kOverflowInt);
        rig.send(Cmds{}.nop(64));  // two blocks, read up to the breakpoint
        CHECK(count(rig.gx, GxCommand::Nop) == 64);
        CHECK(rig.cp_read32(kCpRptr) == 0x60040);
        CHECK(rig.status() == (kAtBreakpoint | kReadIdle | kCommandIdle));
        CHECK(cp_pending());
        rig.send(Cmds{}.nop(96));  // three more blocks wait: 96 > the 64-byte high mark
        CHECK(count(rig.gx, GxCommand::Nop) == 64);
        CHECK(rig.cp_read32(kCpDistance) == 96);
        CHECK(rig.status() == (kHiMark | kAtBreakpoint | kCommandIdle));
        // Releasing the breakpoint reads the rest.
        cp.write16(kCpCtrl, kGpRead | kGpLink | kOverflowInt);
        CHECK(count(rig.gx, GxCommand::Nop) == 160);
        CHECK(rig.cp_read32(kCpDistance) == 0);
        CHECK(rig.status() == (kReadIdle | kCommandIdle));
        CHECK_FALSE(cp_pending());
    }
    SUBCASE("with reading off, nothing is read and no CP interrupt is raised") {
        cp.write16(kCpCtrl, kGpLink | kOverflowInt);
        rig.send(Cmds{}.nop(96));
        CHECK(count(rig.gx, GxCommand::Nop) == 0);
        CHECK(rig.status() == (kHiMark | kCommandIdle));
        CHECK_FALSE(cp_pending());
        cp.write16(kCpCtrl, kGpRead | kGpLink | kOverflowInt);  // turning reading on reads it
        CHECK(count(rig.gx, GxCommand::Nop) == 96);
        CHECK_FALSE(cp_pending());
    }
    SUBCASE("not linked: the CPU sets the CP's pointers and distance, the distance write runs it") {
        cp.write16(kCpCtrl, 0);
        rig.put(0x60000, Cmds{}.bp(0x47, 0x7777).pad());
        cp.write16(kCpCtrl, kGpRead);
        rig.cp_write32(kCpWptr, 0x60020);
        CHECK(rig.gx.pe().read16(kPeToken) == 0);
        rig.cp_write32(kCpDistance, 0x20);  // HI last: the CP reads
        CHECK(rig.gx.pe().read16(kPeToken) == 0x7777);
        CHECK(rig.cp_read32(kCpRptr) == 0x60020);
        // The pipe writes the PI's FIFO; unlinked, the CP's pointers stay.
        rig.send(Cmds{}.nop(32));
        CHECK(rig.cp_read32(kCpWptr) == 0x60020);
        CHECK(rig.cp_read32(kCpDistance) == 0);
    }
    // Registers nobody models, and 32-bit accesses, stop the machine.
    CHECK_THROWS_AS(cp.read16(0x40), BusError);
    CHECK_THROWS_AS(cp.read16(kCpClear), BusError);
    CHECK_THROWS_AS(cp.write16(kCpStatus, 0), BusError);
    CHECK_THROWS_AS(cp.read32(kCpBase), BusError);
    CHECK_THROWS_AS(cp.write32(kCpBase, 0), BusError);
    CHECK_THROWS_AS(rig.gx.pe().read16(0x10), BusError);  // bounding box
    CHECK_THROWS_AS(rig.gx.pe().write16(kPeToken, 0), BusError);
    CHECK_THROWS_AS(rig.gx.pe().read32(kPeCtrl), BusError);
    rig.gx.pe().write16(0x00, 0x1234);  // ZCONF: stored
    CHECK(rig.gx.pe().read16(0x00) == 0x1234);
}

TEST_CASE("the PI FIFO reset drops the gathered bytes and the CP's partial command") {
    GxRig rig;
    rig.pi.set_fifo_reset_sink([&] { rig.gx.reset_pipe(); });
    rig.linked_fifo(0x20000, 0x20FE0);
    rig.send(Cmds{}.nop(31).u8(0x61));  // a BP load's first byte ends the block
    rig.send(Cmds{}.junk(10));          // gathered, not sent
    rig.pi.write32(kPiFifoReset, 1);
    rig.send(Cmds{}.nop(32));
    CHECK(count(rig.gx, GxCommand::LoadBpReg) == 0);
    CHECK(count(rig.gx, GxCommand::Nop) == 63);
    CHECK(rig.gx.counters().fifo_bytes == 64);
    rig.pi.write32(kPiFifoReset, 0);  // bit 0 clear: nothing
    rig.send(Cmds{}.nop(16));
    rig.pi.write32(kPiFifoReset, 0);
    rig.send(Cmds{}.nop(16));
    CHECK(count(rig.gx, GxCommand::Nop) == 95);
}

TEST_CASE("the machine maps CP, PE and the pipe, and counts the FIFO per field") {
    DiscBuilder builder;
    const auto image = builder.write("gx_fifo");
    auto gc = GameCube::create(image);
    REQUIRE(gc != nullptr);
    auto& mem = gc->memory();
    constexpr std::uint32_t kCp = 0x0C000000, kPe = 0x0C001000, kPi = 0x0C003000;
    mem.write32(kPi + kPiFifoBase, 0x00100000);
    mem.write32(kPi + kPiFifoEnd, 0x00107FE0);
    mem.write32(kPi + kPiFifoWptr, 0x00100000);
    mem.write16(kCp + kCpBase, 0x0000);
    mem.write16(kCp + kCpBase + 2, 0x0010);
    mem.write16(kCp + kCpEnd, 0x7FE0);
    mem.write16(kCp + kCpEnd + 2, 0x0010);
    mem.write16(kCp + kCpWptr, 0x0000);
    mem.write16(kCp + kCpWptr + 2, 0x0010);
    mem.write16(kCp + kCpRptr, 0x0000);
    mem.write16(kCp + kCpRptr + 2, 0x0010);
    mem.write16(kCp + kCpHiMark, 0x4000);
    mem.write16(kCp + kCpCtrl, kGpRead | kGpLink);
    // A primitive with no attributes (0 bytes a vertex) and a token, as the CPU stores them.
    mem.write32(Memory::kPipeBase, 0x80000461);  // quads, 4 vertices; LOAD_BP_REG
    mem.write32(Memory::kPipeBase, 0x47000042);  // token 0x0042
    for (int i = 0; i < 6; ++i) mem.write32(Memory::kPipeBase, 0);
    CHECK(mem.read16(kPe + kPeToken) == 0x0042);
    Stats s = gc->stats();
    CHECK(s.fifo_bytes_this_field == 32);
    CHECK(s.gx_commands_this_field[static_cast<std::size_t>(GxCommand::LoadBpReg)] == 1);
    CHECK(s.gx_commands_this_field[static_cast<std::size_t>(GxCommand::Nop)] == 24);
    CHECK(s.gx_primitives_this_field == 1);
    CHECK(s.gx_vertices_this_field == 4);
    CHECK(mem.mem1()[0x00100000] == 0x80);  // the FIFO in RAM holds the data
    // The next field starts the counts again.
    gc->run_field();
    s = gc->stats();
    CHECK(s.fifo_bytes_this_field == 0);
    CHECK(s.gx_primitives_this_field == 0);
    CHECK(mem.read16(kPe + kPeToken) == 0x0042);
}
