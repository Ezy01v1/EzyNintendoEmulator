#pragma once

#include <cstdint>
#include <string>

namespace ezy::chip8 {

// Turns an opcode into a readable mnemonic (Cowgod's notation), e.g. 0x6A2F -> "LD   VA, 2F".
[[nodiscard]] std::string disassemble(std::uint16_t opcode);

}  // namespace ezy::chip8
