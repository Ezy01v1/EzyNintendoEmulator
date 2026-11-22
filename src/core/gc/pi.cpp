#include "gc/pi.hpp"

#include <cstdio>
#include <stdexcept>
#include <string>

#include "gc/gekko/gekko.hpp"
#include "gc/memory.hpp"

namespace ezy::gc {

namespace {
constexpr std::uint32_t kBase = Memory::mmio_block_base(MmioBlock::Pi);

[[noreturn]] void unmodeled(const char* what, std::uint32_t offset) {
    char b[16];
    std::snprintf(b, sizeof b, "%X", offset);
    throw BusError(std::string(what) + " of the unmodeled processor interface register at offset 0x" + b,
                   kBase + offset);
}
}  // namespace

void Pi::set_cause(unsigned bit, bool level) {
    if (bit > kHsp) throw std::invalid_argument("PI cause bit out of range");
    const std::uint32_t m = 1u << bit;
    if (level && !(cause_ & m)) ++counts_[bit];
    cause_ = level ? (cause_ | m) : (cause_ & ~m);
    update_line();
}

void Pi::update_line() {
    const bool now = (cause_ & mask_) != 0;
    if (now == line_) return;
    line_ = now;
    if (sink_) sink_(now);
}

std::uint32_t Pi::read32(std::uint32_t offset) {
    switch (offset) {
        case 0x00: return cause();
        case 0x04: return mask_;
        case 0x0C: return fifo_base_;
        case 0x10: return fifo_end_;
        case 0x14: return fifo_wptr_;
        case 0x18: return 0;  // the FIFO reset register reads 0 (accepted, no effect)
        case 0x24: return reset_code_;
        case 0x2C: return kRevision;
        default: unmodeled("read", offset);
    }
}

void Pi::write32(std::uint32_t offset, std::uint32_t value) {
    switch (offset) {
        case 0x00:
            cause_ &= ~value;
            update_line();
            return;
        case 0x04:
            mask_ = value & kMaskableBits;
            update_line();
            return;
        case 0x0C: fifo_base_ = value & kFifoMask; return;
        case 0x10: fifo_end_ = value & kFifoMask; return;
        case 0x14: fifo_wptr_ = value & kFifoMask; return;
        case 0x18: return;  // FIFO reset: accepted, no effect
        case 0x24: reset_code_ = value; return;
        default: unmodeled("write", offset);  // includes the read-only revision at 0x2C
    }
}

std::uint16_t Pi::read16(std::uint32_t offset) {
    const std::uint32_t v = read32(offset & ~3u);
    return static_cast<std::uint16_t>((offset & 2) ? v : v >> 16);
}

void Pi::write16(std::uint32_t offset, std::uint16_t) {
    unmodeled("16-bit write (invalid on the hardware)", offset);
}

}  // namespace ezy::gc
