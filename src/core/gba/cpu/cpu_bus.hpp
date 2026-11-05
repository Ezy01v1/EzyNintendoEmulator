#pragma once

#include <cstdint>

namespace ezy::gba {

// How the CPU accesses memory. Waitstates depend on it: sequential accesses (the next
// address after the previous one) are cheaper than non-sequential ones, and opcode fetches
// go through the prefetch buffer. The bit values match NanoBoyAdvance's (and therefore the
// SingleStepTests' "access" field).
enum Access : std::uint32_t {
    kNonseq = 0,
    kSeq = 1,
    kCode = 2,
    kDma = 4,
    kLock = 8,  // SWP: read and write happen as one locked bus transaction
};

// What the ARM7TDMI sees of the rest of the system. The real Bus implements it; tests use
// a flat memory or replay recorded transactions. Addresses are passed exactly as the CPU
// computes them, possibly misaligned: the bus ignores the low bits of 16/32-bit accesses,
// and the CPU applies any rotation the instruction defines (e.g. misaligned LDR).
class CpuBus {
public:
    virtual ~CpuBus() = default;

    virtual std::uint8_t read8(std::uint32_t addr, Access access) = 0;
    virtual std::uint16_t read16(std::uint32_t addr, Access access) = 0;
    virtual std::uint32_t read32(std::uint32_t addr, Access access) = 0;
    virtual void write8(std::uint32_t addr, std::uint8_t value, Access access) = 0;
    virtual void write16(std::uint32_t addr, std::uint16_t value, Access access) = 0;
    virtual void write32(std::uint32_t addr, std::uint32_t value, Access access) = 0;
    virtual void idle() {}  // an internal cycle with no bus access (e.g. after a load)
};

}  // namespace ezy::gba
