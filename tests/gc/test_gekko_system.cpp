#include <doctest/doctest.h>

#include <cstdint>
#include <initializer_list>

#include "ppc_asm.hpp"

using namespace ppc;
using ezy::gc::Gekko;
using ezy::gc::GekkoError;
namespace spr = ezy::gc::spr;

namespace {

constexpr std::uint32_t kEe = Gekko::kMsrEe, kPr = Gekko::kMsrPr, kMe = Gekko::kMsrMe, kIp = Gekko::kMsrIp,
                        kIr = Gekko::kMsrIr, kDr = Gekko::kMsrDr;

// The BATs the GameCube IPL leaves (and the HLE boot will set): 256 MiB of RAM at
// 0x80000000 (cached) and at 0xC0000000 (uncached), instructions only at 0x80000000.
void boot_bats(Gekko& cpu) {
    cpu.set_spr(spr::kIbat0u, 0x80001FFF);
    cpu.set_spr(spr::kIbat0l, 0x00000002);
    cpu.set_spr(spr::kDbat0u, 0x80001FFF);
    cpu.set_spr(spr::kDbat0l, 0x00000002);
    cpu.set_spr(spr::kDbat1u, 0xC0001FFF);
    cpu.set_spr(spr::kDbat1l, 0x0000002A);  // WIMG = I+G, read/write
}

// Loads a program at physical `at` and starts it at virtual `pc`.
void load_at(GekkoFixture& f, std::uint32_t at, std::uint32_t pc, std::initializer_list<std::uint32_t> words) {
    f.load(words, at);
    f.cpu.set_pc(pc);
}

}  // namespace

TEST_CASE("BAT translation maps 0x80000000 and 0xC0000000 to physical 0") {
    GekkoFixture f;
    auto& cpu = f.cpu;

    // Translation off: identity.
    boot_bats(cpu);
    CHECK(cpu.translate(0x80001234, false, false) == 0x80001234u);
    CHECK(cpu.translate(0x90001234, true, false) == 0x90001234u);

    cpu.set_msr(kIr | kDr);
    CHECK(cpu.translate(0x80001234, false, false) == 0x1234u);
    CHECK(cpu.translate(0xC0001234, false, true) == 0x1234u);
    CHECK(cpu.translate(0x8FFFFFFC, false, false) == 0x0FFFFFFCu);
    CHECK(cpu.translate(0x80001234, true, false) == 0x1234u);
    CHECK_FALSE(cpu.translate(0xC0001234, true, false).has_value());  // no IBAT there
    CHECK_FALSE(cpu.translate(0x90000000, false, false).has_value());
    CHECK_FALSE(cpu.translate(0x7FFFFFFC, false, false).has_value());

    // Only IR or only DR: the other side stays physical.
    cpu.set_msr(kDr);
    CHECK(cpu.translate(0x90000000, true, false) == 0x90000000u);
    CHECK_FALSE(cpu.translate(0x90000000, false, false).has_value());

    // A 128 KiB block (BL = 0) at 0xE0000000, supervisor only.
    cpu.set_spr(spr::kDbat0u + 4, 0xE0000002);
    cpu.set_spr(spr::kDbat0u + 5, 0x00400002);
    CHECK(cpu.translate(0xE001FFFC, false, false) == 0x0041FFFCu);
    CHECK_FALSE(cpu.translate(0xE0020000, false, false).has_value());
    // A read-only block (PP = 01) for user and supervisor at 0xD0000000.
    cpu.set_spr(spr::kDbat0u + 6, 0xD0000003);
    cpu.set_spr(spr::kDbat0u + 7, 0x00500001);
    CHECK(cpu.translate(0xD0000010, false, false) == 0x00500010u);
    CHECK_FALSE(cpu.translate(0xD0000010, false, true).has_value());
    // In user mode (MSR[PR]) only the Vp blocks match.
    cpu.set_msr(kDr | kPr);
    CHECK_FALSE(cpu.translate(0xE0000000, false, false).has_value());
    CHECK(cpu.translate(0xD0000010, false, false) == 0x00500010u);

    // A program at virtual 0x80003000 runs through the IBAT and accesses memory through both
    // DBATs.
    f.bus.write32(0x10, 0xDEADBEEF);
    cpu.set_msr(kIr | kDr);
    cpu.set_gpr(4, 0xC0000000);
    load_at(f, 0x3000, 0x80003000, {lwz(3, 0x10, 4), stw(3, 0x20, 4), lis(5, 0x8000), lwz(6, 0x20, 5)});
    f.run(4);
    CHECK(cpu.gpr(3) == 0xDEADBEEF);
    CHECK(f.bus.read32(0x20) == 0xDEADBEEF);
    CHECK(cpu.gpr(6) == 0xDEADBEEF);
    CHECK(cpu.pc() == 0x80003010);
}

TEST_CASE("an address no BAT covers raises DSI with DAR and DSISR") {
    GekkoFixture f;
    auto& cpu = f.cpu;
    boot_bats(cpu);
    const std::uint32_t msr = kIr | kDr | kMe;

    // A load with update: nothing is written, not even rA.
    cpu.set_msr(msr);
    cpu.set_gpr(3, 7);
    load_at(f, 0x3000, 0x80003000, {lis(4, 0x9000), lwzu(3, 4, 4), stw(3, 0, 4)});
    f.run(2);
    CHECK(cpu.pc() == 0x300);
    CHECK(cpu.spr(spr::kSrr0) == 0x80003004);
    CHECK(cpu.spr(spr::kSrr1) == msr);
    CHECK(cpu.spr(spr::kDar) == 0x90000004);
    CHECK(cpu.spr(spr::kDsisr) == 0x40000000);  // no translation found
    CHECK(cpu.msr() == kMe);
    CHECK(cpu.gpr(3) == 7);
    CHECK(cpu.gpr(4) == 0x90000000);

    // A store also sets DSISR's store bit.
    cpu.set_msr(msr);
    cpu.set_pc(0x80003008);
    f.run(1);
    CHECK(cpu.pc() == 0x300);
    CHECK(cpu.spr(spr::kSrr0) == 0x80003008);
    CHECK(cpu.spr(spr::kDar) == 0x90000000);
    CHECK(cpu.spr(spr::kDsisr) == 0x42000000);

    // A store to a read-only block is a protection violation.
    cpu.set_spr(spr::kDbat0u + 6, 0xD0000003);
    cpu.set_spr(spr::kDbat0u + 7, 0x00500001);
    cpu.set_msr(msr);
    cpu.set_gpr(4, 0xD0000000);
    cpu.set_pc(0x80003008);
    f.run(1);
    CHECK(cpu.pc() == 0x300);
    CHECK(cpu.spr(spr::kDar) == 0xD0000000);
    CHECK(cpu.spr(spr::kDsisr) == 0x0A000000);
    CHECK(f.bus.read32(0x00500000) == 0);
}

TEST_CASE("a fetch no IBAT covers raises ISI") {
    GekkoFixture f;
    auto& cpu = f.cpu;
    boot_bats(cpu);
    cpu.set_msr(kIr | kDr | kMe | kIp);
    load_at(f, 0x3000, 0x80003000, {ba(0x10000)});  // lands at 0x10000: no IBAT
    f.run(2);
    CHECK(cpu.pc() == 0xFFF00400);  // MSR[IP] selects the high vectors
    CHECK(cpu.spr(spr::kSrr0) == 0x00010000);
    CHECK(cpu.spr(spr::kSrr1) == (0x40000000 | kIr | kDr | kMe | kIp));
    CHECK(cpu.msr() == (kMe | kIp));
}

TEST_CASE("external interrupt waits for MSR[EE]") {
    GekkoFixture f;
    auto& cpu = f.cpu;
    cpu.set_msr(0);
    cpu.set_external_interrupt(true);
    f.load({li(3, 0x3100), mtspr(spr::kSrr0, 3), li(4, 0), ori(4, 4, 0x8000), mtspr(spr::kSrr1, 4), rfi()});
    f.load({li(5, 1)}, 0x3100);
    f.load({li(6, 2)}, 0x500);
    cpu.set_pc(GekkoFixture::kProgram);

    f.run(6);  // with EE = 0 the level is ignored
    CHECK(cpu.pc() == 0x3100);
    CHECK(cpu.msr() == kEe);

    f.run(1);  // taken before li r5 runs
    CHECK(cpu.pc() == 0x500);
    CHECK(cpu.spr(spr::kSrr0) == 0x3100);
    CHECK(cpu.spr(spr::kSrr1) == kEe);
    CHECK((cpu.msr() & kEe) == 0);
    CHECK(cpu.gpr(5) == 0);

    // The handler runs with EE off although the line is still high.
    f.run(1);
    CHECK(cpu.gpr(6) == 2);
    CHECK(cpu.pc() == 0x504);
}

TEST_CASE("decrementer raises its exception on the 0 to negative transition") {
    GekkoFixture f;
    auto& cpu = f.cpu;
    cpu.set_msr(kEe);
    cpu.set_spr(spr::kDec, 2);
    f.load({ori(0, 0, 0), ori(0, 0, 0), ori(0, 0, 0), ori(0, 0, 0)});

    cpu.advance_timebase(2);  // DEC reaches 0: no exception yet
    CHECK(cpu.spr(spr::kDec) == 0);
    f.run(1);
    CHECK(cpu.pc() == 0x3004);

    cpu.advance_timebase(1);  // 0 -> -1
    CHECK(cpu.spr(spr::kDec) == 0xFFFFFFFF);
    f.run(1);
    CHECK(cpu.pc() == 0x900);
    CHECK(cpu.spr(spr::kSrr0) == 0x3004);
    CHECK(cpu.spr(spr::kSrr1) == kEe);
    CHECK(cpu.spr(spr::kTbl) == 3);

    // Taken once: counting further below zero raises nothing new.
    cpu.set_pc(0x3008);
    cpu.set_msr(kEe);
    cpu.advance_timebase(5);
    f.run(1);
    CHECK(cpu.pc() == 0x300C);

    // With EE off the request waits for EE.
    cpu.set_msr(0);
    cpu.set_spr(spr::kDec, 1);
    cpu.set_pc(0x3000);
    cpu.advance_timebase(3);
    f.run(1);
    CHECK(cpu.pc() == 0x3004);
    cpu.set_msr(kEe);
    f.run(1);
    CHECK(cpu.pc() == 0x900);
    CHECK(cpu.spr(spr::kSrr0) == 0x3004);

    // A large step that crosses zero also counts.
    cpu.set_msr(kEe);
    cpu.set_spr(spr::kDec, 0x7FFFFFFF);
    cpu.set_pc(0x3000);
    cpu.advance_timebase(0x80000000ULL);
    f.run(1);
    CHECK(cpu.pc() == 0x900);

    // mtspr DEC that sets bit 0 from 0 to 1 requests the exception too.
    cpu.set_msr(0);
    cpu.set_spr(spr::kDec, 5);
    cpu.set_gpr(3, 0x80000000);
    f.load({mtspr(spr::kDec, 3), ori(0, 0, 0)});
    f.run(1);
    cpu.set_msr(kEe);
    f.run(1);
    CHECK(cpu.pc() == 0x900);
    CHECK(cpu.spr(spr::kSrr0) == 0x3004);

    // The time base carries from TBL into TBU.
    cpu.set_spr(spr::kTblWrite, 0xFFFFFFFF);
    cpu.set_spr(spr::kTbuWrite, 0x00000010);
    cpu.advance_timebase(2);
    CHECK(cpu.spr(spr::kTbl) == 1);
    CHECK(cpu.spr(spr::kTbu) == 0x11);
}

TEST_CASE("sc, rfi, mtmsr, mfspr and mtspr of HID, SPRG, BAT, DEC, TB") {
    GekkoFixture f;
    auto& cpu = f.cpu;

    // sc: SRR0 = the next instruction; rfi returns there and restores MSR.
    cpu.set_msr(kEe | kMe);
    f.load({sc()});
    f.load({rfi()}, 0xC00);
    cpu.set_pc(GekkoFixture::kProgram);
    f.run(1);
    CHECK(cpu.pc() == 0xC00);
    CHECK(cpu.spr(spr::kSrr0) == 0x3004);
    CHECK(cpu.spr(spr::kSrr1) == (kEe | kMe));
    CHECK(cpu.msr() == kMe);
    f.run(1);
    CHECK(cpu.pc() == 0x3004);
    CHECK(cpu.msr() == (kEe | kMe));

    // With MSR[IP] the vector is at 0xFFF00C00.
    cpu.set_msr(kIp | kMe);
    f.exec(sc());
    CHECK(cpu.pc() == 0xFFF00C00);
    CHECK(cpu.msr() == (kIp | kMe));

    // mtmsr / mfmsr.
    cpu.set_msr(0);
    cpu.set_gpr(3, kMe | kIp | Gekko::kMsrFp);
    f.load({mtmsr(3), mfmsr(4)});
    f.run(2);
    CHECK(cpu.msr() == (kMe | kIp | Gekko::kMsrFp));
    CHECK(cpu.gpr(4) == cpu.msr());
    cpu.set_msr(0);

    // Every stored SPR reads back what mtspr wrote.
    const int stored[] = {spr::kLr,    spr::kCtr,   spr::kDsisr, spr::kDar,  spr::kSrr0, spr::kSrr1,
                          spr::kSprg0, spr::kSprg0 + 1, spr::kSprg0 + 2, spr::kSprg0 + 3, spr::kHid0, spr::kHid1,
                          spr::kHid2,  spr::kHid4,  spr::kWpar,  spr::kDmaU, spr::kDmaL, spr::kL2cr};
    std::uint32_t value = 0x10203040;
    for (const int n : stored) {
        CAPTURE(n);
        cpu.set_gpr(3, value);
        f.load({mtspr(n, 3), mfspr(4, n)});
        f.run(2);
        CHECK(cpu.gpr(4) == value);
        CHECK(cpu.spr(n) == value);
        value += 0x01010101;
    }
    for (int n = 0; n < 8; ++n) {
        CAPTURE(n);
        cpu.set_gpr(3, 0x1000 + static_cast<std::uint32_t>(n));
        f.load({mtspr(spr::kIbat0u + n, 3), mfspr(4, spr::kIbat0u + n), mtspr(spr::kDbat0u + n, 3),
                mfspr(5, spr::kDbat0u + n), mtspr(spr::kGqr0 + n, 3), mfspr(6, spr::kGqr0 + n)});
        f.run(6);
        CHECK(cpu.gpr(4) == 0x1000 + static_cast<std::uint32_t>(n));
        CHECK(cpu.gpr(5) == 0x1000 + static_cast<std::uint32_t>(n));
        CHECK(cpu.gpr(6) == 0x1000 + static_cast<std::uint32_t>(n));
    }

    // XER keeps SO, OV, CA and the byte count.
    cpu.set_gpr(3, 0xFFFFFFFF);
    f.load({mtspr(spr::kXer, 3), mfspr(4, spr::kXer)});
    f.run(2);
    CHECK(cpu.gpr(4) == 0xE000007F);

    // PVR is read-only and identifies the Gekko.
    f.exec(mfspr(4, spr::kPvr));
    CHECK(cpu.gpr(4) == 0x00083214);

    // DEC and the time base (written through 284/285, read with mftb).
    cpu.set_gpr(3, 0x89ABCDEF);
    cpu.set_gpr(4, 0x01234567);
    cpu.set_gpr(5, 1000);
    f.load({mtspr(spr::kTblWrite, 3), mtspr(spr::kTbuWrite, 4), mftb(6, spr::kTbl), mftb(7, spr::kTbu),
            mtspr(spr::kDec, 5), mfspr(8, spr::kDec), mfspr(9, spr::kTbl)});
    f.run(7);
    CHECK(cpu.gpr(6) == 0x89ABCDEF);
    CHECK(cpu.gpr(7) == 0x01234567);
    CHECK(cpu.gpr(8) == 1000);
    CHECK(cpu.gpr(9) == 0x89ABCDEF);

    // Segment registers are stored (no page tables: BAT-only translation).
    cpu.set_gpr(3, 0x20000005);
    cpu.set_gpr(4, 0xE0000000);
    f.load({mtsr(3, 3), mfsr(5, 3), mtsrin(3, 4), mfsrin(6, 4)});
    f.run(4);
    CHECK(cpu.gpr(5) == 0x20000005);
    CHECK(cpu.gpr(6) == 0x20000005);

    // An SPR the Gekko core does not model stops with the pc and the word.
    const std::uint32_t word = mfspr(4, 1013);  // DABR
    f.load({word});
    try {
        f.run(1);
        FAIL("expected GekkoError");
    } catch (const GekkoError& e) {
        CHECK(e.pc == GekkoFixture::kProgram);
        CHECK(e.word_or_address == word);
    }
    CHECK_THROWS_AS(cpu.set_spr(spr::kPvr, 0), GekkoError);
}

TEST_CASE("traps and privileged instructions raise program exceptions") {
    GekkoFixture f;
    auto& cpu = f.cpu;

    cpu.set_gpr(3, 1);
    cpu.set_gpr(4, 2);
    cpu.set_msr(kMe);
    f.load({twi(4, 3, 0), tw(31, 3, 4)});  // no trap (1 != 0), then an unconditional trap
    f.run(2);
    CHECK(cpu.pc() == 0x700);
    CHECK(cpu.spr(spr::kSrr0) == 0x3004);
    CHECK(cpu.spr(spr::kSrr1) == (0x00020000 | kMe));  // SRR1[14]: trap

    f.load({tw(16, 3, 4), tw(2, 4, 3), tw(8, 4, 3)});  // lt: yes
    f.run(1);
    CHECK(cpu.pc() == 0x700);
    f.load({tw(2, 4, 3), tw(1, 3, 4), tw(8, 4, 3)});  // ltu: no, gtu: no, gt: yes
    f.run(3);
    CHECK(cpu.pc() == 0x700);
    CHECK(cpu.spr(spr::kSrr0) == 0x3008);

    // Supervisor-only instructions in user mode.
    cpu.set_msr(kPr | kMe);
    f.exec(mfmsr(3));
    CHECK(cpu.pc() == 0x700);
    CHECK(cpu.spr(spr::kSrr1) == (0x00040000 | kPr | kMe));  // SRR1[13]: privileged
    cpu.set_msr(kPr);
    f.exec(mtspr(spr::kSprg0, 3));
    CHECK(cpu.pc() == 0x700);
    // LR, CTR and XER are user-level.
    cpu.set_msr(kPr);
    f.exec(mtspr(spr::kLr, 3));
    CHECK(cpu.pc() == 0x3004);
}

TEST_CASE("lmw with an unaligned address raises alignment") {
    GekkoFixture f;
    auto& cpu = f.cpu;
    cpu.set_msr(kMe);
    cpu.set_gpr(4, 0x1002);
    cpu.set_gpr(29, 0x77);
    f.exec(lmw(29, 0, 4));
    CHECK(cpu.pc() == 0x600);
    CHECK(cpu.spr(spr::kSrr0) == 0x3000);
    CHECK(cpu.spr(spr::kDar) == 0x1002);
    // DSISR: D-form, opcode bits 1-4 (0111) in 18-21, rD in 22-26, rA in 27-31.
    CHECK(cpu.spr(spr::kDsisr) == ((7u << 10) | (29u << 5) | 4u));
    CHECK(cpu.gpr(29) == 0x77);
}

TEST_CASE("dcbz_l needs the locked cache enabled in HID2") {
    GekkoFixture f;
    auto& cpu = f.cpu;
    f.bus.write32(0x1000, 0xFFFFFFFF);
    cpu.set_gpr(4, 0x1000);
    f.exec(dcbz_l(0, 4));
    CHECK(cpu.pc() == 0x700);
    CHECK((cpu.spr(spr::kSrr1) & 0x00080000) != 0);  // SRR1[12]: illegal instruction
    CHECK(f.bus.read32(0x1000) == 0xFFFFFFFF);

    // With HID2[LCE] but the data cache off (HID0[DCE] = 0): alignment, as dcbz.
    cpu.set_msr(0);
    cpu.set_spr(spr::kHid2, 0x10000000);  // HID2[LCE]
    f.exec(dcbz_l(0, 4));
    CHECK(cpu.pc() == 0x600);
    CHECK(cpu.spr(spr::kDar) == 0x1000);
    CHECK(f.bus.read32(0x1000) == 0xFFFFFFFF);

    cpu.set_msr(0);
    cpu.set_spr(spr::kHid0, Gekko::kHid0Dce);
    f.exec(dcbz_l(0, 4));
    CHECK(cpu.pc() == 0x3004);
    CHECK(f.bus.read32(0x1000) == 0);

    // LCE is checked before DCE: without LCE it is illegal even with the cache off.
    cpu.set_spr(spr::kHid0, 0);
    cpu.set_spr(spr::kHid2, 0);
    f.exec(dcbz_l(0, 4));
    CHECK(cpu.pc() == 0x700);
}

TEST_CASE("dcbz raises alignment while the data cache is off") {
    GekkoFixture f;
    auto& cpu = f.cpu;
    f.bus.write32(0x1020, 0xFFFFFFFF);
    cpu.set_gpr(4, 0x1000);
    cpu.set_gpr(5, 0x24);
    // HID0 = 0 after reset: DCE off (the IPL turns it on; so will the HLE boot).
    f.exec(dcbz(4, 5));
    CHECK(cpu.pc() == 0x600);
    CHECK(cpu.spr(spr::kSrr0) == 0x3000);
    CHECK(cpu.spr(spr::kDar) == 0x1024);
    // DSISR, X-form: xo 1014 bits 29-30 (10) in 15-16, bit 25 (1) in 17, bits 21-24 (1111) in
    // 18-21; rA in 27-31.
    CHECK(cpu.spr(spr::kDsisr) == ((2u << 15) | (1u << 14) | (15u << 10) | 4u));
    CHECK(f.bus.read32(0x1020) == 0xFFFFFFFF);

    cpu.set_msr(0);
    cpu.set_spr(spr::kHid0, Gekko::kHid0Dce | Gekko::kHid0Ice);
    f.exec(dcbz(4, 5));
    CHECK(cpu.pc() == 0x3004);
    CHECK(f.bus.read32(0x1020) == 0);
}

TEST_CASE("bcctr that decrements CTR (an invalid form) stops emulation") {
    GekkoFixture f;
    auto& cpu = f.cpu;
    cpu.set_ctr(0x5000);
    // BO = 16 (decrement CTR, branch if != 0, ignore the condition): undefined for bcctr.
    const std::uint32_t word = bcctr(16, 0);
    f.load({word});
    try {
        f.run(1);
        FAIL("expected GekkoError");
    } catch (const GekkoError& e) {
        CHECK(e.pc == GekkoFixture::kProgram);
        CHECK(e.word_or_address == word);
    }
    CHECK(cpu.pc() == GekkoFixture::kProgram);
    CHECK(cpu.ctr() == 0x5000);
}

TEST_CASE("bus() reaches guest memory by physical address without touching the CPU") {
    GekkoFixture f;
    auto& cpu = f.cpu;
    cpu.set_msr(kDr);  // translation on, no BATs: a CPU access would DSI
    cpu.set_pc(0x4000);
    cpu.bus().write32(0x2000, 0x12345678);
    CHECK(cpu.bus().read32(0x2000) == 0x12345678);
    CHECK(f.bus.read32(0x2000) == 0x12345678);
    CHECK(cpu.pc() == 0x4000);
    CHECK(cpu.msr() == kDr);
    CHECK(!cpu.translate(0x2000, false, false).has_value());
}

TEST_CASE("an unimplemented opcode throws GekkoError with the pc and the word") {
    GekkoFixture f;
    auto& cpu = f.cpu;
    for (const std::uint32_t word : {0x00000000u, 0x04000000u, 0x7C000002u, 0x4C000002u}) {
        CAPTURE(word);
        f.load({word});
        try {
            f.run(1);
            FAIL("expected GekkoError");
        } catch (const GekkoError& e) {
            CHECK(e.pc == GekkoFixture::kProgram);
            CHECK(e.word_or_address == word);
        }
        CHECK(cpu.pc() == GekkoFixture::kProgram);  // nothing ran
    }
}

TEST_CASE("reset leaves the CPU at the system reset vector") {
    GekkoFixture f;
    auto& cpu = f.cpu;
    cpu.set_gpr(3, 5);
    f.bus.write32(0xFFF00100, ori(0, 0, 0));  // the flat bus wraps the high vectors
    cpu.reset();
    CHECK(cpu.pc() == 0xFFF00100);
    CHECK(cpu.msr() == kIp);
    CHECK(cpu.gpr(3) == 0);
    CHECK(cpu.spr(spr::kPvr) == 0x00083214);
    CHECK(f.run(1) == 1);
}
