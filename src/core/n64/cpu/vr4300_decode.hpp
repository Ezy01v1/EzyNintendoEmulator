// VR4300 instruction decoding shared by the interpreter and the JIT.
#pragma once

#include <cstdint>

namespace ezy::n64 {

// The memory range a Memory instruction touches: `bytes` at its address, rounded down to
// `bytes` for LWL/LWR/LDL/LDR/SWL/SWR/SDL/SDR. bytes == 0: not a Memory instruction.
struct MemoryAccess {
    std::uint8_t bytes = 0;
    bool aligned_down = false;
    bool store = false;
};
[[nodiscard]] MemoryAccess memory_access(std::uint32_t instr);

}  // namespace ezy::n64
