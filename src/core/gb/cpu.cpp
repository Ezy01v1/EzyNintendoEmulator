#include "gb/cpu.hpp"

#include <bit>

namespace ezy::gb {

namespace {

template <typename T>
constexpr std::uint8_t u8(T v) {
    return static_cast<std::uint8_t>(v);
}
template <typename T>
constexpr std::uint16_t u16(T v) {
    return static_cast<std::uint16_t>(v);
}

}  // namespace

void Cpu::reset_post_boot() {
    r_.set_af(0x01B0);
    r_.set_bc(0x0013);
    r_.set_de(0x00D8);
    r_.set_hl(0x014D);
    r_.sp = 0xFFFE;
    r_.pc = 0x0100;
    ime_ = ime_pending_ = halted_ = halt_bug_ = locked_ = false;
}

// ---------------------------------------------------------------------------
// Step, interrupts, HALT

void Cpu::step() {
    if (locked_) {
        bus_.tick();
        return;
    }
    if (halted_) {
        if (bus_.pending_interrupts() == 0) {
            bus_.tick();
            return;
        }
        halted_ = false;  // any pending interrupt wakes the CPU, even with IME off
    }
    if (ime_ && bus_.pending_interrupts() != 0) {
        service_interrupt();
        return;
    }

    const bool enable_ime_after = ime_pending_;
    execute(fetch_opcode());
    if (enable_ime_after && ime_pending_) {  // a DI in between cancels the EI
        ime_ = true;
        ime_pending_ = false;
    }
}

void Cpu::service_interrupt() {
    // 5 M-cycles: 2 wait states, push PC (2), jump (1). Lowest bit has priority.
    const std::uint8_t pending = bus_.pending_interrupts();
    const int index = std::countr_zero(pending);
    bus_.tick();
    bus_.tick();
    push16(r_.pc);
    bus_.acknowledge_interrupt(u8(1 << index));
    r_.pc = u16(0x40 + index * 8);
    ime_ = false;
    bus_.tick();
}

void Cpu::halt() {
    if (!ime_ && bus_.pending_interrupts() != 0) {
        halt_bug_ = true;  // HALT is skipped and the next byte is read twice
    } else {
        halted_ = true;
    }
}

// ---------------------------------------------------------------------------
// Memory and operand helpers

std::uint8_t Cpu::fetch_opcode() {
    const std::uint8_t op = bus_.read(r_.pc);
    if (halt_bug_) {
        halt_bug_ = false;
    } else {
        ++r_.pc;
    }
    return op;
}

std::uint8_t Cpu::read8() { return bus_.read(r_.pc++); }

std::uint16_t Cpu::read16() {
    const std::uint8_t lo = read8();
    const std::uint8_t hi = read8();
    return u16((hi << 8) | lo);
}

void Cpu::push16(std::uint16_t value) {
    bus_.write(--r_.sp, u8(value >> 8));
    bus_.write(--r_.sp, u8(value & 0xFF));
}

std::uint16_t Cpu::pop16() {
    const std::uint8_t lo = bus_.read(r_.sp++);
    const std::uint8_t hi = bus_.read(r_.sp++);
    return u16((hi << 8) | lo);
}

std::uint8_t Cpu::read_r(int index) {
    switch (index) {
        case 0: return r_.b;
        case 1: return r_.c;
        case 2: return r_.d;
        case 3: return r_.e;
        case 4: return r_.h;
        case 5: return r_.l;
        case 6: return bus_.read(r_.hl());
        default: return r_.a;
    }
}

void Cpu::write_r(int index, std::uint8_t value) {
    switch (index) {
        case 0: r_.b = value; break;
        case 1: r_.c = value; break;
        case 2: r_.d = value; break;
        case 3: r_.e = value; break;
        case 4: r_.h = value; break;
        case 5: r_.l = value; break;
        case 6: bus_.write(r_.hl(), value); break;
        default: r_.a = value; break;
    }
}

std::uint16_t Cpu::read_rp(int index) const {
    switch (index) {
        case 0: return r_.bc();
        case 1: return r_.de();
        case 2: return r_.hl();
        default: return r_.sp;
    }
}

void Cpu::write_rp(int index, std::uint16_t value) {
    switch (index) {
        case 0: r_.set_bc(value); break;
        case 1: r_.set_de(value); break;
        case 2: r_.set_hl(value); break;
        default: r_.sp = value; break;
    }
}

std::uint16_t Cpu::read_rp2(int index) const { return index == 3 ? r_.af() : read_rp(index); }

void Cpu::write_rp2(int index, std::uint16_t value) {
    if (index == 3) {
        r_.set_af(value);
    } else {
        write_rp(index, value);
    }
}

bool Cpu::condition(int cc) const {
    switch (cc) {
        case 0: return !r_.flag(flag::kZ);
        case 1: return r_.flag(flag::kZ);
        case 2: return !r_.flag(flag::kC);
        default: return r_.flag(flag::kC);
    }
}

// ---------------------------------------------------------------------------
// Decoding
//
// Opcodes are split into fields:  x = bits 7-6, y = bits 5-3, z = bits 2-0,
// p = y >> 1, q = y & 1. Each (x, z) pair is a family of related instructions.
// Reference: "Decoding Gamboy Z80 opcodes" (gb-archive) and Pan Docs.

void Cpu::execute(std::uint8_t opcode) {
    const int x = opcode >> 6;
    const int y = (opcode >> 3) & 7;
    const int z = opcode & 7;
    const int p = y >> 1;
    const int q = y & 1;

    switch (x) {
        case 0: execute_block0(y, z, p, q); break;
        case 1:  // LD r[y], r[z]  (0x76 would be LD (HL),(HL): it is HALT instead)
            if (opcode == 0x76) {
                halt();
            } else {
                write_r(y, read_r(z));
            }
            break;
        case 2: alu(y, read_r(z)); break;  // ALU A, r[z]
        default: execute_block3(y, z, p, q); break;
    }
}

void Cpu::execute_block0(int y, int z, int p, int q) {
    switch (z) {
        case 0:
            switch (y) {
                case 0: break;  // NOP
                case 1: {       // LD (nn), SP
                    const std::uint16_t addr = read16();
                    bus_.write(addr, u8(r_.sp & 0xFF));
                    bus_.write(u16(addr + 1), u8(r_.sp >> 8));
                    break;
                }
                case 2: read8(); break;  // STOP: treated as a 2-byte NOP for now
                case 3: jr(true); break;  // JR e
                default: jr(condition(y - 4)); break;  // JR cc, e
            }
            break;
        case 1:
            if (q == 0) {  // LD rp, nn
                write_rp(p, read16());
            } else {  // ADD HL, rp
                add_hl(read_rp(p));
                bus_.tick();
            }
            break;
        case 2: {  // LD (BC)/(DE)/(HL+)/(HL-), A  and the reverse
            std::uint16_t addr = 0;
            switch (p) {
                case 0: addr = r_.bc(); break;
                case 1: addr = r_.de(); break;
                case 2: addr = r_.hl(); r_.set_hl(u16(addr + 1)); break;
                default: addr = r_.hl(); r_.set_hl(u16(addr - 1)); break;
            }
            if (q == 0) {
                bus_.write(addr, r_.a);
            } else {
                r_.a = bus_.read(addr);
            }
            break;
        }
        case 3: {  // INC rp / DEC rp (no flags)
            const std::uint16_t v = read_rp(p);
            write_rp(p, u16(q == 0 ? v + 1 : v - 1));
            bus_.tick();
            break;
        }
        case 4: write_r(y, inc8(read_r(y))); break;  // INC r
        case 5: write_r(y, dec8(read_r(y))); break;  // DEC r
        case 6: write_r(y, read8()); break;          // LD r, n
        default:
            switch (y) {
                case 0:  // RLCA, RRCA, RLA, RRA: like the CB versions, but Z is always 0
                case 1:
                case 2:
                case 3:
                    r_.a = rotate_shift(y, r_.a);
                    r_.set_flag(flag::kZ, false);
                    break;
                case 4: daa(); break;
                case 5:  // CPL
                    r_.a = u8(~r_.a);
                    r_.set_flag(flag::kN, true);
                    r_.set_flag(flag::kH, true);
                    break;
                case 6:  // SCF
                    r_.set_flag(flag::kN, false);
                    r_.set_flag(flag::kH, false);
                    r_.set_flag(flag::kC, true);
                    break;
                default:  // CCF
                    r_.set_flag(flag::kN, false);
                    r_.set_flag(flag::kH, false);
                    r_.set_flag(flag::kC, !r_.flag(flag::kC));
                    break;
            }
            break;
    }
}

void Cpu::execute_block3(int y, int z, int p, int q) {
    switch (z) {
        case 0:
            switch (y) {
                case 0:  // RET cc
                case 1:
                case 2:
                case 3:
                    bus_.tick();  // condition check
                    if (condition(y)) {
                        r_.pc = pop16();
                        bus_.tick();
                    }
                    break;
                case 4: bus_.write(u16(0xFF00 + read8()), r_.a); break;  // LDH (n), A
                case 5:                                                  // ADD SP, e
                    r_.sp = sp_plus_offset();
                    bus_.tick();
                    bus_.tick();
                    break;
                case 6: r_.a = bus_.read(u16(0xFF00 + read8())); break;  // LDH A, (n)
                default:                                                 // LD HL, SP+e
                    r_.set_hl(sp_plus_offset());
                    bus_.tick();
                    break;
            }
            break;
        case 1:
            if (q == 0) {  // POP rp2
                write_rp2(p, pop16());
                break;
            }
            switch (p) {
                case 0:  // RET
                    r_.pc = pop16();
                    bus_.tick();
                    break;
                case 1:  // RETI: enables interrupts immediately (no EI delay)
                    r_.pc = pop16();
                    bus_.tick();
                    ime_ = true;
                    break;
                case 2: r_.pc = r_.hl(); break;  // JP HL
                default:                         // LD SP, HL
                    r_.sp = r_.hl();
                    bus_.tick();
                    break;
            }
            break;
        case 2:
            switch (y) {
                case 4: bus_.write(u16(0xFF00 + r_.c), r_.a); break;  // LD (C), A
                case 5: bus_.write(read16(), r_.a); break;           // LD (nn), A
                case 6: r_.a = bus_.read(u16(0xFF00 + r_.c)); break;  // LD A, (C)
                case 7: r_.a = bus_.read(read16()); break;           // LD A, (nn)
                default: {                                           // JP cc, nn
                    const std::uint16_t addr = read16();
                    if (condition(y)) {
                        bus_.tick();
                        r_.pc = addr;
                    }
                    break;
                }
            }
            break;
        case 3:
            switch (y) {
                case 0: {  // JP nn
                    const std::uint16_t addr = read16();
                    bus_.tick();
                    r_.pc = addr;
                    break;
                }
                case 1: execute_cb(); break;
                case 6:  // DI
                    ime_ = false;
                    ime_pending_ = false;
                    break;
                case 7: ime_pending_ = true; break;  // EI
                default: locked_ = true; break;      // illegal opcode: the CPU hangs
            }
            break;
        case 4:
            if (y < 4) {  // CALL cc, nn
                const std::uint16_t addr = read16();
                if (condition(y)) call(addr);
            } else {
                locked_ = true;
            }
            break;
        case 5:
            if (q == 0) {  // PUSH rp2
                bus_.tick();
                push16(read_rp2(p));
            } else if (p == 0) {  // CALL nn
                call(read16());
            } else {
                locked_ = true;
            }
            break;
        case 6: alu(y, read8()); break;   // ALU A, n
        default: call(u16(y * 8)); break;  // RST y*8
    }
}

void Cpu::execute_cb() {
    const std::uint8_t opcode = read8();
    const int x = opcode >> 6;
    const int y = (opcode >> 3) & 7;
    const int z = opcode & 7;
    const std::uint8_t value = read_r(z);

    switch (x) {
        case 0: write_r(z, rotate_shift(y, value)); break;
        case 1:  // BIT y, r (only reads, even for (HL))
            r_.set_flag(flag::kZ, ((value >> y) & 1) == 0);
            r_.set_flag(flag::kN, false);
            r_.set_flag(flag::kH, true);
            break;
        case 2: write_r(z, u8(value & ~(1 << y))); break;  // RES y, r
        default: write_r(z, u8(value | (1 << y))); break;  // SET y, r
    }
}

// ---------------------------------------------------------------------------
// Operations

void Cpu::jr(bool taken) {
    const auto offset = static_cast<std::int8_t>(read8());
    if (taken) {
        bus_.tick();
        r_.pc = u16(r_.pc + offset);
    }
}

void Cpu::call(std::uint16_t addr) {
    bus_.tick();
    push16(r_.pc);
    r_.pc = addr;
}

void Cpu::alu(int op, std::uint8_t value) {
    switch (op) {
        case 0: add8(value, false); break;                     // ADD
        case 1: add8(value, r_.flag(flag::kC)); break;         // ADC
        case 2: r_.a = sub8(value, false); break;              // SUB
        case 3: r_.a = sub8(value, r_.flag(flag::kC)); break;  // SBC
        case 4:                                                // AND
            r_.a &= value;
            r_.f = u8((r_.a == 0 ? flag::kZ : 0) | flag::kH);
            break;
        case 5:  // XOR
            r_.a ^= value;
            r_.f = u8(r_.a == 0 ? flag::kZ : 0);
            break;
        case 6:  // OR
            r_.a |= value;
            r_.f = u8(r_.a == 0 ? flag::kZ : 0);
            break;
        default: sub8(value, false); break;  // CP: subtract, keep only the flags
    }
}

void Cpu::add8(std::uint8_t value, bool carry) {
    const int c = carry ? 1 : 0;
    const int result = r_.a + value + c;
    r_.set_flag(flag::kZ, u8(result) == 0);
    r_.set_flag(flag::kN, false);
    r_.set_flag(flag::kH, (r_.a & 0xF) + (value & 0xF) + c > 0xF);
    r_.set_flag(flag::kC, result > 0xFF);
    r_.a = u8(result);
}

std::uint8_t Cpu::sub8(std::uint8_t value, bool carry) {
    const int c = carry ? 1 : 0;
    const int result = r_.a - value - c;
    r_.set_flag(flag::kZ, u8(result) == 0);
    r_.set_flag(flag::kN, true);
    r_.set_flag(flag::kH, (r_.a & 0xF) - (value & 0xF) - c < 0);
    r_.set_flag(flag::kC, result < 0);
    return u8(result);
}

std::uint8_t Cpu::inc8(std::uint8_t value) {
    const std::uint8_t result = u8(value + 1);
    r_.set_flag(flag::kZ, result == 0);
    r_.set_flag(flag::kN, false);
    r_.set_flag(flag::kH, (value & 0xF) == 0xF);
    return result;
}

std::uint8_t Cpu::dec8(std::uint8_t value) {
    const std::uint8_t result = u8(value - 1);
    r_.set_flag(flag::kZ, result == 0);
    r_.set_flag(flag::kN, true);
    r_.set_flag(flag::kH, (value & 0xF) == 0);
    return result;
}

void Cpu::add_hl(std::uint16_t value) {
    const std::uint16_t hl = r_.hl();
    r_.set_flag(flag::kN, false);
    r_.set_flag(flag::kH, (hl & 0xFFF) + (value & 0xFFF) > 0xFFF);
    r_.set_flag(flag::kC, hl + value > 0xFFFF);
    r_.set_hl(u16(hl + value));
}

std::uint16_t Cpu::sp_plus_offset() {
    // Flags come from the unsigned low-byte addition, whatever the sign of the offset.
    const std::uint8_t raw = read8();
    const auto offset = static_cast<std::int8_t>(raw);
    r_.f = 0;
    r_.set_flag(flag::kH, (r_.sp & 0xF) + (raw & 0xF) > 0xF);
    r_.set_flag(flag::kC, (r_.sp & 0xFF) + raw > 0xFF);
    return u16(r_.sp + offset);
}

std::uint8_t Cpu::rotate_shift(int op, std::uint8_t value) {
    const int old_carry = r_.flag(flag::kC) ? 1 : 0;
    std::uint8_t result = 0;
    bool carry = false;
    switch (op) {
        case 0: result = u8((value << 1) | (value >> 7)); carry = (value & 0x80) != 0; break;  // RLC
        case 1: result = u8((value >> 1) | (value << 7)); carry = (value & 0x01) != 0; break;  // RRC
        case 2: result = u8((value << 1) | old_carry); carry = (value & 0x80) != 0; break;     // RL
        case 3: result = u8((value >> 1) | (old_carry << 7)); carry = (value & 0x01) != 0; break;  // RR
        case 4: result = u8(value << 1); carry = (value & 0x80) != 0; break;                   // SLA
        case 5: result = u8((value >> 1) | (value & 0x80)); carry = (value & 0x01) != 0; break;  // SRA
        case 6: result = u8((value << 4) | (value >> 4)); carry = false; break;                // SWAP
        default: result = u8(value >> 1); carry = (value & 0x01) != 0; break;                  // SRL
    }
    r_.f = 0;
    r_.set_flag(flag::kZ, result == 0);
    r_.set_flag(flag::kC, carry);
    return result;
}

void Cpu::daa() {
    // Adjusts A after a BCD addition/subtraction, using N, H and C from that operation.
    std::uint8_t a = r_.a;
    bool carry = r_.flag(flag::kC);
    if (!r_.flag(flag::kN)) {
        if (carry || a > 0x99) {
            a = u8(a + 0x60);
            carry = true;
        }
        if (r_.flag(flag::kH) || (a & 0x0F) > 0x09) a = u8(a + 0x06);
    } else {
        if (carry) a = u8(a - 0x60);
        if (r_.flag(flag::kH)) a = u8(a - 0x06);
    }
    r_.a = a;
    r_.set_flag(flag::kZ, a == 0);
    r_.set_flag(flag::kH, false);
    r_.set_flag(flag::kC, carry);
}

}  // namespace ezy::gb
