#include <doctest/doctest.h>

#include <cstdint>

#include "mips_asm.hpp"
#include "n64/cpu/cop0.hpp"

using namespace mips;
using ezy::n64::Cop0;
using ezy::n64::ExceptionCode;

namespace {

constexpr int t0 = 8, t1 = 9;
constexpr std::uint32_t kTlbwi = 0x42000002, kTlbp = 0x42000008, kTlbr = 0x42000001;

// EntryLo: PFN, cache attribute 3 (cached), dirty, valid, global.
constexpr std::uint64_t entry_lo(std::uint32_t paddr, bool dirty = true, bool valid = true, bool global = false) {
    return (static_cast<std::uint64_t>(paddr >> 12) << 6) | (3U << 3) | (dirty ? 4U : 0U) | (valid ? 2U : 0U) |
           (global ? 1U : 0U);
}

// Writes TLB entry `index` straight through COP0 + TLBWI.
void map(CpuFixture& f, int index, std::uint64_t vpn2_vaddr, std::uint64_t lo0, std::uint64_t lo1,
         std::uint32_t page_mask = 0, std::uint8_t asid = 0) {
    Cop0& c = f.cpu.cop0();
    c.write(Cop0::kIndex, static_cast<std::uint64_t>(index));
    c.write(Cop0::kPageMask, page_mask);
    c.write(Cop0::kEntryHi, (vpn2_vaddr & ~std::uint64_t{0x1FFF}) | asid);
    c.write(Cop0::kEntryLo0, lo0);
    c.write(Cop0::kEntryLo1, lo1);
    f.load({kTlbwi}, 0x7000);
    f.cpu.set_pc(0xFFFFFFFF80007000ULL);
    f.cpu.step();
}

std::uint32_t exc_code(CpuFixture& f) { return static_cast<std::uint32_t>(f.cpu.cop0().read(Cop0::kCause) >> 2) & 31; }

}  // namespace

TEST_CASE("TLBWI then a load through the mapped 4 KB page") {
    CpuFixture f;
    f.cpu.cop0().write(Cop0::kStatus, 0);
    map(f, 5, 0x10000, entry_lo(0x20000), entry_lo(0x21000));
    f.bus.write32(0x20004, 0xCAFEBABE);
    f.bus.write32(0x21008, 0x12345678);
    f.cpu.set_gpr(t0, 0x10000);
    f.load({lw(t1, t0, 4), lw(t0, t0, 0x1008)});
    f.run(2);
    CHECK(f.cpu.gpr(t1) == 0xFFFFFFFFCAFEBABEULL);
    CHECK(f.cpu.gpr(t0) == 0x12345678);
}

TEST_CASE("16 KB page uses the odd entry for the upper half") {
    CpuFixture f;
    f.cpu.cop0().write(Cop0::kStatus, 0);
    map(f, 1, 0x40000, entry_lo(0x100000), entry_lo(0x200000), 0x6000);
    f.bus.write32(0x100000 + 0x3FFC, 0x11111111);
    f.bus.write32(0x200000 + 0x0010, 0x22222222);
    f.cpu.set_gpr(t0, 0x40000);
    f.load({lw(t1, t0, 0x3FFC), lw(t0, t0, 0x4010)});
    f.run(2);
    CHECK(f.cpu.gpr(t1) == 0x11111111);
    CHECK(f.cpu.gpr(t0) == 0x22222222);
}

TEST_CASE("global entry ignores ASID") {
    CpuFixture f;
    f.cpu.cop0().write(Cop0::kStatus, 0);
    map(f, 2, 0x10000, entry_lo(0x20000, true, true, true), entry_lo(0x21000, true, true, true), 0, 5);
    map(f, 3, 0x30000, entry_lo(0x22000), entry_lo(0x23000), 0, 5);
    f.bus.write32(0x20000, 0xAAAA);
    f.cpu.cop0().write(Cop0::kEntryHi, 7);  // another ASID
    f.cpu.set_gpr(t0, 0x10000);
    f.load({lw(t1, t0, 0)});
    f.run(1);
    CHECK(f.cpu.gpr(t1) == 0xAAAA);
    f.cpu.set_gpr(t0, 0x30000);
    f.load({lw(t1, t0, 0)});
    f.run(1);
    CHECK(exc_code(f) == static_cast<std::uint32_t>(ExceptionCode::TlbL));  // ASID 5 entry, not global
}

TEST_CASE("miss with EXL clear goes to the refill vector") {
    CpuFixture f;
    f.cpu.cop0().write(Cop0::kStatus, 0);
    f.cpu.cop0().write(Cop0::kEntryHi, 0x42);
    f.cpu.set_gpr(t0, 0x5000);
    f.load({lw(t1, t0, 0x10)});
    f.run(1);
    CHECK(f.cpu.pc() == 0xFFFFFFFF80000000ULL);
    CHECK(exc_code(f) == static_cast<std::uint32_t>(ExceptionCode::TlbL));
    CHECK(f.cpu.cop0().read(Cop0::kBadVAddr) == 0x5010);
    CHECK(f.cpu.cop0().read(Cop0::kEntryHi) == 0x4042);  // VPN2 of the address, ASID kept
    CHECK(f.cpu.cop0().read(Cop0::kContext) == (0x5010 >> 13) << 4);
}

TEST_CASE("miss with EXL set goes to the general vector") {
    CpuFixture f;
    f.cpu.cop0().write(Cop0::kStatus, 0x2);
    f.cpu.set_gpr(t0, 0x5000);
    f.load({sw(t1, t0, 0)});
    f.run(1);
    CHECK(f.cpu.pc() == 0xFFFFFFFF80000180ULL);
    CHECK(exc_code(f) == static_cast<std::uint32_t>(ExceptionCode::TlbS));
}

TEST_CASE("invalid entry goes to the general vector") {
    CpuFixture f;
    f.cpu.cop0().write(Cop0::kStatus, 0);
    map(f, 4, 0x10000, entry_lo(0x20000, true, false), entry_lo(0x21000, true, false));
    f.cpu.set_gpr(t0, 0x10000);
    f.load({lw(t1, t0, 0)});
    f.run(1);
    CHECK(f.cpu.pc() == 0xFFFFFFFF80000180ULL);
    CHECK(exc_code(f) == static_cast<std::uint32_t>(ExceptionCode::TlbL));
}

TEST_CASE("store to a clean page raises Mod") {
    CpuFixture f;
    f.cpu.cop0().write(Cop0::kStatus, 0);
    map(f, 6, 0x10000, entry_lo(0x20000, false), entry_lo(0x21000, false));
    f.cpu.set_gpr(t0, 0x10000);
    f.load({sw(t1, t0, 0)});
    f.run(1);
    CHECK(f.cpu.pc() == 0xFFFFFFFF80000180ULL);
    CHECK(exc_code(f) == static_cast<std::uint32_t>(ExceptionCode::Mod));
}

TEST_CASE("TLBP finds the matching entry and sets Index") {
    CpuFixture f;
    f.cpu.cop0().write(Cop0::kStatus, 0);
    map(f, 9, 0x10000, entry_lo(0x20000), entry_lo(0x21000));
    f.cpu.cop0().write(Cop0::kIndex, 0);
    f.cpu.cop0().write(Cop0::kEntryHi, 0x10000);
    f.load({kTlbp});
    f.run(1);
    CHECK(f.cpu.cop0().read(Cop0::kIndex) == 9);
}

TEST_CASE("TLBP miss sets Index bit 31") {
    CpuFixture f;
    f.cpu.cop0().write(Cop0::kStatus, 0);
    f.cpu.cop0().write(Cop0::kEntryHi, 0x77000);
    f.load({kTlbp});
    f.run(1);
    CHECK(f.cpu.cop0().read(Cop0::kIndex) == 0x80000000);
}

TEST_CASE("TLBR reads an entry back") {
    CpuFixture f;
    f.cpu.cop0().write(Cop0::kStatus, 0);
    map(f, 11, 0x10000, entry_lo(0x20000, true, true, true), entry_lo(0x21000, true, true, true), 0x6000, 3);
    Cop0& c = f.cpu.cop0();
    c.write(Cop0::kEntryHi, 0);
    c.write(Cop0::kEntryLo0, 0);
    c.write(Cop0::kPageMask, 0);
    c.write(Cop0::kIndex, 11);
    f.load({kTlbr});
    f.run(1);
    CHECK(c.read(Cop0::kPageMask) == 0x6000);
    CHECK((c.read(Cop0::kEntryHi) & ~std::uint64_t{0x7FFF}) == 0x10000 - 0x10000 % 0x8000);
    CHECK((c.read(Cop0::kEntryLo0) & 1) == 1);  // G
}

TEST_CASE("DADD from a user page in 32-bit user mode is reserved") {
    CpuFixture f;
    f.cpu.cop0().write(Cop0::kStatus, 0);
    map(f, 0, 0x10000, entry_lo(0x20000), entry_lo(0x21000));
    f.bus.write32(0x20000, dadd(t0, t0, t0));
    f.cpu.cop0().write(Cop0::kStatus, 0x10);  // user mode, UX = 0
    f.cpu.set_pc(0x10000);
    f.run(1);
    CHECK(exc_code(f) == static_cast<std::uint32_t>(ExceptionCode::RI));
    CHECK(f.cpu.cop0().read(Cop0::kEpc) == 0x10000);
}

TEST_CASE("user mode may not touch kseg0") {
    CpuFixture f;
    f.cpu.cop0().write(Cop0::kStatus, 0);
    map(f, 0, 0x10000, entry_lo(0x20000), entry_lo(0x21000));
    f.bus.write32(0x20000, lw(t1, t0, 0));
    f.cpu.set_gpr(t0, 0xFFFFFFFF80001000ULL);
    f.cpu.cop0().write(Cop0::kStatus, 0x10);
    f.cpu.set_pc(0x10000);
    f.run(1);
    CHECK(exc_code(f) == static_cast<std::uint32_t>(ExceptionCode::AdEL));
    CHECK(f.cpu.cop0().read(Cop0::kBadVAddr) == 0xFFFFFFFF80001000ULL);  // the load, not the fetch
}
