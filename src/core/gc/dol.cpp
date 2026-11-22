#include "gc/dol.hpp"

#include <cstdio>
#include <stdexcept>

namespace ezy::gc {

namespace {

std::uint32_t be32(std::span<const std::uint8_t> bytes, std::size_t at) {
    return (static_cast<std::uint32_t>(bytes[at]) << 24) | (static_cast<std::uint32_t>(bytes[at + 1]) << 16) |
           (static_cast<std::uint32_t>(bytes[at + 2]) << 8) | bytes[at + 3];
}

constexpr std::uint32_t kOffsets = 0x00, kAddresses = 0x48, kSizes = 0x90;
constexpr std::uint32_t kBssAddress = 0xD8, kBssSize = 0xDC, kEntry = 0xE0;
constexpr int kSectionCount = Dol::kTextCount + Dol::kDataCount;

constexpr std::uint32_t physical(std::uint32_t address) { return address & 0x3FFFFFFF; }

}  // namespace

std::optional<std::uint32_t> Dol::file_size(std::span<const std::uint8_t> header) {
    if (header.size() < kHeaderSize) return std::nullopt;
    std::uint64_t end = kHeaderSize;
    for (int i = 0; i < kSectionCount; ++i) {
        const auto at = static_cast<std::size_t>(i) * 4;
        const std::uint32_t size = be32(header, kSizes + at);
        if (size == 0) continue;
        const std::uint64_t section_end = std::uint64_t{be32(header, kOffsets + at)} + size;
        if (section_end > end) end = section_end;
    }
    if (end > 0xFFFFFFFFu) return std::nullopt;
    return static_cast<std::uint32_t>(end);
}

std::optional<Dol> Dol::parse(std::span<const std::uint8_t> bytes) {
    if (bytes.size() < kHeaderSize) return std::nullopt;
    Dol dol;
    for (int i = 0; i < kSectionCount; ++i) {
        const auto at = static_cast<std::size_t>(i) * 4;
        DolSection section{be32(bytes, kOffsets + at), be32(bytes, kAddresses + at), be32(bytes, kSizes + at),
                           i < kTextCount};
        if (section.size == 0) continue;
        if (section.offset < kHeaderSize || std::uint64_t{section.offset} + section.size > bytes.size())
            return std::nullopt;
        dol.sections.push_back(section);
    }
    dol.bss_address = be32(bytes, kBssAddress);
    dol.bss_size = be32(bytes, kBssSize);
    dol.entry = be32(bytes, kEntry);
    return dol;
}

void Dol::load(std::span<const std::uint8_t> bytes, CpuBus& bus, std::uint32_t memory_end) const {
    const auto check = [memory_end](const char* what, std::uint32_t address, std::uint32_t size) {
        if (size == 0) return;
        if ((address & 0x80000000) == 0 || std::uint64_t{physical(address)} + size > memory_end) {
            char text[128];
            std::snprintf(text, sizeof(text), "DOL %s at 0x%08X (0x%X bytes) is outside memory (ends at 0x%08X)", what,
                          address, size, memory_end);
            throw std::runtime_error(text);
        }
    };
    check("BSS", bss_address, bss_size);
    for (const DolSection& section : sections) check(section.text ? "text section" : "data section", section.address, section.size);

    const std::uint32_t bss = physical(bss_address);
    for (std::uint32_t i = 0; i < bss_size; ++i) bus.write8(bss + i, 0);
    for (const DolSection& section : sections) {
        const std::uint32_t target = physical(section.address);
        for (std::uint32_t i = 0; i < section.size; ++i) bus.write8(target + i, bytes[section.offset + i]);
    }
}

}  // namespace ezy::gc
