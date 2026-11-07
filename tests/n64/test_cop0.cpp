#include <doctest/doctest.h>

#include <cstdint>

#include "mips_asm.hpp"
#include "n64/cpu/cop0.hpp"

using namespace mips;
using ezy::n64::Cop0;
using ezy::n64::ExceptionCode;

namespace {

constexpr int t0 = 8, t1 = 9, t2 = 10;
constexpr std::uint64_t kGeneralVector = 0xFFFFFFFF80000180ULL;

// Kernel mode, BEV = 0, EXL = ERL = 0, interrupts off unless asked.
void kernel_ready(CpuFixture& f, std::uint32_t extra_status = 0) {
    f.cpu.cop0().write(Cop0::kStatus, extra_status);
}

std::uint32_t exc_code(CpuFixture& f) { return static_cast<std::uint32_t>(f.cpu.cop0().read(Cop0::kCause) >> 2) & 31; }

}  // namespace

TEST_CASE("exception in a delay slot sets BD and EPC to the branch") {
    CpuFixture f;
    kernel_ready(f);
    f.load({beq(0, 0, 4), syscall()});
    f.run(2);
    CHECK(f.cpu.pc() == kGeneralVector);
    CHECK(exc_code(f) == static_cast<std::uint32_t>(ExceptionCode::Sys));
    CHECK((f.cpu.cop0().read(Cop0::kCause) & 0x80000000) != 0);  // BD
    CHECK(f.cpu.cop0().read(Cop0::kEpc) == CpuFixture::kEntry);   // the branch
}

TEST_CASE("ERET returns to EPC and clears EXL") {
    CpuFixture f;
    kernel_ready(f, 0x2);  // EXL
    f.cpu.cop0().write(Cop0::kEpc, 0xFFFFFFFF80002000ULL);
    f.load({eret(), addiu(t0, 0, 1)});
    f.run(1);
    CHECK(f.cpu.pc() == 0xFFFFFFFF80002000ULL);  // no delay slot
    CHECK((f.cpu.cop0().read(Cop0::kStatus) & 0x2) == 0);
    f.run(1);
    CHECK(f.cpu.gpr(t0) == 0);
}

TEST_CASE("ADD overflow traps and leaves rd unchanged") {
    CpuFixture f;
    kernel_ready(f);
    f.cpu.set_gpr(t0, 0x7FFFFFFF);
    f.cpu.set_gpr(t1, 1);
    f.cpu.set_gpr(t2, 0x55);
    f.load({add(t2, t0, t1)});
    f.run(1);
    CHECK(exc_code(f) == static_cast<std::uint32_t>(ExceptionCode::Ov));
    CHECK(f.cpu.gpr(t2) == 0x55);
    CHECK(f.cpu.cop0().read(Cop0::kEpc) == CpuFixture::kEntry);
}

TEST_CASE("misaligned LW raises AdEL with BadVAddr") {
    CpuFixture f;
    kernel_ready(f);
    f.cpu.set_gpr(t0, 0xFFFFFFFF80003001ULL);
    f.load({lw(t1, t0, 0)});
    f.run(1);
    CHECK(exc_code(f) == static_cast<std::uint32_t>(ExceptionCode::AdEL));
    CHECK(f.cpu.cop0().read(Cop0::kBadVAddr) == 0xFFFFFFFF80003001ULL);
}

TEST_CASE("TEQ traps when equal") {
    CpuFixture f;
    kernel_ready(f);
    f.cpu.set_gpr(t0, 3);
    f.cpu.set_gpr(t1, 3);
    f.load({teq(t0, t1)});
    f.run(1);
    CHECK(exc_code(f) == static_cast<std::uint32_t>(ExceptionCode::Tr));
}

TEST_CASE("Count advances every 2 cycles and Compare raises IP7") {
    CpuFixture f;
    kernel_ready(f);
    f.cpu.cop0().write(Cop0::kCount, 0);
    f.cpu.cop0().write(Cop0::kCompare, 3);
    f.load({nop(), nop(), nop(), nop(), nop(), nop()});
    f.run(5);  // 5 cycles: Count = 2
    CHECK(f.cpu.cop0().read(Cop0::kCount) == 2);
    CHECK((f.cpu.cop0().read(Cop0::kCause) & 0x8000) == 0);
    f.run(1);  // Count = 3
    CHECK((f.cpu.cop0().read(Cop0::kCause) & 0x8000) != 0);
}

TEST_CASE("writing Compare clears IP7") {
    CpuFixture f;
    kernel_ready(f);
    f.cpu.cop0().write(Cop0::kCount, 0);
    f.cpu.cop0().write(Cop0::kCompare, 1);
    f.load({nop(), nop(), mtc0(0, Cop0::kCompare), nop()});
    f.run(2);
    CHECK((f.cpu.cop0().read(Cop0::kCause) & 0x8000) != 0);
    f.run(1);
    CHECK((f.cpu.cop0().read(Cop0::kCause) & 0x8000) == 0);
}

TEST_CASE("MI interrupt is taken when IM2 and IE are set") {
    CpuFixture f;
    kernel_ready(f, 0x0401);  // IM2, IE
    f.load({nop(), nop()});
    f.run(1);
    CHECK(f.cpu.pc() == CpuFixture::kEntry + 4);
    f.bus.irq = true;
    f.run(1);
    CHECK(f.cpu.pc() == kGeneralVector);
    CHECK(exc_code(f) == static_cast<std::uint32_t>(ExceptionCode::Int));
    CHECK((f.cpu.cop0().read(Cop0::kCause) & 0x400) != 0);  // IP2
    CHECK(f.cpu.cop0().read(Cop0::kEpc) == CpuFixture::kEntry + 4);
}

TEST_CASE("COP1 instruction with CU1 clear raises CpU with CE = 1") {
    CpuFixture f;
    kernel_ready(f);
    f.load({r(0x11, 0x00, t0, 0, 0, 0)});  // MFC1
    f.run(1);
    CHECK(exc_code(f) == static_cast<std::uint32_t>(ExceptionCode::CpU));
    CHECK(((f.cpu.cop0().read(Cop0::kCause) >> 28) & 3) == 1);
}

TEST_CASE("64-bit op in 32-bit user mode is reserved") {
    CpuFixture f;
    // User mode (KSU = 2) with UX = 0. (Running user code needs the TLB; test_tlb.cpp
    // executes a DADD from a mapped user page.)
    kernel_ready(f, 0x10);
    CHECK(f.cpu.allow_64bit_ops() == false);
    f.cpu.cop0().write(Cop0::kStatus, 0x10 | 0x2);  // EXL forces kernel mode
    CHECK(f.cpu.allow_64bit_ops());
    f.cpu.cop0().write(Cop0::kStatus, 0x10 | 0x20);  // UX
    CHECK(f.cpu.allow_64bit_ops());
    f.cpu.cop0().write(Cop0::kStatus, 0);  // kernel
    CHECK(f.cpu.allow_64bit_ops());
}

TEST_CASE("BEV moves the vector to 0xBFC00380") {
    CpuFixture f;
    kernel_ready(f, 0x00400000);  // BEV
    f.load({syscall()});
    f.run(1);
    CHECK(f.cpu.pc() == 0xFFFFFFFFBFC00380ULL);
}

TEST_CASE("unused COP0 registers read back the last value written to COP0") {
    CpuFixture f;
    kernel_ready(f);
    f.cpu.set_gpr(t0, 0x12345678);
    f.load({mtc0(t0, Cop0::kEpc), mfc0(t1, 7)});
    f.run(2);
    CHECK(f.cpu.gpr(t1) == 0x12345678);
}

TEST_CASE("LL sets LLAddr to the physical address >> 4") {
    CpuFixture f;
    kernel_ready(f);
    f.cpu.set_gpr(t0, 0xFFFFFFFF80003C04ULL);
    f.load({i(0x30, t0, t1, 0)});  // LL t1, 0(t0)
    f.run(1);
    CHECK(f.cpu.cop0().read(Cop0::kLLAddr) == 0x3C0);
}

// n64-systemtest probes for emulator extensions with COP0 function 0x20; on hardware the
// undefined COP0 functions do nothing.
TEST_CASE("an undefined COP0 function is a no-op") {
    CpuFixture f;
    kernel_ready(f);
    f.load({r(0x10, 0x10, 0, 0, 0, 0x20), addiu(t0, 0, 7)});
    f.run(2);
    CHECK(f.cpu.pc() == CpuFixture::kEntry + 8);
    CHECK(f.cpu.gpr(t0) == 7);
}

TEST_CASE("PRId and Random") {
    CpuFixture f;
    CHECK(f.cpu.cop0().read(Cop0::kPrId) == 0xB22);
    CHECK(f.cpu.cop0().read(Cop0::kRandom) == 31);
    f.load({nop()});
    f.run(1);
    CHECK(f.cpu.cop0().read(Cop0::kRandom) == 30);
}

// n64-systemtest "Unaligned LW exception": address errors fill Context like TLB misses.
TEST_CASE("address error sets Context's BadVPN2") {
    CpuFixture f;
    kernel_ready(f);
    f.cpu.set_gpr(t0, 0xFFFFFFFF807FE001ULL);
    f.load({lw(t1, t0, 0)});
    f.run(1);
    CHECK(f.cpu.cop0().read(Cop0::kContext) == ((0xFFFFFFFF807FE001ULL >> 13) & 0x7FFFF) << 4);
}

// n64-systemtest "COP2Usable": with CU2 set, the COP2 moves go through a 64-bit latch.
TEST_CASE("COP2 moves with CU2 set use a latch") {
    CpuFixture f;
    kernel_ready(f, 1U << 30);
    f.cpu.set_gpr(t0, 0x1234'5678'9ABC'DEF0ULL);
    f.load({r(0x12, 5, t0, 3, 0, 0) /* DMTC2 */, r(0x12, 0, t1, 7, 0, 0) /* MFC2 */, r(0x12, 1, t2, 7, 0, 0) /* DMFC2 */});
    f.run(3);
    CHECK(f.cpu.gpr(t1) == 0xFFFFFFFF9ABCDEF0ULL);
    CHECK(f.cpu.gpr(t2) == 0x123456789ABCDEF0ULL);
}
