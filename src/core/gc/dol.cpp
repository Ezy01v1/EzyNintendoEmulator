#include "gc/dol.hpp"

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

void Dol::load(std::span<const std::uint8_t> bytes, CpuBus& bus) const {
    const std::uint32_t bss = physical(bss_address);
    for (std::uint32_t i = 0; i < bss_size; ++i) bus.write8(bss + i, 0);
    for (const DolSection& section : sections) {
        const std::uint32_t target = physical(section.address);
        for (std::uint32_t i = 0; i < section.size; ++i) bus.write8(target + i, bytes[section.offset + i]);
    }
}

}  // namespace ezy::gc
