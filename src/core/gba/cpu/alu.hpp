#pragma once

#include <cstdint>

namespace ezy::gba::alu {

// Arithmetic shared by the ARM and Thumb instruction sets.

struct AddResult {
    std::uint32_t value;
    bool carry;
    bool overflow;
};

// a + b + carry_in, with ARM carry/overflow. Subtraction is a + ~b + 1 (carry = no borrow).
constexpr AddResult add_with_carry(std::uint32_t a, std::uint32_t b, bool carry_in) {
    const std::uint64_t wide = std::uint64_t{a} + b + (carry_in ? 1 : 0);
    const auto value = static_cast<std::uint32_t>(wide);
    return {value, (wide >> 32) != 0, ((~(a ^ b) & (a ^ value)) >> 31) != 0};
}

constexpr AddResult add(std::uint32_t a, std::uint32_t b) { return add_with_carry(a, b, false); }
constexpr AddResult sub(std::uint32_t a, std::uint32_t b) { return add_with_carry(a, ~b, true); }

}  // namespace ezy::gba::alu
