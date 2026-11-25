#pragma once

#include <cstdint>

namespace ezy::gc {

class Disc;
class Gekko;
class Memory;

struct HleBootOptions {
    // The boot time the IPL leaves at 0x800030D8, as seconds since 2000-01-01 (the RTC's
    // epoch). 0 by default so a boot is deterministic; the machine passes its RTC value.
    std::uint64_t seconds_since_2000 = 0;
};

// Leaves the machine as the IPL (BS2) and the disc's apploader leave it when they jump to the
// game (design, "Arranque HLE"): the disc header and the low-memory globals, BI2 and the FST
// at the top of MEM1, the DOL loaded (BSS cleared), and the CPU with the boot BATs, MSR, HID0,
// HID2, GQRs, r1 and pc = the DOL's entry. The CPU is reset first; `memory` should be fresh
// (nothing else is cleared). Throws std::runtime_error when the disc's FST (with BI2) does
// not fit in MEM1 above the low globals, or its DOL cannot be read or parsed, or would load
// outside MEM1 or over the FST/BI2 area; nothing of the DOL is written then.
//
// Unlike Dolphin's HLE boot (Boot_BS2Emu.cpp, EmulatedBS2_GC), which runs the disc's
// apploader on the emulated CPU to load the DOL and the FST, this loads them directly and
// writes what the apploader would (0x80000024, 0x80000034-3C, 0x800000F0/F4, BI2). Task 7
// decides whether MKDD needs the real apploader.
void hle_boot(Gekko& cpu, Memory& memory, Disc& disc, const HleBootOptions& options = {});

}  // namespace ezy::gc
