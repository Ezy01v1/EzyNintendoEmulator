#pragma once

#include <cstdint>

#include "gb/cpu_bus.hpp"

namespace ezy::gb {

// Flag bits in register F (the low nibble is always 0).
namespace flag {
inline constexpr std::uint8_t kZ = 0x80;  // zero
inline constexpr std::uint8_t kN = 0x40;  // subtract
inline constexpr std::uint8_t kH = 0x20;  // half carry (bit 3 -> 4)
inline constexpr std::uint8_t kC = 0x10;  // carry
}  // namespace flag

struct Registers {
    std::uint8_t a = 0, f = 0, b = 0, c = 0, d = 0, e = 0, h = 0, l = 0;
    std::uint16_t sp = 0, pc = 0;

    [[nodiscard]] std::uint16_t af() const { return pair(a, f); }
    [[nodiscard]] std::uint16_t bc() const { return pair(b, c); }
    [[nodiscard]] std::uint16_t de() const { return pair(d, e); }
    [[nodiscard]] std::uint16_t hl() const { return pair(h, l); }
    void set_af(std::uint16_t v) { split(v, a, f); f &= 0xF0; }
    void set_bc(std::uint16_t v) { split(v, b, c); }
    void set_de(std::uint16_t v) { split(v, d, e); }
    void set_hl(std::uint16_t v) { split(v, h, l); }

    [[nodiscard]] bool flag(std::uint8_t mask) const { return (f & mask) != 0; }
    void set_flag(std::uint8_t mask, bool on) {
        f = static_cast<std::uint8_t>(on ? (f | mask) : (f & ~mask));
    }

private:
    static std::uint16_t pair(std::uint8_t hi, std::uint8_t lo) {
        return static_cast<std::uint16_t>((hi << 8) | lo);
    }
    static void split(std::uint16_t v, std::uint8_t& hi, std::uint8_t& lo) {
        hi = static_cast<std::uint8_t>(v >> 8);
        lo = static_cast<std::uint8_t>(v & 0xFF);
    }
};

// Sharp SM83 (the Game Boy CPU). Executes one instruction per step(); every memory
// access and internal cycle goes through the CpuBus, which advances the rest of the system.
class Cpu {
public:
    explicit Cpu(CpuBus& bus) : bus_(bus) {}

    // Register state the DMG boot ROM leaves behind (Pan Docs, "Power Up Sequence").
    // We never run the boot ROM itself: it is Nintendo's copyrighted code.
    void reset_post_boot();

    void step();  // services an interrupt or executes one instruction

    [[nodiscard]] Registers& regs() { return r_; }
    [[nodiscard]] const Registers& regs() const { return r_; }
    [[nodiscard]] bool ime() const { return ime_; }
    void set_ime(bool on) { ime_ = on; }
    [[nodiscard]] bool halted() const { return halted_; }
    [[nodiscard]] bool locked() const { return locked_; }  // hit an illegal opcode

private:
    // Memory helpers (each read/write is one M-cycle).
    std::uint8_t fetch_opcode();
    std::uint8_t read8();
    std::uint16_t read16();
    void push16(std::uint16_t value);
    std::uint16_t pop16();

    // Operand helpers. r: B C D E H L (HL) A.  rp: BC DE HL SP.  rp2: BC DE HL AF.
    std::uint8_t read_r(int index);
    void write_r(int index, std::uint8_t value);
    [[nodiscard]] std::uint16_t read_rp(int index) const;
    void write_rp(int index, std::uint16_t value);
    [[nodiscard]] std::uint16_t read_rp2(int index) const;
    void write_rp2(int index, std::uint16_t value);
    [[nodiscard]] bool condition(int cc) const;  // NZ Z NC C

    // Instruction groups.
    void execute(std::uint8_t opcode);
    void execute_block0(int y, int z, int p, int q);
    void execute_block3(int y, int z, int p, int q);
    void execute_cb();
    void service_interrupt();

    // Operations.
    void jr(bool taken);
    void call(std::uint16_t addr);
    void halt();
    void alu(int op, std::uint8_t value);
    void add8(std::uint8_t value, bool carry);
    std::uint8_t sub8(std::uint8_t value, bool carry);
    std::uint8_t inc8(std::uint8_t value);
    std::uint8_t dec8(std::uint8_t value);
    void add_hl(std::uint16_t value);
    std::uint16_t sp_plus_offset();  // for ADD SP,e and LD HL,SP+e
    std::uint8_t rotate_shift(int op, std::uint8_t value);
    void daa();

    CpuBus& bus_;
    Registers r_;
    bool ime_ = false;
    bool ime_pending_ = false;  // EI enables interrupts after the next instruction
    bool halted_ = false;
    bool halt_bug_ = false;     // next opcode fetch does not increment PC
    bool locked_ = false;
};

}  // namespace ezy::gb
