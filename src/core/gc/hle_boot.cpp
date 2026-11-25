#include "gc/hle_boot.hpp"

#include <algorithm>
#include <stdexcept>
#include <string>
#include <vector>

#include "gc/disc.hpp"
#include "gc/dol.hpp"
#include "gc/gekko/gekko.hpp"
#include "gc/memory.hpp"

// Sources for every value below (read on the web, nothing copied):
// - YAGCD (Yet Another GameCube Documentation), chapter 4 "Dolphin OS globals": the low-memory
//   map and who writes each word (the IPL, the apploader, or OSInit).
// - Dolphin, Source/Core/Core/Boot/Boot_BS2Emu.cpp: SetupMSR, SetupHID, SetupBAT,
//   SetupGCMemory and EmulatedBS2_GC (its HLE of the IPL, which then runs the apploader).
// - WiiBrew "Memory map" (its low-memory table is shared with the GameCube), for the values
//   the apploader leaves (0x80000024, 0x800000EC, 0x800000F0, 0x800000F4).
// - The MKDD decompilation (SwareJonge/mkdd, libs/dolphin/os/OS.c and OSMemory.c), for what
//   this game's SDK reads: OSInit takes ArenaLo/ArenaHi from 0x80000030/34 (0 = its linker
//   default), BI2 from 0x800000F4 (debug flag, pad spec), and __OSInitMemoryProtection the
//   simulated memory size at 0x800000F0.

namespace ezy::gc {

namespace {

constexpr std::uint32_t phys(std::uint32_t cached) { return cached - 0x80000000u; }

std::uint32_t be32(std::span<const std::uint8_t> bytes, std::size_t at) {
    return (static_cast<std::uint32_t>(bytes[at]) << 24) | (static_cast<std::uint32_t>(bytes[at + 1]) << 16) |
           (static_cast<std::uint32_t>(bytes[at + 2]) << 8) | bytes[at + 3];
}

constexpr std::uint32_t kMem1Top = 0x81800000;
constexpr std::uint32_t kRfi = 0x4C000064;

// The low globals and exception vectors end here; the DOL's text usually starts at it. The
// FST and BI2 placed at the top of MEM1 must stay above it.
constexpr std::uint32_t kLowMemoryEnd = 0x80003100;

// Reads, parses and loads the disc's DOL below `memory_end` (physical); returns its entry.
std::uint32_t load_dol(Memory& memory, Disc& disc, std::uint32_t memory_end) {
    std::vector<std::uint8_t> bytes(Dol::kHeaderSize);
    if (!disc.read(disc.dol_offset(), bytes)) throw std::runtime_error("hle_boot: cannot read the DOL header");
    const auto size = Dol::file_size(bytes);
    if (!size) throw std::runtime_error("hle_boot: bad DOL header");
    if (*size > Memory::kMem1Size + Dol::kHeaderSize) throw std::runtime_error("hle_boot: the DOL is larger than MEM1");
    if (std::uint64_t{disc.dol_offset()} + *size > disc.size())
        throw std::runtime_error("hle_boot: the DOL runs past the end of the disc");
    bytes.resize(*size);
    if (!disc.read(disc.dol_offset(), bytes)) throw std::runtime_error("hle_boot: the DOL runs past the end of the disc");
    const auto dol = Dol::parse(bytes);
    if (!dol) throw std::runtime_error("hle_boot: bad DOL");
    dol->load(bytes, memory, memory_end);
    return dol->entry;
}

}  // namespace

void hle_boot(Gekko& cpu, Memory& memory, Disc& disc, const HleBootOptions& options) {
    cpu.reset();
    const std::span<const std::uint8_t> header = disc.header();
    const std::span<const std::uint8_t> bi2 = header.subspan(Disc::kBi2Offset, Disc::kBi2Size);
    std::span<std::uint8_t> mem1 = memory.mem1();

    // --- Disc header (IPL: YAGCD "B"): game code, maker, disc number and version, the
    // audio-streaming flag and buffer size, the magic words at 0x18/0x1C. Dolphin's
    // EmulatedBS2_GC reads the same 0x20 bytes to 0 (DVDReadDiscID). ---
    std::copy_n(header.begin(), 0x20, mem1.begin());

    // --- Globals the IPL writes (Dolphin's SetupGCMemory; YAGCD) ---
    memory.write32(phys(0x80000020), 0x0D15EA5E);  // boot magic: "booted by the IPL"
    memory.write32(phys(0x80000028), Memory::kMem1Size);  // physical memory size, 24 MiB
    // Console type: retail, "latest production board" = 3 (YAGCD; the design asks for
    // retail). Dolphin writes its LatestDevkit value (0x10000006) instead, with a TODO that some
    // games fail with the retail id; if MKDD turns out to be one, Task 7 changes this.
    memory.write32(phys(0x8000002C), 0x00000003);
    // Video mode: 0 NTSC, 1 PAL (YAGCD; Dolphin: 0 for NTSC discs, 1 otherwise). The disc's
    // region is BI2's country code (+0x18: 0 Japan, 1 USA, 2 PAL).
    // Only NTSC timing is modeled (the VI refuses PAL), so a PAL disc stops here instead of
    // running 50 Hz code against a 59.94 Hz retrace.
    if (be32(bi2, 0x18) == 2)
        throw std::runtime_error("hle_boot: this is a PAL disc (BI2 region 2); only NTSC video timing is modeled");
    memory.write32(phys(0x800000CC), 0);
    memory.write32(phys(0x800000D0), 0x01000000);  // ARAM size, 16 MiB
    memory.write32(phys(0x800000F8), 162000000);   // bus clock (Dolphin: 0x09A7EC80)
    memory.write32(phys(0x800000FC), 486000000);   // CPU clock (Dolphin: 0x1CF7C580)
    // The boot time in time-base ticks (40.5 MHz) since 2000 (YAGCD: "OS system time, set at
    // power-up"; Dolphin's PresetTimeBaseTicks). 64 bits.
    memory.write64(phys(0x800030D8), options.seconds_since_2000 * 40500000ull);
    // Dolphin's SetupGCMemory leaves an rfi at the DSI, FP-unavailable and system-call
    // vectors, so an exception taken before OSInit installs its handlers returns.
    memory.write32(phys(0x80000300), kRfi);
    memory.write32(phys(0x80000800), kRfi);
    memory.write32(phys(0x80000C00), kRfi);

    // --- What the apploader leaves (YAGCD "A"; WiiBrew), since the DOL is loaded directly ---
    memory.write32(phys(0x80000024), 1);  // version (WiiBrew: "set by the apploader", 1)
    // The FST goes at the top of MEM1, 32-byte aligned, with room for its maximum size (the
    // size of the largest disc's FST in a multi-disc game); BI2 (0x2000 bytes) right below it.
    // The header's sizes are only known to fit in the file (Disc::open), so check they fit
    // in MEM1 above the low globals, with BI2, before computing any address from them.
    const std::uint32_t fst_room = std::max(disc.fst_size(), disc.fst_max_size());
    if (std::uint64_t{fst_room} + 0x1F + Disc::kBi2Size > kMem1Top - kLowMemoryEnd)
        throw std::runtime_error("hle_boot: the disc's FST size (" + std::to_string(fst_room) +
                                 " bytes) does not fit in MEM1");
    const std::uint32_t fst = (kMem1Top - fst_room) & ~0x1Fu;
    const std::uint32_t bi2_copy = fst - Disc::kBi2Size;
    if (!disc.read(disc.fst_offset(), mem1.subspan(phys(fst), disc.fst_size())))
        throw std::runtime_error("hle_boot: cannot read the FST");
    std::copy(bi2.begin(), bi2.end(), mem1.begin() + phys(bi2_copy));
    memory.write32(phys(0x80000038), fst);              // FST location
    memory.write32(phys(0x8000003C), fst_room);         // FST maximum length
    memory.write32(phys(0x800000F4), bi2_copy);         // BI2 location
    // Simulated memory size from BI2 (+4), as the apploader copies it; "usually the same as
    // the physical size" (YAGCD). __OSInitMemoryProtection maps the BATs by it.
    const std::uint32_t simulated = be32(bi2, 4);
    memory.write32(phys(0x800000F0), simulated != 0 ? simulated : Memory::kMem1Size);
    // Debug monitor size (BI2 +0, usually 0) and location ("usually the top of main memory":
    // YAGCD; WiiBrew's default 0x81800000). No monitor runs, so this is below nothing.
    memory.write32(phys(0x800000E8), be32(bi2, 0));
    memory.write32(phys(0x800000EC), kMem1Top - be32(bi2, 0));
    // The arena: ArenaLo = 0, so OSInit uses the DOL's own __ArenaLo (YAGCD shows 0 there);
    // ArenaHi = below BI2 and the FST so the game's heap never overwrites them. (On the Wii,
    // where the apploader's values are documented, ArenaHi = the FST address and BI2 sits
    // below it, inside the arena: here BI2 is kept out of it too, which only costs 8 KiB.)
    memory.write32(phys(0x80000030), 0);
    memory.write32(phys(0x80000034), bi2_copy);

    // The DOL must stay below BI2 and the FST (and inside MEM1).
    const std::uint32_t entry = load_dol(memory, disc, phys(bi2_copy));

    // --- CPU registers as the IPL leaves them (Dolphin's SetupMSR/SetupHID/SetupBAT) ---
    // MSR = 0x00002032: FP, IR, DR, RI (exception vectors at 0: IP clear).
    cpu.set_msr(Gekko::kMsrFp | Gekko::kMsrIr | Gekko::kMsrDr | Gekko::kMsrRi);
    // HID0 = 0x0011C464 (Dolphin: BHT, BTIC, DCFA, DCFI, DCE, ICE, NHR, DPM). DCE makes dcbz
    // legal; DCFI clears itself (Gekko::try_write_spr), so it reads back as 0x0011C064.
    cpu.set_spr(spr::kHid0, 0x0011C464);
    // HID2 = 0xE0000000: LSQE (psq_l/psq_st D-forms legal), WPE (write-gather pipe), PSE.
    cpu.set_spr(spr::kHid2, 0xE0000000);
    // IBAT0/DBAT0: 0x80000000 -> physical 0, 256 MiB, cached, read/write; DBAT1: 0xC0000000 ->
    // physical 0, 256 MiB, cache-inhibited and guarded (the hardware registers).
    cpu.set_spr(spr::kIbat0u, 0x80001FFF);
    cpu.set_spr(spr::kIbat0l, 0x00000002);
    cpu.set_spr(spr::kDbat0u, 0x80001FFF);
    cpu.set_spr(spr::kDbat0l, 0x00000002);
    cpu.set_spr(spr::kDbat1u, 0xC0001FFF);
    cpu.set_spr(spr::kDbat1l, 0x0000002A);
    // GQR0-7 = 0 (float, unscaled). Dolphin's boot leaves them at their reset value, 0; the
    // SDK's crt0 (__OSPSInit) clears GQR1-7 itself and relies on GQR0 = 0, which
    // Gekko::reset() already gives: written here only to say so.
    for (int i = 0; i < 8; ++i) cpu.set_gqr(i, 0);
    // r1: a stack below the default ArenaHi (design; the crt0 sets its own stack, r2 and r13
    // before it calls anything). Dolphin leaves the apploader's r1/r2/r13 here instead.
    cpu.set_gpr(1, 0x816FFFFC);
    cpu.set_pc(entry);
}

}  // namespace ezy::gc
