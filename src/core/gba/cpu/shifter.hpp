#pragma once

#include <bit>
#include <cstdint>

namespace ezy::gba::shifter {

// The ARM7TDMI barrel shifter. Every function returns the shifted value and writes the
// shifter carry-out to `carry` (which holds the current C flag on entry, because several
// cases leave it unchanged). Pure functions, so they are easy to test exhaustively.
//
// "imm" variants take the 5-bit amount encoded in the instruction, where 0 has special
// meanings (LSR/ASR #0 mean #32, ROR #0 means RRX). "reg" variants take the bottom byte of
// a register, where 0 means "no shift, carry unchanged" and amounts >= 32 are defined.

enum class Type : std::uint32_t { Lsl = 0, Lsr = 1, Asr = 2, Ror = 3 };

constexpr std::uint32_t bit(std::uint32_t value, std::uint32_t n) { return (value >> n) & 1; }

constexpr std::uint32_t lsl(std::uint32_t value, std::uint32_t amount, bool& carry) {
    if (amount == 0) return value;
    if (amount < 32) {
        carry = bit(value, 32 - amount) != 0;
        return value << amount;
    }
    carry = amount == 32 && bit(value, 0) != 0;
    return 0;
}

constexpr std::uint32_t lsr(std::uint32_t value, std::uint32_t amount, bool& carry) {
    if (amount == 0) return value;
    if (amount < 32) {
        carry = bit(value, amount - 1) != 0;
        return value >> amount;
    }
    carry = amount == 32 && bit(value, 31) != 0;
    return 0;
}

constexpr std::uint32_t asr(std::uint32_t value, std::uint32_t amount, bool& carry) {
    if (amount == 0) return value;
    if (amount < 32) {
        carry = bit(value, amount - 1) != 0;
        return static_cast<std::uint32_t>(static_cast<std::int32_t>(value) >> amount);
    }
    carry = bit(value, 31) != 0;
    return carry ? 0xFFFFFFFFU : 0;
}

constexpr std::uint32_t ror(std::uint32_t value, std::uint32_t amount, bool& carry) {
    if (amount == 0) return value;
    amount &= 31;
    if (amount == 0) {  // a multiple of 32: value unchanged, carry = bit 31
        carry = bit(value, 31) != 0;
        return value;
    }
    carry = bit(value, amount - 1) != 0;
    return std::rotr(value, static_cast<int>(amount));
}

constexpr std::uint32_t rrx(std::uint32_t value, bool& carry) {
    const std::uint32_t result = (carry ? 0x80000000U : 0) | (value >> 1);
    carry = bit(value, 0) != 0;
    return result;
}

// Shift by the 5-bit immediate from the instruction encoding.
constexpr std::uint32_t by_imm(Type type, std::uint32_t value, std::uint32_t amount, bool& carry) {
    switch (type) {
        case Type::Lsl: return lsl(value, amount, carry);
        case Type::Lsr: return lsr(value, amount == 0 ? 32 : amount, carry);
        case Type::Asr: return asr(value, amount == 0 ? 32 : amount, carry);
        default: return amount == 0 ? rrx(value, carry) : ror(value, amount, carry);
    }
}

// Shift by the bottom byte of a register.
constexpr std::uint32_t by_reg(Type type, std::uint32_t value, std::uint32_t amount, bool& carry) {
    amount &= 0xFF;
    switch (type) {
        case Type::Lsl: return lsl(value, amount, carry);
        case Type::Lsr: return lsr(value, amount, carry);
        case Type::Asr: return asr(value, amount, carry);
        default: return ror(value, amount, carry);
    }
}

}  // namespace ezy::gba::shifter
