#include "gc/mi.hpp"

#include <cstdio>
#include <string>

#include "gc/gekko/gekko.hpp"
#include "gc/memory.hpp"

namespace ezy::gc {

namespace {
constexpr std::uint32_t kBase = Memory::mmio_block_base(MmioBlock::Mi);
constexpr std::uint16_t kMaskBits = 0x1F;

[[noreturn]] void unmodeled(const char* what, std::uint32_t offset) {
    char b[16];
    std::snprintf(b, sizeof b, "%X", offset);
    throw BusError(std::string(what) + " of the unmodeled memory interface register at offset 0x" + b,
                   kBase + offset);
}
bool store_only(std::uint32_t offset) { return offset <= 0x10 || offset == 0x20 || offset == 0x22 || offset == 0x24; }
}  // namespace

void Mi::raise(unsigned region) {
    cause_ |= static_cast<std::uint16_t>((1u << region) | 0x10u);
    update_irq();
}

void Mi::update_irq() {
    const bool now = (cause_ & regs_[0x1C / 2] & kMaskBits) != 0;
    irq_ = now;
    if (sink_) sink_(now);  // by level, like every other peripheral: the PI may have dropped bit 7
}

std::uint16_t Mi::read16(std::uint32_t offset) {
    if (offset & 1) unmodeled("misaligned read", offset);
    if (store_only(offset) || offset == 0x1C) return regs_[offset / 2];
    if (offset == 0x1E) return cause_;
    unmodeled("read", offset);
}

std::uint32_t Mi::read32(std::uint32_t offset) {
    return (std::uint32_t{read16(offset)} << 16) | read16(offset + 2);
}

void Mi::write16(std::uint32_t offset, std::uint16_t value) {
    if (offset & 1) unmodeled("misaligned write", offset);
    if (store_only(offset)) {
        regs_[offset / 2] = value;  // accepted, no effect: protection is not enforced
    } else if (offset == 0x1C) {
        regs_[offset / 2] = value & kMaskBits;
        update_irq();
    } else if (offset == 0x1E) {
        cause_ &= static_cast<std::uint16_t>(~value);
        update_irq();
    } else {
        unmodeled("write", offset);
    }
}

void Mi::write32(std::uint32_t offset, std::uint32_t value) {
    // All or nothing: a half that would throw must not leave the other one applied.
    if (offset & 1) unmodeled("misaligned write", offset);
    for (const std::uint32_t half : {offset, offset + 2})
        if (!store_only(half) && half != 0x1C && half != 0x1E) unmodeled("write", half);
    write16(offset, static_cast<std::uint16_t>(value >> 16));
    write16(offset + 2, static_cast<std::uint16_t>(value));
}

}  // namespace ezy::gc
