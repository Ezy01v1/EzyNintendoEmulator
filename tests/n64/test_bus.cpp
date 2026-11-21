#include <doctest/doctest.h>

#include <algorithm>
#include <cstdint>
#include <utility>
#include <vector>

#include "mips_asm.hpp"
#include "n64_test_helpers.hpp"
#include "n64/bus.hpp"
#include "n64/cartridge.hpp"
#include "n64/scheduler.hpp"

using namespace ezy::n64;

namespace {

// A 1 MB cartridge whose byte at offset i is (i * 3) & 0xFF (after the header).
Cartridge test_cart() {
    std::vector<std::uint8_t> rom(0x100000, 0);
    rom[0] = 0x80, rom[1] = 0x37, rom[2] = 0x12, rom[3] = 0x40;
    for (std::size_t i = 0x1000; i < rom.size(); ++i) rom[i] = static_cast<std::uint8_t>(i * 3);
    return *Cartridge::load(rom);
}

struct BusFixture {
    Scheduler scheduler;
    Cartridge cart = test_cart();
    Bus bus;

    explicit BusFixture(RdpMode mode = default_rdp_mode()) : bus(scheduler, cart, mode) {}

    // Runs every event due up to `cycles` from now.
    void run(std::uint64_t cycles) {
        const std::uint64_t end = scheduler.now() + cycles;
        while (true) {
            const auto next = scheduler.next_event_time();
            if (!next || *next > end) break;
            scheduler.advance_to(*next);
            while (const auto due = scheduler.pop_due()) bus.handle_event(due->type, due->when);
        }
        scheduler.advance_to(end);
    }
};

constexpr std::uint32_t kMiMode = 0x04300000, kMiVersion = 0x04300004, kMiIntr = 0x04300008, kMiMask = 0x0430000C;
constexpr std::uint32_t kPiDram = 0x04600000, kPiCart = 0x04600004, kPiWrLen = 0x0460000C, kPiStatus = 0x04600010;
constexpr std::uint32_t kDmem = 0x04000000, kImem = 0x04001000, kSpMemAddr = 0x04040000,
                        kSpDramAddr = 0x04040004, kSpReadLen = 0x04040008, kSpWriteLen = 0x0404000C,
                        kSpStatus = 0x04040010, kSpDmaFull = 0x04040014, kSpDmaBusy = 0x04040018,
                        kSpPc = 0x04080000;

// Records what the bus reports to its code watch. By default every page counts as holding
// compiled code.
struct CodeWatch {
    std::vector<std::uint8_t> pages = std::vector<std::uint8_t>(Bus::kRdramSize >> 12, 1);
    std::vector<std::pair<std::uint32_t, std::uint32_t>> ranges;
    const std::uint8_t* probe = nullptr;  // sampled at every report
    std::vector<std::uint8_t> probed;

    explicit CodeWatch(Bus& bus) { bus.set_code_watch(pages.data(), &record, this); }
    static void record(void* user, std::uint32_t begin, std::uint32_t end) {
        auto& self = *static_cast<CodeWatch*>(user);
        self.ranges.emplace_back(begin, end);
        if (self.probe != nullptr) self.probed.push_back(*self.probe);
    }
    // Every byte of [begin, end) was in some report.
    [[nodiscard]] bool covers(std::uint32_t begin, std::uint32_t end) const {
        for (std::uint32_t at = begin; at < end; ++at) {
            if (!std::any_of(ranges.begin(), ranges.end(), [at](const auto& r) { return r.first <= at && at < r.second; }))
                return false;
        }
        return true;
    }
    [[nodiscard]] bool touches(std::uint32_t begin, std::uint32_t end) const {
        return std::any_of(ranges.begin(), ranges.end(), [&](const auto& r) { return r.first < end && begin < r.second; });
    }
};

}  // namespace

TEST_CASE("RDRAM reads and writes are big-endian") {
    BusFixture f;
    f.bus.write32(0x100, 0x11223344);
    CHECK(f.bus.read8(0x100) == 0x11);
    CHECK(f.bus.read8(0x103) == 0x44);
    CHECK(f.bus.read16(0x102) == 0x3344);
    f.bus.write8(0x101, 0xAA);
    CHECK(f.bus.read32(0x100) == 0x11AA3344);
}

TEST_CASE("64-bit access splits into two words") {
    BusFixture f;
    f.bus.write64(0x200, 0x0102030405060708ULL);
    CHECK(f.bus.read32(0x200) == 0x01020304);
    CHECK(f.bus.read32(0x204) == 0x05060708);
    CHECK(f.bus.read64(0x200) == 0x0102030405060708ULL);
}

TEST_CASE("MI_VERSION reads 0x02020102") {
    BusFixture f;
    CHECK(f.bus.read32(kMiVersion) == 0x02020102);
}

TEST_CASE("MI line follows interrupt and mask") {
    BusFixture f;
    f.bus.mi().raise(MiInterrupt::Pi);
    CHECK(f.bus.read32(kMiIntr) == 0x10);
    CHECK_FALSE(f.bus.interrupt_line());
    f.bus.write32(kMiMask, 1U << 9);  // set PI mask
    CHECK(f.bus.read32(kMiMask) == 0x10);
    CHECK(f.bus.interrupt_line());
    f.bus.write32(kMiMask, 1U << 8);  // clear PI mask
    CHECK_FALSE(f.bus.interrupt_line());
}

TEST_CASE("MI_MODE bit 11 clears the DP interrupt") {
    BusFixture f;
    f.bus.mi().raise(MiInterrupt::Dp);
    f.bus.write32(kMiMode, 1U << 11);
    CHECK(f.bus.read32(kMiIntr) == 0);
}

TEST_CASE("unmapped read returns 0") {
    BusFixture f;
    CHECK(f.bus.read32(0x00900000) == 0);  // beyond 8 MB of RDRAM
    CHECK(f.bus.read32(0x04900000) == 0);
}

TEST_CASE("SP DMEM is reachable and the RSP starts halted") {
    BusFixture f;
    f.bus.write32(kDmem + 0x10, 0xDEADBEEF);
    CHECK(f.bus.read32(kDmem + 0x10) == 0xDEADBEEF);
    CHECK((f.bus.read32(kSpStatus) & 1) == 1);
    f.bus.write32(kSpStatus, 3);  // simultaneous clear and set leave HALT alone
    CHECK((f.bus.read32(kSpStatus) & 1) == 1);
}

TEST_CASE("SP DMA aligns, strides and wraps within its bank with a queued reverse transfer") {
    BusFixture f;
    f.bus.write64(0x100, 0x1122334455667788ULL);
    f.bus.write64(0x108, 0xDEADBEEFDEADBEEFULL);
    f.bus.write64(0x110, 0x99AABBCCDDEEFF00ULL);
    f.bus.write64(kImem, 0x1234567887654321ULL);
    f.bus.write32(kSpMemAddr, 0xFFF);
    f.bus.write32(kSpDramAddr, 0x107);
    f.bus.write32(kSpReadLen, 0x00901001);  // two 8-byte rows, 8-byte skip after masking
    CHECK(f.bus.read32(kSpDmaBusy) == 1);
    CHECK(f.bus.read32(kSpMemAddr) == 0xFF8);
    CHECK(f.bus.read32(kSpDramAddr) == 0x100);
    CHECK(f.bus.read64(kDmem + 0xFF8) == 0);

    f.bus.write32(kSpMemAddr, 0xFF8);
    f.bus.write32(kSpDramAddr, 0x300);
    f.bus.write32(kSpWriteLen, 15);
    CHECK(f.bus.read32(kSpDmaFull) == 1);
    CHECK((f.bus.read32(kSpStatus) & 0xC) == 0xC);
    CHECK(f.bus.read32(kSpDramAddr) == 0x100);

    f.run(5);
    CHECK(f.bus.read64(kDmem + 0xFF8) == 0x1122334455667788ULL);
    CHECK(f.bus.read32(kSpDramAddr) == 0x110);
    CHECK(f.bus.read64(kDmem) == 0);
    f.run(5);
    CHECK(f.bus.read64(kDmem) == 0x99AABBCCDDEEFF00ULL);
    CHECK(f.bus.read64(kImem) == 0x1234567887654321ULL);
    CHECK(f.bus.read32(kSpDmaFull) == 0);
    CHECK(f.bus.read32(kSpDmaBusy) == 1);
    f.run(9);
    CHECK(f.bus.read32(kSpDmaBusy) == 0);
    CHECK(f.bus.read32(kSpDramAddr) == 0x310);
    CHECK(f.bus.read32(kSpMemAddr) == 8);
    CHECK(f.bus.read32(kSpReadLen) == 0xFF8);
    CHECK(f.bus.read32(kSpWriteLen) == 0xFF8);
    CHECK(f.bus.read64(0x300) == 0x1122334455667788ULL);
    CHECK(f.bus.read64(0x308) == 0x99AABBCCDDEEFF00ULL);
    CHECK(f.bus.read32(kMiIntr) == 0);
}

TEST_CASE("RSP COP0 can initiate an SP DMA and poll its completion") {
    using namespace mips;
    BusFixture f;
    f.bus.write64(0x100, 0x1122334455667788ULL);
    const std::uint32_t program[] = {
        mtc0(0, 0), addiu(1, 0, 0x100), mtc0(1, 1), mtc0(0, 2),
        mfc0(2, 6), bne(2, 0, -2), nop(),
        lw(3, 0, 0), sw(3, 0, 8), special(0, 0, 0, 0, 0x0D),
    };
    std::uint32_t address = kImem;
    for (const auto word : program) {
        f.bus.write32(address, word);
        address += 4;
    }
    f.bus.write32(kSpStatus, 1);
    f.run(100);
    CHECK(f.bus.read32(kSpStatus) == 3);
    CHECK(f.bus.read32(kSpPc) == sizeof(program));
    CHECK(f.bus.read32(kDmem + 8) == 0x11223344);
    CHECK(f.bus.read32(kMiIntr) == 0);
}

TEST_CASE("cartridge ROM reads through domain 1") {
    BusFixture f;
    CHECK(f.bus.read32(0x10000000) == 0x80371240);
    CHECK(f.bus.read8(0x10001001) == static_cast<std::uint8_t>(0x1001 * 3));
}

TEST_CASE("PI DMA copies cart to RDRAM and interrupts at the end") {
    BusFixture f;
    f.bus.write32(kPiDram, 0x1000);
    f.bus.write32(kPiCart, 0x10002000);
    f.bus.write32(kPiWrLen, 0x7F);  // 128 bytes
    for (std::uint32_t i = 0; i < 128; ++i) CHECK(f.bus.read8(0x1000 + i) == static_cast<std::uint8_t>((0x2000 + i) * 3));
    CHECK((f.bus.read32(kMiIntr) & 0x10) == 0);
    f.run(1'000'000);
    CHECK((f.bus.read32(kMiIntr) & 0x10) != 0);
    CHECK((f.bus.read32(kPiStatus) & 1) == 0);
    CHECK((f.bus.read32(kPiStatus) & 8) != 0);
    f.bus.write32(kPiStatus, 2);  // clear the interrupt
    CHECK((f.bus.read32(kMiIntr) & 0x10) == 0);
    CHECK(f.bus.read32(kPiDram) == 0x1080);
    CHECK(f.bus.read32(kPiCart) == 0x10002080);
}

TEST_CASE("PI stays busy until the scheduled end") {
    BusFixture f;
    f.bus.write32(0x04600014, 0x40);  // DOM1 LAT
    f.bus.write32(0x04600018, 0x12);  // DOM1 PWD
    f.bus.write32(0x0460001C, 0x07);  // DOM1 PGS
    f.bus.write32(0x04600020, 0x03);  // DOM1 RLS
    f.bus.write32(kPiDram, 0);
    f.bus.write32(kPiCart, 0x10001000);
    f.bus.write32(kPiWrLen, 0xFFF);  // 4 KB
    CHECK((f.bus.read32(kPiStatus) & 1) == 1);
    // 4 KB in 512-byte pages: 8 x (0x40 + 1) + 2048 halfwords x (0x12 + 1 + 0x03 + 1) RCP cycles
    const std::uint64_t rcp = 8 * 0x41 + 2048 * 0x17;
    f.run(rcp * 3 / 2 - 1);
    CHECK((f.bus.read32(kPiStatus) & 1) == 1);
    f.run(2);
    CHECK((f.bus.read32(kPiStatus) & 1) == 0);
}

// PeterLemon DMAAlignment-PI-ROM-FROM: a DMA to an RDRAM address 6, 4 or 2 bytes past an
// 8-byte boundary loses that many bytes of its first block.
TEST_CASE("PI DMA to a misaligned RDRAM address shortens the first block") {
    for (const std::uint32_t misalign : {2U, 4U, 6U}) {
        BusFixture f;
        for (std::uint32_t i = 0; i < 0x40; ++i) f.bus.write8(0x3000 + i, 0);
        f.bus.write32(kPiDram, 0x3000 + misalign);
        f.bus.write32(kPiCart, 0x10002000);
        f.bus.write32(kPiWrLen, 14);  // 15 bytes
        const std::uint32_t written = 15 - misalign;
        for (std::uint32_t i = 0; i < written; ++i) {
            CHECK(f.bus.read8(0x3000 + misalign + i) == static_cast<std::uint8_t>((0x2000 + i) * 3));
        }
        CHECK(f.bus.read8(0x3000 + misalign + written) == 0);
        CHECK(f.bus.read8(0x3000 + misalign - 1) == 0);
    }
}

TEST_CASE("PI length registers read back 0x7F after a DMA") {
    BusFixture f;
    f.bus.write32(kPiDram, 0x1000);
    f.bus.write32(kPiCart, 0x10002000);
    f.bus.write32(kPiWrLen, 0xFFF);
    CHECK(f.bus.read32(kPiWrLen) == 0x7F);
    CHECK(f.bus.read32(0x04600008) == 0x7F);
}

// PeterLemon DMAAlignment-PI-ROM-FROM: 8- and 16-bit CPU reads of the cartridge return the
// upper half of the word at (address + 2) & ~3.
TEST_CASE("8- and 16-bit cartridge reads see the upper half of the next word") {
    BusFixture f;
    // Words at 0x1000/0x1004/0x1008: (i * 3) bytes, so compute the expectations from them.
    const auto upper = [&](std::uint32_t a) { return static_cast<std::uint16_t>(f.bus.read32(0x10000000 + ((a + 2) & ~3U)) >> 16); };
    for (std::uint32_t a = 0x1000; a < 0x100C; a += 2) CHECK(f.bus.read16(0x10000000 + a) == upper(a));
    for (std::uint32_t a = 0x1000; a < 0x1006; ++a) {
        const std::uint16_t half = upper(a);
        CHECK(f.bus.read8(0x10000000 + a) == static_cast<std::uint8_t>((a & 1) ? half : half >> 8));
    }
    CHECK(f.bus.read16(0x10000000) == 0x8037);
    CHECK(f.bus.read16(0x10000002) == 0x0000);  // the upper half of the word at 4
}

TEST_CASE("ISViewer write of the length emits the buffer") {
    BusFixture f;
    CHECK(f.bus.read32(0x13FF0000) == 0x49533634);  // "IS64"
    f.bus.write32(0x13FF0020, 0x48690A00);           // "Hi\n"
    f.bus.write32(0x13FF0014, 3);
    CHECK(f.bus.take_isviewer_output() == "Hi\n");
    CHECK(f.bus.take_isviewer_output().empty());
}

TEST_CASE("uncached RDRAM access has a penalty, RCP registers too") {
    BusFixture f;
    CHECK(f.bus.uncached_cycles(0x1000) > 0);
    CHECK(f.bus.uncached_cycles(0x04300004) > 0);
    CHECK(f.bus.uncached_cycles(0x10000000) > f.bus.uncached_cycles(0x1000));
}

TEST_CASE("Bus reports PI, SI, SP DMA, audio HLE and RDP writes to the code watch") {
    BusFixture f;
    CodeWatch watch(f.bus);

    SUBCASE("CPU accesses through the bus") {
        f.bus.write32(0x500, 1);
        f.bus.write8(0x601, 1);
        f.bus.write64(0x700, 1);
        CHECK(watch.covers(0x500, 0x504));
        CHECK(watch.covers(0x601, 0x602));
        CHECK(watch.covers(0x700, 0x708));
        CHECK(f.bus.read32(0x500) == 1);
        CHECK_FALSE(watch.touches(0x504, 0x601));
    }
    SUBCASE("PI DMA, cartridge to RDRAM") {
        f.bus.write32(kPiDram, 0x1000);
        f.bus.write32(kPiCart, 0x10002000);
        f.bus.write32(kPiWrLen, 0x7F);
        CHECK(watch.covers(0x1000, 0x1080));
    }
    SUBCASE("SI DMA, PIF RAM to RDRAM, and not the reverse") {
        f.bus.write32(0x04800000, 0x2000);  // SI_DRAM_ADDR
        f.bus.write32(0x04800004, 0);       // SI_PIF_AD_RD64B
        CHECK(watch.covers(0x2000, 0x2040));
        watch.ranges.clear();
        f.run(100000);
        f.bus.write32(0x04800010, 0);  // SI_PIF_AD_WR64B: reads RDRAM
        CHECK(watch.ranges.empty());
    }
    SUBCASE("SP DMA, DMEM to RDRAM, and not the reverse") {
        f.bus.write32(kSpMemAddr, 0);
        f.bus.write32(kSpDramAddr, 0x3000);
        f.bus.write32(kSpWriteLen, 0x7F);
        f.run(1000);
        CHECK(watch.covers(0x3000, 0x3080));
        watch.ranges.clear();
        f.bus.write32(kSpDramAddr, 0x3000);
        f.bus.write32(kSpReadLen, 0x7F);
        f.run(1000);
        CHECK(watch.ranges.empty());
    }
    SUBCASE("SP DMA that wraps the 24-bit address reports the part at 0") {
        f.bus.write32(kSpMemAddr, 0);
        f.bus.write32(kSpDramAddr, 0xFFFFF8);
        f.bus.write32(kSpWriteLen, 15);
        f.run(1000);
        CHECK(watch.covers(0, 8));
        CHECK_FALSE(watch.touches(8, Bus::kRdramSize));
    }
    SUBCASE("audio HLE") {
        // The callback AudioHle::run gets (a real task needs the MK64 microcode's data).
        f.bus.audio_hle_access(0x4000, 16, true);
        f.bus.audio_hle_access(0x5000, 16, false);
        CHECK(watch.covers(0x4000, 0x4010));
        CHECK_FALSE(watch.touches(0x5000, 0x5010));
    }
}

TEST_CASE("Bus reports RDP draws to the code watch before they write") {
    using n64_test::kFillMode;
    using n64_test::rdp_rectangle;
    // A 320x240 RGBA5551 image at 1 MB, filled whole: the report must cover it and come
    // before the first pixel is written (in threaded mode, before the worker gets the draw).
    constexpr std::uint32_t kImage = 0x100000, kBytes = 320 * 240 * 2;
    const std::vector<std::uint64_t> list = {
        0x3F10013F00000000ULL | kImage,
        rdp_rectangle(0x2D, 0, 0, 320, 240),
        kFillMode,
        0x37000000F801F801ULL,
        rdp_rectangle(0x36, 319, 239, 0, 0),
    };
    for (const RdpMode mode : {RdpMode::Synchronous, RdpMode::Threaded}) {
        CAPTURE(static_cast<int>(mode));
        BusFixture f(mode);
        CodeWatch watch(f.bus);
        watch.probe = &f.bus.rdram()[kImage];
        f.bus.rdp().submit_words(list, f.bus.mi(), f.bus.rdram());
        CHECK(watch.covers(kImage, kImage + kBytes));
        REQUIRE(watch.probed.size() == watch.ranges.size());
        for (const std::uint8_t before : watch.probed) CHECK(before == 0);
        f.bus.drain_rdp();
        CHECK(f.bus.read32(kImage) == 0xF801F801);
        CHECK(f.bus.read32(kImage + kBytes - 4) == 0xF801F801);
    }
}

TEST_CASE("Code watch reports only marked pages, clamped to RDRAM") {
    BusFixture f;
    CodeWatch watch(f.bus);
    std::fill(watch.pages.begin(), watch.pages.end(), std::uint8_t{0});
    watch.pages[2] = watch.pages[3] = watch.pages[5] = 1;
    watch.pages.back() = 1;
    f.bus.note_ram_write(0x1800, 0x5000);  // pages 1..6
    REQUIRE(watch.ranges.size() == 2);
    CHECK(watch.ranges[0] == std::pair<std::uint32_t, std::uint32_t>{0x2000, 0x4000});
    CHECK(watch.ranges[1] == std::pair<std::uint32_t, std::uint32_t>{0x5000, 0x6000});
    watch.ranges.clear();
    f.bus.note_ram_write(0x2010, 4);
    f.bus.note_ram_write(0x4010, 4);  // unmarked
    REQUIRE(watch.ranges.size() == 1);
    CHECK(watch.ranges[0] == std::pair<std::uint32_t, std::uint32_t>{0x2010, 0x2014});
    watch.ranges.clear();
    f.bus.note_ram_write(Bus::kRdramSize - 4, 64);  // runs off the end of RDRAM
    f.bus.note_ram_write(Bus::kRdramSize, 64);      // past it
    REQUIRE(watch.ranges.size() == 1);
    CHECK(watch.ranges[0] == std::pair<std::uint32_t, std::uint32_t>{Bus::kRdramSize - 4, Bus::kRdramSize});
    watch.ranges.clear();
    f.bus.set_code_watch(nullptr, nullptr, nullptr);
    f.bus.write32(0x2010, 1);
    CHECK(watch.ranges.empty());
}
