#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <vector>

#include "gc/gekko/cpu_bus.hpp"

namespace ezy::gc {

// One loadable section of a DOL: `size` bytes at file `offset`, loaded at virtual `address`.
struct DolSection {
    std::uint32_t offset = 0;
    std::uint32_t address = 0;
    std::uint32_t size = 0;
    bool text = false;  // a text (code) section; else data
};

// The GameCube executable format (YAGCD, "DOL"): a 0x100-byte big-endian header with the
// file offsets (0x00: 7 text, 0x1C: 11 data), load addresses (0x48, 0x64) and sizes (0x90,
// 0xAC) of 18 sections, then the BSS address (0xD8) and size (0xDC) and the entry (0xE0).
struct Dol {
    static constexpr std::uint32_t kHeaderSize = 0x100;
    static constexpr int kTextCount = 7;
    static constexpr int kDataCount = 11;

    // The sections with a nonzero size, text first, in header order. Empty when `bytes` is
    // shorter than the header, or a section starts inside the header or runs past the end.
    static std::optional<Dol> parse(std::span<const std::uint8_t> bytes);
    // How many bytes of the file the header's sections need (the end of the last one), so a
    // reader knows how much to read after the header. Empty when `header` is too short.
    static std::optional<std::uint32_t> file_size(std::span<const std::uint8_t> header);

    // Clears the BSS range, then copies every section, into `bus` at physical address =
    // virtual & 0x3FFFFFFF (the boot BATs' view: 0x80000000 cached and 0xC0000000 uncached
    // both map physical 0; MEM1 is never past 0x3FFFFFFF). The BSS goes first because
    // linkers put small-data sections (.sdata, .sdata2) inside the BSS range, as MKDD's DOL
    // does; the crt0 clears only the real BSS parts itself. A target outside the bus's memory
    // throws BusError. `bytes` is the image parse() was given.
    void load(std::span<const std::uint8_t> bytes, CpuBus& bus) const;

    std::uint32_t entry = 0;
    std::vector<DolSection> sections;
    std::uint32_t bss_address = 0;
    std::uint32_t bss_size = 0;
};

}  // namespace ezy::gc
