#pragma once

#include <cstdint>

namespace ezy::gb::interrupt {

// Interrupt bits, shared by IE (0xFFFF) and IF (0xFF0F). Lower bit = higher priority.
inline constexpr std::uint8_t kVBlank = 1 << 0;
inline constexpr std::uint8_t kStat = 1 << 1;
inline constexpr std::uint8_t kTimer = 1 << 2;
inline constexpr std::uint8_t kSerial = 1 << 3;
inline constexpr std::uint8_t kJoypad = 1 << 4;

}  // namespace ezy::gb::interrupt
