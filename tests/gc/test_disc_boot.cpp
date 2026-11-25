#include <doctest/doctest.h>

#include <cstdint>
#include <cstdio>
#include <string>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <vector>

#include "disc_builder.hpp"
#include "gc/disc.hpp"
#include "gc/dol.hpp"
#include "gc/gekko/gekko.hpp"
#include "gc/hle_boot.hpp"
#include "gc/memory.hpp"

using ezy::gc::BusError;
using ezy::gc::Disc;
using ezy::gc::Dol;
using ezy::gc::Gekko;
using ezy::gc::Memory;

namespace {

constexpr std::uint32_t phys(std::uint32_t cached) { return cached - 0x80000000u; }

std::string hex(std::uint32_t value) {
    char text[11];
    std::snprintf(text, sizeof(text), "0x%08X", value);
    return text;
}

std::vector<std::uint8_t> read_all(Disc& disc, std::uint64_t offset, std::uint32_t size) {
    std::vector<std::uint8_t> bytes(size);
    REQUIRE(disc.read(offset, bytes));
    return bytes;
}

}  // namespace

TEST_CASE("Disc reads the header, game id, DOL and FST offsets") {
    DiscBuilder builder;
    const auto image = builder.write("header");
    const auto disc = Disc::open(image);
    REQUIRE(disc != nullptr);
    CHECK(disc->game_id() == "GTST01");
    CHECK(disc->dol_offset() == DiscBuilder::kDolOffset);
    CHECK(disc->fst_offset() == DiscBuilder::kFstOffset);
    CHECK(disc->fst_size() == DiscBuilder::fst_size());
    CHECK(disc->fst_max_size() == DiscBuilder::fst_size());
    CHECK(disc->size() == DiscBuilder::kImageSize);

    // Streaming reads anywhere inside the file.
    const auto file = read_all(*disc, DiscBuilder::kFileOffset, static_cast<std::uint32_t>(builder.file_contents.size()));
    CHECK(std::string(file.begin(), file.end()) == builder.file_contents);
    // The last byte can be read; one past it cannot, and the reader survives that.
    std::vector<std::uint8_t> one(1);
    CHECK(disc->read(DiscBuilder::kImageSize - 1, one));
    CHECK_FALSE(disc->read(DiscBuilder::kImageSize, one));
    std::vector<std::uint8_t> two(2);
    CHECK_FALSE(disc->read(DiscBuilder::kImageSize - 1, two));
    CHECK_FALSE(disc->read(~std::uint64_t{0}, one));
    CHECK(disc->read(0, two));
    CHECK(two[0] == 'G');
}

TEST_CASE("Disc rejects a file without the GameCube magic") {
    DiscBuilder builder;
    builder.put32(0x01C, 0);
    CHECK(Disc::open(builder.write("nomagic")) == nullptr);

    // Offsets that fall outside the file.
    DiscBuilder bad_dol;
    bad_dol.put32(0x420, DiscBuilder::kImageSize);
    CHECK(Disc::open(bad_dol.write("baddol")) == nullptr);
    DiscBuilder bad_fst;
    bad_fst.put32(0x428, DiscBuilder::kImageSize);  // FST size past the end
    CHECK(Disc::open(bad_fst.write("badfst")) == nullptr);

    // Too short to hold a header, and a missing file.
    const TempImage tiny("tiny", {'G', 'T', 'S', 'T', '0', '1'});
    CHECK(Disc::open(tiny) == nullptr);
    CHECK(Disc::open(std::filesystem::temp_directory_path() / "ezy_gc_does_not_exist.iso") == nullptr);
}

TEST_CASE("Dol loads text and data sections and clears BSS") {
    DiscBuilder builder;
    const std::vector<std::uint8_t> bytes(builder.image.begin() + DiscBuilder::kDolOffset, builder.image.end());

    const auto dol = Dol::parse(bytes);
    REQUIRE(dol.has_value());
    CHECK(dol->entry == DiscBuilder::kTextAddress);
    REQUIRE(dol->sections.size() == 2);
    CHECK(dol->sections[0].text);
    CHECK(dol->sections[0].offset == 0x100);
    CHECK(dol->sections[0].address == DiscBuilder::kTextAddress);
    CHECK(dol->sections[0].size == DiscBuilder::program().size() * 4);
    CHECK_FALSE(dol->sections[1].text);
    CHECK(dol->sections[1].address == DiscBuilder::kDataAddress);
    CHECK(dol->sections[1].size == 0x20);
    CHECK(dol->bss_address == DiscBuilder::kBssAddress);
    CHECK(dol->bss_size == DiscBuilder::kBssSize);
    CHECK(Dol::file_size(bytes) == 0x220);

    Memory memory;
    // Garbage where the BSS and the sections go.
    for (std::uint32_t a = 0x3000; a < 0x5200; a += 4) memory.write32(a, 0xCCCCCCCC);
    dol->load(bytes, memory, Memory::kMem1Size);
    const auto program = DiscBuilder::program();
    for (std::size_t i = 0; i < program.size(); ++i)
        CHECK(memory.read32(phys(DiscBuilder::kTextAddress) + static_cast<std::uint32_t>(i * 4)) == program[i]);
    CHECK(memory.read32(phys(DiscBuilder::kDataAddress)) == DiscBuilder::kDataWord);
    CHECK(memory.read32(phys(DiscBuilder::kDataAddress) + 0x1C) == 0xDA7A001C);
    for (std::uint32_t a = 0; a < DiscBuilder::kBssSize; a += 4) CHECK(memory.read32(phys(DiscBuilder::kBssAddress) + a) == 0);
    // Outside every section: untouched.
    CHECK(memory.read32(phys(DiscBuilder::kBssAddress) + DiscBuilder::kBssSize) == 0xCCCCCCCC);
    CHECK(memory.read32(phys(DiscBuilder::kDataAddress) + 0x20) == 0xCCCCCCCC);

    // A data section inside the BSS range (as .sdata sits inside MKDD's BSS range) survives:
    // the BSS is cleared before the sections are copied.
    Dol overlapping = *dol;
    overlapping.bss_address = DiscBuilder::kDataAddress;
    overlapping.bss_size = 0x100;
    overlapping.load(bytes, memory, Memory::kMem1Size);
    CHECK(memory.read32(phys(DiscBuilder::kDataAddress)) == DiscBuilder::kDataWord);
    CHECK(memory.read32(phys(DiscBuilder::kDataAddress) + 0x20) == 0);

    // Targets outside memory are refused before anything is written: a section at the
    // write-gather pipe's address, one ending past memory_end, a physical (not 0x8...) address,
    // a BSS past MEM1. The BSS garbage below stays.
    for (std::uint32_t a = 0; a < DiscBuilder::kBssSize; a += 4)
        memory.write32(phys(DiscBuilder::kBssAddress) + a, 0xCCCCCCCC);
    const auto refused = [&](const Dol& changed, std::uint32_t memory_end) {
        CHECK_THROWS_AS(changed.load(bytes, memory, memory_end), std::runtime_error);
        CHECK(memory.read32(phys(DiscBuilder::kBssAddress)) == 0xCCCCCCCC);
    };
    Dol wild = *dol;
    wild.sections[1].address = 0x8C008000;
    refused(wild, Memory::kMem1Size);
    refused(*dol, phys(DiscBuilder::kBssAddress) + DiscBuilder::kBssSize - 1);  // BSS 1 byte past the end
    wild = *dol;
    wild.sections[0].address = 0x00003100;
    refused(wild, Memory::kMem1Size);
    wild = *dol;
    wild.bss_address = 0x817FFF80;
    refused(wild, Memory::kMem1Size);
    dol->load(bytes, memory, phys(DiscBuilder::kBssAddress) + DiscBuilder::kBssSize);  // exactly at the end
    CHECK(memory.read32(phys(DiscBuilder::kBssAddress)) == 0);

    // Malformed DOLs.
    CHECK_FALSE(Dol::parse(std::span<const std::uint8_t>(bytes.data(), 0xFF)).has_value());  // short header
    CHECK_FALSE(Dol::parse(std::span<const std::uint8_t>(bytes.data(), 0x210)).has_value());  // data cut short
    std::vector<std::uint8_t> bad = bytes;
    bad[0x02] = 0x00;
    bad[0x03] = 0x10;  // text0 at offset 0x10: inside the header
    CHECK_FALSE(Dol::parse(bad).has_value());
}

TEST_CASE("hle_boot sets the low-memory globals, BATs, MSR, r1 and pc") {
    DiscBuilder builder;
    const auto image = builder.write("boot");
    auto disc = Disc::open(image);
    REQUIRE(disc != nullptr);
    Memory memory;
    Gekko cpu(memory);
    ezy::gc::hle_boot(cpu, memory, *disc);

    // The disc header's first 0x20 bytes.
    CHECK(memory.read32(0x00) == 0x47545354);  // "GTST"
    CHECK(memory.read16(0x04) == 0x3031);      // "01"
    CHECK(memory.read32(0x1C) == 0xC2339F3D);
    // The globals (YAGCD; Dolphin's SetupGCMemory).
    CHECK(memory.read32(phys(0x80000020)) == 0x0D15EA5E);
    CHECK(memory.read32(phys(0x80000024)) == 1);
    CHECK(memory.read32(phys(0x80000028)) == 0x01800000);
    CHECK(memory.read32(phys(0x8000002C)) == 0x00000003);
    CHECK(memory.read32(phys(0x800000CC)) == 0);  // NTSC (BI2 region USA)
    CHECK(memory.read32(phys(0x800000D0)) == 0x01000000);
    CHECK(memory.read32(phys(0x800000F0)) == 0x01800000);
    CHECK(memory.read32(phys(0x800000F8)) == 162000000);
    CHECK(memory.read32(phys(0x800000FC)) == 486000000);
    CHECK(memory.read32(phys(0x80000300)) == 0x4C000064);  // rfi
    CHECK(memory.read32(phys(0x80000800)) == 0x4C000064);
    CHECK(memory.read32(phys(0x80000C00)) == 0x4C000064);

    // The FST, copied to the top of MEM1.
    const std::uint32_t fst = memory.read32(phys(0x80000038));
    CHECK(fst == ((0x81800000 - DiscBuilder::fst_size()) & ~0x1Fu));
    CHECK(memory.read32(phys(0x8000003C)) == DiscBuilder::fst_size());
    const auto disc_fst = read_all(*disc, DiscBuilder::kFstOffset, DiscBuilder::fst_size());
    for (std::uint32_t i = 0; i < DiscBuilder::fst_size(); ++i) {
        CAPTURE(i);
        CHECK(memory.read8(phys(fst) + i) == disc_fst[i]);
    }
    // BI2 just below it; the arena ends below both.
    const std::uint32_t bi2 = memory.read32(phys(0x800000F4));
    CHECK(bi2 == fst - 0x2000);
    CHECK(memory.read32(phys(bi2) + 4) == 0x01800000);
    CHECK(memory.read32(phys(0x80000030)) == 0);
    CHECK(memory.read32(phys(0x80000034)) == bi2);

    // The DOL is loaded.
    CHECK(memory.read32(phys(DiscBuilder::kTextAddress)) == DiscBuilder::program()[0]);
    CHECK(memory.read32(phys(DiscBuilder::kDataAddress)) == DiscBuilder::kDataWord);

    // CPU state as the IPL leaves it (Dolphin's SetupMSR/SetupHID/SetupBAT).
    CHECK(cpu.pc() == DiscBuilder::kTextAddress);
    CHECK(cpu.msr() == 0x00002032);
    CHECK(cpu.gpr(1) == 0x816FFFFC);
    CHECK(cpu.spr(ezy::gc::spr::kHid0) == 0x0011C064);  // 0x0011C464 with DCFI self-cleared
    CHECK(cpu.spr(ezy::gc::spr::kHid2) == 0xE0000000);
    CHECK(cpu.spr(ezy::gc::spr::kIbat0u) == 0x80001FFF);
    CHECK(cpu.spr(ezy::gc::spr::kIbat0l) == 0x00000002);
    CHECK(cpu.spr(ezy::gc::spr::kDbat0u) == 0x80001FFF);
    CHECK(cpu.spr(ezy::gc::spr::kDbat0l) == 0x00000002);
    CHECK(cpu.spr(ezy::gc::spr::kDbat1u) == 0xC0001FFF);
    CHECK(cpu.spr(ezy::gc::spr::kDbat1l) == 0x0000002A);
    for (int i = 0; i < 8; ++i) CHECK(cpu.gqr(i) == 0);
    CHECK(cpu.translate(0x80003100, true, false) == 0x3100u);
    CHECK(cpu.translate(0xCC002000, false, true) == 0x0C002000u);
}

TEST_CASE("hle_boot refuses an FST or a DOL that does not fit in MEM1") {
    const auto boot_throws = [](const DiscBuilder& builder) {
        const auto image = builder.write("refused");
        auto disc = Disc::open(image);
        REQUIRE(disc != nullptr);  // the file itself is fine
        Memory memory;
        Gekko cpu(memory);
        CHECK_THROWS_AS(ezy::gc::hle_boot(cpu, memory, *disc), std::runtime_error);
    };
    // An FST maximum size in the header that cannot fit in MEM1 above the low globals with BI2.
    for (const std::uint32_t max_size : {0xFFFFFFFFu, 0x01800001u, 0x01800000u - 0x3100u - 0x2000u}) {
        CAPTURE(max_size);
        DiscBuilder builder;
        builder.put32(0x42C, max_size);
        boot_throws(builder);
    }
    // The largest that fits: BI2 lands at or above the end of the low globals.
    {
        DiscBuilder builder;
        builder.put32(0x42C, 0x01800000u - 0x3100u - 0x2000u - 0x20u);
        const auto image = builder.write("maxfst");
        auto disc = Disc::open(image);
        REQUIRE(disc != nullptr);
        Memory memory;
        Gekko cpu(memory);
        CHECK_THROWS_AS(ezy::gc::hle_boot(cpu, memory, *disc), std::runtime_error);  // the DOL is under it now
        CHECK(memory.read32(phys(0x800000F4)) >= 0x80003100u);
    }
    // A DOL section over the FST/BI2 area at the top of MEM1.
    {
        DiscBuilder builder;
        builder.put32(DiscBuilder::kDolOffset + 0x64, 0x817FE000);  // data0 address
        boot_throws(builder);
    }
    // A DOL whose sections run past the end of the disc.
    {
        DiscBuilder builder;
        builder.put32(DiscBuilder::kDolOffset + 0xAC, DiscBuilder::kImageSize);  // data0 size
        boot_throws(builder);
    }
}

TEST_CASE("hle_boot writes the boot time and refuses a PAL disc") {
    {
        DiscBuilder builder;
        const auto image = builder.write("ntsc_time");
        auto disc = Disc::open(image);
        REQUIRE(disc != nullptr);
        Memory memory;
        Gekko cpu(memory);
        ezy::gc::hle_boot(cpu, memory, *disc, {.seconds_since_2000 = 100});
        CHECK(memory.read64(phys(0x800030D8)) == 100ull * 40500000ull);
    }
    DiscBuilder builder;
    builder.put32(0x458, 2);  // BI2 region: PAL
    const auto image = builder.write("pal");
    auto disc = Disc::open(image);
    REQUIRE(disc != nullptr);
    Memory memory;
    Gekko cpu(memory);
    try {
        ezy::gc::hle_boot(cpu, memory, *disc, {.seconds_since_2000 = 100});
        FAIL("expected the PAL disc to be refused");
    } catch (const std::runtime_error& e) {
        CHECK(std::string(e.what()).find("PAL") != std::string::npos);
    }
}

TEST_CASE("a boot of the synthetic disc runs its DOL to a known result") {
    DiscBuilder builder;
    const auto image = builder.write("run");
    auto disc = Disc::open(image);
    REQUIRE(disc != nullptr);
    Memory memory;
    Gekko cpu(memory);
    ezy::gc::hle_boot(cpu, memory, *disc);
    for (int i = 0; i < 100; ++i) cpu.step();
    CHECK(memory.read32(phys(DiscBuilder::kResultAddress)) == 0x600DF00D);
    CHECK(memory.read32(phys(DiscBuilder::kResultAddress) + 4) == DiscBuilder::kDataWord);
    CHECK(cpu.pc() == DiscBuilder::kTextAddress + 6 * 4);  // spinning on its b 0
}

TEST_CASE("an access to an unmapped physical address throws with the address") {
    Memory memory;
    for (const std::uint32_t address : {0x01800000u, 0x08000000u, 0x0C007000u, 0x0C009000u, 0x10000000u,
                                        0xE0004000u, 0xFFF00100u}) {
        CAPTURE(address);
        try {
            (void)memory.read32(address);
            FAIL("expected BusError");
        } catch (const BusError& e) {
            CHECK(e.word_or_address == address);
            CHECK_FALSE(e.pc_known);
        }
        CHECK_THROWS_AS(memory.write8(address, 0), BusError);
        CHECK_THROWS_AS(memory.read16(address), BusError);
        CHECK_THROWS_AS(memory.write64(address, 0), BusError);
    }
    // The last bytes of MEM1 and of the locked cache (once HID2[LCE] enables it) are mapped.
    memory.write32(0x017FFFFC, 1);
    CHECK(memory.read32(0x017FFFFC) == 1);
    CHECK_THROWS_AS(memory.write32(0xE0003FFC, 2), BusError);
    memory.set_locked_cache_enabled(true);
    memory.write32(0xE0003FFC, 2);
    CHECK(memory.read32(0xE0003FFC) == 2);
}

// The MKDD dump (an NKit ISO, which is still a valid GCM). Runs only on a machine that has it.
TEST_CASE("the MKDD NKit image opens and its DOL fits in MEM1") {
    const std::filesystem::path path = "E:/Rom nintendo/Mario Kart Double Dash/Mario Kart - Double Dash!! (USA).nkit.iso";
    if (!std::filesystem::exists(path)) {
        MESSAGE("MKDD image not found; skipped: " << path.string());
        return;
    }
    auto disc = Disc::open(path);
    REQUIRE(disc != nullptr);
    CHECK(disc->size() == 490999808u);
    CHECK(disc->game_id() == "GM4E01");
    CHECK(disc->dol_offset() == 0x1F700);
    CHECK(disc->fst_offset() == 0x387B00);
    CHECK(disc->fst_size() == 0x1313);
    std::vector<std::uint8_t> marker(8);
    REQUIRE(disc->read(0x200, marker));
    CHECK(std::string(marker.begin(), marker.end()) == "NKIT v01");

    std::vector<std::uint8_t> header(Dol::kHeaderSize);
    REQUIRE(disc->read(disc->dol_offset(), header));
    const auto size = Dol::file_size(header);
    REQUIRE(size.has_value());
    const auto bytes = read_all(*disc, disc->dol_offset(), *size);
    const auto dol = Dol::parse(bytes);
    REQUIRE(dol.has_value());
    const auto in_mem1 = [](std::uint32_t address, std::uint32_t length) {
        return address >= 0x80000000u && address + length <= 0x81800000u && address + length >= address;
    };
    CHECK(in_mem1(dol->entry, 4));
    CHECK(dol->sections.size() == 11);  // 2 text + 9 data
    for (const auto& section : dol->sections) {
        CAPTURE(section.address);
        CHECK(in_mem1(section.address, section.size));
    }
    CHECK(in_mem1(dol->bss_address, dol->bss_size));
    MESSAGE("MKDD DOL: entry " << hex(dol->entry) << ", " << dol->sections.size() << " sections, " << *size
                               << " bytes");

    // The HLE boot of the real image.
    Memory memory;
    Gekko cpu(memory);
    ezy::gc::hle_boot(cpu, memory, *disc);
    CHECK(cpu.pc() == dol->entry);
    CHECK(memory.read32(0) == 0x474D3445);  // "GM4E"
    const std::uint32_t fst = memory.read32(phys(0x80000038));
    CHECK(fst == 0x817FECE0);  // (0x81800000 - 0x1313) & ~0x1F
    CHECK(memory.read8(phys(fst)) == 1);  // the root entry is a directory
    for (const auto& section : dol->sections)
        CHECK(memory.read32(section.address & 0x3FFFFFFF) == ((std::uint32_t{bytes[section.offset]} << 24) |
                                                              (std::uint32_t{bytes[section.offset + 1]} << 16) |
                                                              (std::uint32_t{bytes[section.offset + 2]} << 8) |
                                                              bytes[section.offset + 3]));

    // Then the CPU runs until the first thing this sub-project has not built yet (an MMIO
    // block, Tasks 4-6, or an SPR/instruction Task 7 adds) or 2 million instructions. Where it
    // stops is reported, not checked: it moves as later tasks land. What is checked is that
    // the crt0 got going (thousands of instructions ran through the boot BATs).
    int executed = 0;
    try {
        for (; executed < 2'000'000; ++executed) cpu.step();
        MESSAGE("MKDD ran 2M instructions; pc " << hex(cpu.pc()));
    } catch (const ezy::gc::GekkoError& e) {
        MESSAGE("MKDD stopped after " << executed << " instructions: " << std::string(e.what()));
    }
    CHECK(executed > 1000);
}
