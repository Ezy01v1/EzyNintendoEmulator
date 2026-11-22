#include "gc/memory.hpp"

#include <cstdio>
#include <string>

#include "gc/gekko/gekko.hpp"

namespace ezy::gc {

namespace {

std::string hex32(std::uint32_t value) {
    char text[11];
    std::snprintf(text, sizeof(text), "0x%08X", value);
    return text;
}

constexpr const char* kBlockNames[kMmioBlockCount] = {"CP", "PE", "VI", "PI", "MI", "DSP", "DI", "SI", "EXI", "AI"};

std::uint64_t load_be(const std::uint8_t* p, int bytes) {
    std::uint64_t value = 0;
    for (int i = 0; i < bytes; ++i) value = (value << 8) | p[i];
    return value;
}

void store_be(std::uint8_t* p, std::uint64_t value, int bytes) {
    for (int i = bytes - 1; i >= 0; --i) {
        p[i] = static_cast<std::uint8_t>(value);
        value >>= 8;
    }
}

}  // namespace

Memory::Memory() : mem1_(kMem1Size, 0), locked_cache_(kLockedCacheSize, 0) {}

std::uint8_t* Memory::ram(std::uint32_t paddr, std::uint32_t bytes) {
    if (paddr < kMem1Size && kMem1Size - paddr >= bytes) return mem1_.data() + paddr;
    if (paddr - kLockedCacheBase < kLockedCacheSize && kLockedCacheBase + kLockedCacheSize - paddr >= bytes)
        return locked_cache_.data() + (paddr - kLockedCacheBase);
    return nullptr;
}

MmioHandler& Memory::mmio(std::uint32_t paddr, std::uint32_t& offset) {
    // 0x0C000000-0x0C005FFF: one 4 KiB page per block; 0x0C006000-0x0C006FFF: DI, SI, EXI, AI
    // in 1 KiB each.
    const std::uint32_t rel = paddr - kMmioBase;
    const std::size_t block = rel < 0x6000 ? rel >> 12 : 6 + ((rel >> 10) & 3);
    offset = paddr - mmio_block_base(static_cast<MmioBlock>(block));
    MmioHandler* handler = mmio_[block];
    if (handler == nullptr)
        throw BusError(std::string("no handler for the ") + kBlockNames[block] + " registers at " + hex32(paddr), paddr);
    return *handler;
}

std::uint64_t Memory::read_io(std::uint32_t paddr, int bytes) {
    if (paddr >= kMmioBase && paddr < kMmioEnd) {
        if (bytes == 2 || bytes == 4) {
            std::uint32_t offset = 0;
            MmioHandler& handler = mmio(paddr, offset);
            return bytes == 2 ? handler.read16(offset) : handler.read32(offset);
        }
        throw BusError(std::to_string(bytes * 8) + "-bit read of the hardware register at " + hex32(paddr), paddr);
    }
    if (paddr - kPipeBase < kPipeSize) throw BusError("read of the write-gather pipe at " + hex32(paddr), paddr);
    throw BusError(std::to_string(bytes * 8) + "-bit read of the unmapped physical address " + hex32(paddr), paddr);
}

void Memory::write_io(std::uint32_t paddr, std::uint64_t value, int bytes) {
    if (paddr >= kMmioBase && paddr < kMmioEnd) {
        if (bytes == 2 || bytes == 4) {
            std::uint32_t offset = 0;
            MmioHandler& handler = mmio(paddr, offset);
            if (bytes == 2) handler.write16(offset, static_cast<std::uint16_t>(value));
            else handler.write32(offset, static_cast<std::uint32_t>(value));
            return;
        }
        throw BusError(std::to_string(bytes * 8) + "-bit write of the hardware register at " + hex32(paddr), paddr);
    }
    if (paddr - kPipeBase < kPipeSize) {
        if (pipe_ == nullptr) throw BusError("write to the write-gather pipe with no consumer at " + hex32(paddr), paddr);
        std::uint8_t data[8];
        store_be(data, value, bytes);
        pipe_->write(std::span<const std::uint8_t>(data, static_cast<std::size_t>(bytes)));
        return;
    }
    throw BusError(std::to_string(bytes * 8) + "-bit write of the unmapped physical address " + hex32(paddr), paddr);
}

std::uint8_t Memory::read8(std::uint32_t paddr) {
    if (const std::uint8_t* p = ram(paddr, 1)) return *p;
    return static_cast<std::uint8_t>(read_io(paddr, 1));
}
std::uint16_t Memory::read16(std::uint32_t paddr) {
    if (const std::uint8_t* p = ram(paddr, 2)) return static_cast<std::uint16_t>(load_be(p, 2));
    return static_cast<std::uint16_t>(read_io(paddr, 2));
}
std::uint32_t Memory::read32(std::uint32_t paddr) {
    if (const std::uint8_t* p = ram(paddr, 4)) return static_cast<std::uint32_t>(load_be(p, 4));
    return static_cast<std::uint32_t>(read_io(paddr, 4));
}
std::uint64_t Memory::read64(std::uint32_t paddr) {
    if (const std::uint8_t* p = ram(paddr, 8)) return load_be(p, 8);
    return read_io(paddr, 8);
}
void Memory::write8(std::uint32_t paddr, std::uint8_t value) {
    if (std::uint8_t* p = ram(paddr, 1)) *p = value;
    else write_io(paddr, value, 1);
}
void Memory::write16(std::uint32_t paddr, std::uint16_t value) {
    if (std::uint8_t* p = ram(paddr, 2)) store_be(p, value, 2);
    else write_io(paddr, value, 2);
}
void Memory::write32(std::uint32_t paddr, std::uint32_t value) {
    if (std::uint8_t* p = ram(paddr, 4)) store_be(p, value, 4);
    else write_io(paddr, value, 4);
}
void Memory::write64(std::uint32_t paddr, std::uint64_t value) {
    if (std::uint8_t* p = ram(paddr, 8)) store_be(p, value, 8);
    else write_io(paddr, value, 8);
}

}  // namespace ezy::gc
