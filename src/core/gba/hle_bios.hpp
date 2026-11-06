#pragma once

#include <array>
#include <cstdint>

namespace ezy::gba {

// Our own minimal BIOS, used when the user doesn't load a BIOS dump. It contains no
// Nintendo code: just the exception vectors and the IRQ trampoline every game relies on.
// BIOS calls (SWI) are emulated in C++ instead (Gba::on_swi); a SWI that isn't emulated
// lands on vector 0x08 here and simply returns.
//
// The IRQ trampoline does what the real BIOS does: save the scratch registers, call the
// game's handler whose address the game stored at 0x03007FFC (read through its mirror
// 0x03FFFFFC = 0x04000000 - 4), restore and return from the exception.
inline constexpr std::array<std::uint32_t, 12> kHleBiosCode = {
    0xEAFFFFFE,  // 0x00 reset:          B .        (we never boot through the BIOS)
    0xE1B0F00E,  // 0x04 undefined:      MOVS pc, lr
    0xE1B0F00E,  // 0x08 SWI:            MOVS pc, lr (unemulated call: no-op)
    0xE25EF004,  // 0x0C prefetch abort: SUBS pc, lr, #4
    0xE25EF008,  // 0x10 data abort:     SUBS pc, lr, #8
    0xEAFFFFFE,  // 0x14 reserved:       B .
    0xE92D500F,  // 0x18 IRQ:            STMFD sp!, {r0-r3, r12, lr}
    0xE3A00301,  // 0x1C                 MOV   r0, #0x04000000
    0xE28FE000,  // 0x20                 ADD   lr, pc, #0          ; return to 0x28
    0xE510F004,  // 0x24                 LDR   pc, [r0, #-4]       ; game's handler
    0xE8BD500F,  // 0x28                 LDMFD sp!, {r0-r3, r12, lr}
    0xE25EF004,  // 0x2C                 SUBS  pc, lr, #4          ; return from IRQ
};

// The same code as bytes, ready to be mapped at 0x00000000.
[[nodiscard]] constexpr std::array<std::uint8_t, kHleBiosCode.size() * 4> hle_bios_image() {
    std::array<std::uint8_t, kHleBiosCode.size() * 4> bytes{};
    for (std::size_t i = 0; i < kHleBiosCode.size(); ++i) {
        for (std::size_t b = 0; b < 4; ++b) bytes[i * 4 + b] = static_cast<std::uint8_t>(kHleBiosCode[i] >> (8 * b));
    }
    return bytes;
}

// Where the BIOS and games keep their IRQ bookkeeping (GBATEK "BIOS RAM Usage").
inline constexpr std::uint32_t kBiosIrqHandlerAddr = 0x03007FFC;  // game's IRQ handler
inline constexpr std::uint32_t kBiosIfAddr = 0x03007FF8;          // IntrWait flags, set by the game's handler

}  // namespace ezy::gba
