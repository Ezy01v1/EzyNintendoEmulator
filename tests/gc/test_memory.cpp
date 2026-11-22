#include <doctest/doctest.h>

#include <cstdint>
#include <vector>

#include "gc/gekko/gekko.hpp"
#include "gc/memory.hpp"
#include "gc/mmio.hpp"
#include "ppc_asm.hpp"

using ezy::gc::BusError;
using ezy::gc::Gekko;
using ezy::gc::Memory;
using ezy::gc::MmioBlock;
using ezy::gc::MmioHandler;
using ezy::gc::WriteGatherPipe;

namespace {

// Records every access; reads return 0xA000 | offset (16) or 0xB0000000 | offset (32).
struct RecordingHandler final : MmioHandler {
    struct Access {
        char kind;  // 'r' or 'w'
        int bits;
        std::uint32_t offset;
        std::uint32_t value;
    };
    std::vector<Access> log;

    std::uint16_t read16(std::uint32_t offset) override {
        log.push_back({'r', 16, offset, 0});
        return static_cast<std::uint16_t>(0xA000 | offset);
    }
    std::uint32_t read32(std::uint32_t offset) override {
        log.push_back({'r', 32, offset, 0});
        return 0xB0000000 | offset;
    }
    void write16(std::uint32_t offset, std::uint16_t value) override { log.push_back({'w', 16, offset, value}); }
    void write32(std::uint32_t offset, std::uint32_t value) override { log.push_back({'w', 32, offset, value}); }
};

struct RecordingPipe final : WriteGatherPipe {
    std::vector<std::uint8_t> bytes;
    void write(std::span<const std::uint8_t> data) override { bytes.insert(bytes.end(), data.begin(), data.end()); }
};

}  // namespace

TEST_CASE("MEM1 is big-endian at every access size") {
    Memory memory;
    memory.write32(0x100, 0x11223344);
    CHECK(memory.read8(0x100) == 0x11);
    CHECK(memory.read8(0x103) == 0x44);
    CHECK(memory.read16(0x102) == 0x3344);
    memory.write64(0x108, 0x0102030405060708ull);
    CHECK(memory.read32(0x108) == 0x01020304);
    CHECK(memory.read64(0x108) == 0x0102030405060708ull);
    memory.write16(0x110, 0xBEEF);
    memory.write8(0x112, 0x7F);
    CHECK(memory.read32(0x110) == 0xBEEF7F00);
    CHECK(memory.mem1().size() == 0x01800000);
    CHECK(memory.mem1()[0x100] == 0x11);
}

TEST_CASE("MMIO accesses go to the handler registered for their block, by offset") {
    Memory memory;
    RecordingHandler vi, si, ai, cp;
    memory.map_mmio(MmioBlock::Vi, &vi);
    memory.map_mmio(MmioBlock::Si, &si);
    memory.map_mmio(MmioBlock::Ai, &ai);
    memory.map_mmio(MmioBlock::Cp, &cp);

    CHECK(memory.read32(0x0C00202C) == 0xB000002C);
    CHECK(memory.read16(0x0C002002) == 0xA002);
    memory.write32(0x0C006438, 0x80000000);  // SI 0x38
    memory.write16(0x0C006C04, 0x1234);      // AI 0x04
    memory.write16(0x0C000002, 0x5678);      // CP 0x02
    REQUIRE(vi.log.size() == 2);
    CHECK(vi.log[0].offset == 0x2C);
    CHECK(vi.log[1].offset == 0x02);
    CHECK(vi.log[1].bits == 16);
    REQUIRE(si.log.size() == 1);
    CHECK(si.log[0].kind == 'w');
    CHECK(si.log[0].offset == 0x38);
    CHECK(si.log[0].value == 0x80000000);
    REQUIRE(ai.log.size() == 1);
    CHECK(ai.log[0].offset == 0x04);
    CHECK(ai.log[0].value == 0x1234);
    REQUIRE(cp.log.size() == 1);
    CHECK(cp.log[0].offset == 0x02);

    // Every block's base address.
    CHECK(Memory::mmio_block_base(MmioBlock::Pe) == 0x0C001000);
    CHECK(Memory::mmio_block_base(MmioBlock::Pi) == 0x0C003000);
    CHECK(Memory::mmio_block_base(MmioBlock::Mi) == 0x0C004000);
    CHECK(Memory::mmio_block_base(MmioBlock::Dsp) == 0x0C005000);
    CHECK(Memory::mmio_block_base(MmioBlock::Di) == 0x0C006000);
    CHECK(Memory::mmio_block_base(MmioBlock::Exi) == 0x0C006800);

    // 8-bit and 64-bit MMIO accesses throw (the hardware registers are 16/32-bit).
    CHECK_THROWS_AS(memory.read8(0x0C002000), BusError);
    CHECK_THROWS_AS(memory.write8(0x0C002000, 1), BusError);
    CHECK_THROWS_AS(memory.read64(0x0C002000), BusError);
    CHECK(vi.log.size() == 2);

    // An unregistered block throws with the address; unmapping works.
    try {
        (void)memory.read32(0x0C003000);  // PI: not registered
        FAIL("expected BusError");
    } catch (const BusError& e) {
        CHECK(e.word_or_address == 0x0C003000);
    }
    memory.map_mmio(MmioBlock::Vi, nullptr);
    CHECK_THROWS_AS(memory.read32(0x0C00202C), BusError);
}

TEST_CASE("the write-gather pipe takes writes of every size in big-endian order") {
    Memory memory;
    CHECK_THROWS_AS(memory.write32(0x0C008000, 0), BusError);  // nothing behind it yet
    RecordingPipe pipe;
    memory.set_write_gather_pipe(&pipe);
    memory.write8(0x0C008000, 0x61);
    memory.write16(0x0C008000, 0x0102);
    memory.write32(0x0C008000, 0x03040506);
    memory.write64(0x0C008000, 0x0708090A0B0C0D0Eull);
    const std::vector<std::uint8_t> expected = {0x61, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14};
    CHECK(pipe.bytes == expected);
    CHECK_THROWS_AS(memory.read32(0x0C008000), BusError);  // write-only
}

TEST_CASE("the locked cache is 16 KiB at physical 0xE0000000") {
    Memory memory;
    memory.write32(0xE0000000, 0xCAFEBABE);
    memory.write64(0xE0003FF8, 0x1122334455667788ull);
    CHECK(memory.read32(0xE0000000) == 0xCAFEBABE);
    CHECK(memory.read8(0xE0003FFF) == 0x88);
    CHECK(memory.read32(0x00000000) == 0);  // separate from MEM1
}

TEST_CASE("mtspr DMAL moves locked-cache lines to and from MEM1") {
    Memory memory;
    Gekko cpu(memory);
    cpu.set_msr(0);
    using namespace ppc;
    for (std::uint32_t i = 0; i < 0x40; i += 4) memory.write32(0x10000 + i, 0xA0000000 | i);

    // Load 2 lines (64 bytes) from MEM1 0x10000 into the cache at 0xE0000100: DMAU = address |
    // LEN_U (0), DMAL = cache address | LEN_L (2) << 2 | LD (0x10) | T (2).
    cpu.set_gpr(3, 0x00010000);
    cpu.set_gpr(4, 0xE0000100 | (2 << 2) | 0x10 | 0x2);
    const std::uint32_t at = 0x3000;
    std::uint32_t a = at;
    for (const std::uint32_t w : {mtspr(ezy::gc::spr::kDmaU, 3), mtspr(ezy::gc::spr::kDmaL, 4)}) {
        memory.write32(a, w);
        a += 4;
    }
    cpu.set_pc(at);
    cpu.step();
    cpu.step();
    for (std::uint32_t i = 0; i < 0x40; i += 4) CHECK(memory.read32(0xE0000100 + i) == (0xA0000000 | i));
    CHECK(memory.read32(0xE0000140) == 0);  // only 2 lines
    CHECK((cpu.spr(ezy::gc::spr::kDmaL) & 3) == 0);  // T and F read back clear

    // Store 1 line back to MEM1 0x20000 (LD = 0), through set_spr.
    cpu.set_spr(ezy::gc::spr::kDmaU, 0x00020000);
    cpu.set_spr(ezy::gc::spr::kDmaL, 0xE0000100 | (1 << 2) | 0x2);
    for (std::uint32_t i = 0; i < 0x20; i += 4) CHECK(memory.read32(0x20000 + i) == (0xA0000000 | i));
    CHECK(memory.read32(0x20020) == 0);

    // Length 0 means 128 lines (4 KiB); without T nothing moves.
    for (std::uint32_t i = 0; i < 0x1000; i += 4) memory.write32(0x30000 + i, i + 1);
    cpu.set_spr(ezy::gc::spr::kDmaU, 0x00030000);
    cpu.set_spr(ezy::gc::spr::kDmaL, 0xE0001000 | 0x10);
    CHECK(memory.read32(0xE0001000) == 0);
    cpu.set_spr(ezy::gc::spr::kDmaL, 0xE0001000 | 0x10 | 0x2);
    CHECK(memory.read32(0xE0001000) == 1);
    CHECK(memory.read32(0xE0001FFC) == 0xFFD);
    CHECK(memory.read32(0xE0002000) == 0);
}

TEST_CASE("a bus error from executing code reports that instruction's pc") {
    Memory memory;
    Gekko cpu(memory);
    cpu.set_msr(0);
    using namespace ppc;
    // A load from an address with nothing behind it, as the third instruction.
    const std::uint32_t at = 0x3000;
    const std::uint32_t program[] = {lis(4, 0x0D00), li(3, 1), lwz(5, 0x10, 4)};
    for (std::uint32_t i = 0; i < 3; ++i) memory.write32(at + 4 * i, program[i]);
    cpu.set_pc(at);
    cpu.step();
    cpu.step();
    try {
        cpu.step();
        FAIL("expected BusError");
    } catch (const BusError& e) {
        CHECK(e.pc_known);
        CHECK(e.pc == at + 8);
        CHECK(e.word_or_address == 0x0D000010);
        CHECK(std::string(e.what()).find("0x00003008") != std::string::npos);
        CHECK(std::string(e.what()).find("0x0D000010") != std::string::npos);
    }
    // MMIO without a handler, through a store with translation on (DBAT1 0xC0000000 -> 0).
    cpu.set_spr(ezy::gc::spr::kDbat1u, 0xC0001FFF);
    cpu.set_spr(ezy::gc::spr::kDbat1l, 0x0000002A);
    cpu.set_msr(Gekko::kMsrDr);
    memory.write32(0x3100, lis(4, static_cast<std::int32_t>(0xCC00)));
    memory.write32(0x3104, stw(3, 0x2000, 4));
    cpu.set_pc(0x3100);
    cpu.step();
    try {
        cpu.step();
        FAIL("expected BusError");
    } catch (const BusError& e) {
        CHECK(e.pc == 0x3104);
        CHECK(e.word_or_address == 0x0C002000);
    }
    // An instruction fetch from an unmapped address (translation off).
    cpu.set_msr(0);
    cpu.set_pc(0x01800000);
    try {
        cpu.step();
        FAIL("expected BusError");
    } catch (const BusError& e) {
        CHECK(e.pc == 0x01800000);
        CHECK(e.word_or_address == 0x01800000);
    }
    // Outside step(), the pc is not known.
    try {
        (void)cpu.bus().read32(0x01800000);
        FAIL("expected BusError");
    } catch (const BusError& e) {
        CHECK_FALSE(e.pc_known);
        CHECK(std::string(e.what()).find("pc unknown") != std::string::npos);
    }
}
