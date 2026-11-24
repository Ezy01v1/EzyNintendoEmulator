#include <doctest/doctest.h>

#include <algorithm>
#include <cstdint>
#include <vector>

#include "disc_builder.hpp"
#include "gc/di.hpp"
#include "gc/disc.hpp"
#include "gc/gamecube.hpp"
#include "gc/gekko/gekko.hpp"
#include "gc/memory.hpp"
#include "gc/pi.hpp"
#include "gc/scheduler.hpp"
#include "run_events.hpp"

using namespace ezy::gc;

namespace {
// DI registers (Dolphin's DVDInterface.cpp): status, cover, the three command words, DMA
// address, DMA length, control, immediate data, configuration.
constexpr std::uint32_t kDisr = 0x00, kDicvr = 0x04, kCmd0 = 0x08, kCmd1 = 0x0C, kCmd2 = 0x10, kDimar = 0x14,
                        kDilength = 0x18, kDicr = 0x1C, kDiimmbuf = 0x20, kDicfg = 0x24;
constexpr std::uint32_t kTcint = 0x10, kTcintMask = 0x08;

struct DiRig {
    DiscBuilder builder;
    TempImage image;
    std::unique_ptr<Disc> disc;
    Scheduler sched;
    Pi pi;
    std::vector<std::uint8_t> ram = std::vector<std::uint8_t>(Memory::kMem1Size, 0);
    std::unique_ptr<Di> di;

    explicit DiRig(const char* name) : image(builder.write(name)), disc(Disc::open(image)) {
        REQUIRE(disc != nullptr);
        di = std::make_unique<Di>(sched, pi, ram, *disc);
    }
    void run_to(std::uint64_t cycle) {
        run_events_to(sched, cycle, [&](const Scheduler::Due& due) {
            if (due.type == EventType::DiTransfer) di->on_event();
        });
    }
    // Starts a command: the three command words, then DICR.
    void command(std::uint32_t c0, std::uint32_t c1, std::uint32_t c2, std::uint32_t dicr) {
        di->write32(kCmd0, c0);
        di->write32(kCmd1, c1);
        di->write32(kCmd2, c2);
        di->write32(kDicr, dicr);
    }
    [[nodiscard]] std::uint32_t ram32(std::uint32_t at) const {
        return (std::uint32_t{ram[at]} << 24) | (std::uint32_t{ram[at + 1]} << 16) | (std::uint32_t{ram[at + 2]} << 8) |
               ram[at + 3];
    }
};
}  // namespace

TEST_CASE("DI Read DMAs the disc bytes to MEM1 and raises TCINT after its delay") {
    DiRig rig("di_read");
    auto& di = *rig.di;
    rig.pi.write32(0x04, 1u << Pi::kDi);  // PI: DI unmasked

    // At reset: the cover is closed (a disc is in), the configuration reads 1 (Dolphin).
    CHECK(di.read32(kDicvr) == 0);
    CHECK(di.read32(kDicfg) == 1);

    // Read 0x1000 bytes from the DOL's offset to physical 0x00100000 (the SDK's DVDLowRead:
    // command 0xA8000000, offset >> 2, length; DIMAR, DILENGTH; DICR = TSTART | DMA).
    constexpr std::uint32_t kBytes = 0x1000, kTarget = 0x00100000;
    di.write32(kDisr, kTcintMask);
    di.write32(kDimar, kTarget);
    di.write32(kDilength, kBytes);
    rig.command(0xA8000000, DiscBuilder::kDolOffset >> 2, kBytes, 3);
    CHECK((di.read32(kDicr) & 1) == 1);  // TSTART: busy
    CHECK(di.reads() == 1);

    // 50 us + bytes / 3.1 MB/s, in 486 MHz CPU cycles.
    const std::uint64_t done = 24300 + std::uint64_t{kBytes} * 4860 / 31;
    CHECK(Di::read_cycles(kBytes) == done);
    rig.run_to(done - 1);
    CHECK((di.read32(kDisr) & kTcint) == 0);
    CHECK((di.read32(kDicr) & 1) == 1);
    CHECK_FALSE(rig.pi.line());
    CHECK(rig.ram32(kTarget) == 0);  // the data lands when the transfer finishes

    rig.run_to(done);
    CHECK((di.read32(kDisr) & kTcint) != 0);
    CHECK((di.read32(kDicr) & 1) == 0);
    CHECK(rig.pi.line());
    CHECK(std::equal(rig.ram.begin() + kTarget, rig.ram.begin() + kTarget + kBytes,
                     rig.builder.image.begin() + DiscBuilder::kDolOffset));
    CHECK(rig.ram[kTarget + kBytes] == 0);         // not one byte past
    CHECK(di.read32(kDimar) == kTarget + kBytes);  // the address advances, the length counts down
    CHECK(di.read32(kDilength) == 0);

    // The CPU acknowledges TCINT by writing it back.
    di.write32(kDisr, kTcint | kTcintMask);
    CHECK((di.read32(kDisr) & kTcint) == 0);
    CHECK_FALSE(rig.pi.line());

    // The disc ID read (0xA8000040): the first 0x20 bytes of the disc.
    di.write32(kDimar, 0x00200000);
    di.write32(kDilength, 0x20);
    rig.command(0xA8000040, 0, 0x20, 3);
    rig.run_to(rig.sched.now() + Di::read_cycles(0x20));
    CHECK(std::equal(rig.ram.begin() + 0x00200000, rig.ram.begin() + 0x00200020, rig.builder.image.begin()));
    CHECK(di.reads() == 2);

    // Inquiry DMAs the drive information (Dolphin's: revision 2, date 2006-05-26, version 0x41).
    di.write32(kDisr, kTcint | kTcintMask);
    di.write32(kDimar, 0x00300000);
    di.write32(kDilength, 0x20);
    rig.command(0x12000000, 0, 0x20, 3);
    rig.run_to(rig.sched.now() + Di::read_cycles(0x20));
    CHECK(rig.ram32(0x00300000) == 0x00000002);
    CHECK(rig.ram32(0x00300004) == 0x20060526);
    CHECK(rig.ram32(0x00300008) == 0x41000000);
    CHECK((di.read32(kDisr) & kTcint) != 0);

    // Immediate commands answer in DIIMMBUF: Request Error = no error, drive ready; the
    // audio-streaming status is "not playing". Seek and the streaming controls just finish,
    // leaving the DMA registers alone.
    const std::uint32_t dimar = di.read32(kDimar);
    CHECK(dimar == 0x00300020);
    for (const std::uint32_t c0 : {0xE0000000u, 0xE2000000u, 0xAB000000u, 0xE1010000u, 0xE4000000u}) {
        di.write32(kDisr, kTcint | kTcintMask);
        di.write32(kDiimmbuf, 0xFFFFFFFF);
        rig.command(c0, 0, 0, 1);
        rig.run_to(rig.sched.now() + Di::kCommandCycles);
        CHECK((di.read32(kDisr) & kTcint) != 0);
        CHECK((di.read32(kDicr) & 1) == 0);
        CHECK(di.read32(kDimar) == dimar);
        if (c0 == 0xE0000000u || c0 == 0xE2000000u) CHECK(di.read32(kDiimmbuf) == 0);
    }

    // An unmodeled command stops the machine; so does a 16-bit access and the read-only DICFG.
    CHECK_THROWS_AS(rig.command(0xE3000000, 0, 0, 1), BusError);  // stop motor
    CHECK_THROWS_AS(di.read16(kDisr), BusError);
    CHECK_THROWS_AS(di.write32(kDicfg, 0), BusError);
    CHECK_THROWS_AS(di.read32(0x28), BusError);
}

TEST_CASE("DI rejects a DMA outside MEM1 with an error") {
    DiRig rig("di_bounds");
    auto& di = *rig.di;
    // DIMAR keeps 0x03FFFFE0 (Dolphin's GameCube mask), so it can name addresses past MEM1.
    di.write32(kDimar, 0x017FFFE0);
    CHECK(di.read32(kDimar) == 0x017FFFE0);
    di.write32(kDilength, 0x40);  // ends 0x20 bytes past MEM1
    CHECK_THROWS_AS(rig.command(0xA8000000, 0, 0x40, 3), BusError);
    di.write32(kDimar, 0x03FFFFFF);
    CHECK(di.read32(kDimar) == 0x03FFFFE0);
    di.write32(kDilength, 0x20);
    CHECK_THROWS_AS(rig.command(0xA8000000, 0, 0x20, 3), BusError);
    CHECK_FALSE(rig.sched.pending(EventType::DiTransfer));  // nothing was started
    CHECK(std::all_of(rig.ram.begin(), rig.ram.end(), [](std::uint8_t b) { return b == 0; }));

    // A read past the end of the disc is an error too, as is a length that disagrees with
    // DILENGTH, and a read without DMA.
    di.write32(kDimar, 0);
    di.write32(kDilength, 0x20);
    CHECK_THROWS_AS(rig.command(0xA8000000, DiscBuilder::kImageSize >> 2, 0x20, 3), BusError);
    CHECK_THROWS_AS(rig.command(0xA8000000, 0, 0x40, 3), BusError);
    CHECK_THROWS_AS(rig.command(0xA8000000, 0, 0x20, 1), BusError);

    // The last 0x20 bytes of MEM1 are fine.
    di.write32(kDimar, Memory::kMem1Size - 0x20);
    rig.command(0xA8000000, 0, 0x20, 3);
    rig.run_to(Di::read_cycles(0x20));
    CHECK(std::equal(rig.ram.end() - 0x20, rig.ram.end(), rig.builder.image.begin()));

    // DIMAR rewritten while a read runs does not move where the data lands (it was checked
    // at TSTART), so it can never land past MEM1.
    di.write32(kDimar, 0x00500000);
    di.write32(kDilength, 0x20);
    rig.command(0xA8000000, 0, 0x20, 3);
    di.write32(kDimar, 0x03FFFFE0);
    rig.run_to(rig.sched.now() + Di::read_cycles(0x20));
    CHECK(std::equal(rig.ram.begin() + 0x00500000, rig.ram.begin() + 0x00500020, rig.builder.image.begin()));
    CHECK(di.read32(kDimar) == 0x00500020);
}

TEST_CASE("the machine maps the DI and counts its reads") {
    DiscBuilder builder;
    const auto image = builder.write("di_machine");
    auto gc = GameCube::create(image);
    REQUIRE(gc != nullptr);
    auto& mem = gc->memory();
    constexpr std::uint32_t kDi = 0x0C006000;
    mem.write32(kDi + kDisr, kTcintMask);
    mem.write32(kDi + kDimar, 0x00400000);
    mem.write32(kDi + kDilength, 0x20);
    mem.write32(kDi + kCmd0, 0xA8000000);
    mem.write32(kDi + kCmd1, DiscBuilder::kFileOffset >> 2);
    mem.write32(kDi + kCmd2, 0x20);
    mem.write32(kDi + kDicr, 3);
    mem.write32(0x0C003004, 1u << Pi::kDi);
    gc->run_field();  // a field is far longer than the read
    CHECK((mem.read32(0x0C003000) & (1u << Pi::kDi)) != 0);
    CHECK(gc->cpu().state().external_interrupt);
    const auto ram = mem.mem1();
    CHECK(std::equal(builder.file_contents.begin(), builder.file_contents.end(), ram.begin() + 0x00400000));
    CHECK(gc->stats().di_reads == 1);
}
