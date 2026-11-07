#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace ezy::n64 {

// The CIC lockout chip on the cartridge. It decides the seed the PIF hands to the IPL3
// (and, for the 6105, the challenge at the end of boot). 7101 is the PAL 6102.
enum class Cic : std::uint8_t { Cic6101, Cic6102, Cic6103, Cic6105, Cic6106, Cic7101 };

[[nodiscard]] std::uint8_t cic_seed(Cic cic);

enum class Region : std::uint8_t { Ntsc, Pal, Mpal };

// A cartridge ROM. Dumps come in three byte orders, told apart by the first word:
//   .z64  80 37 12 40  big-endian, as the N64 sees it
//   .v64  37 80 40 12  every 16-bit halfword byte-swapped
//   .n64  40 12 37 80  every 32-bit word reversed (little-endian)
// rom() is always big-endian.
class Cartridge {
public:
    // nullopt if the file is shorter than the header + IPL3 (0x1000 bytes), its first
    // word is none of the three, or its size doesn't fit its byte order.
    [[nodiscard]] static std::optional<Cartridge> load(std::span<const std::uint8_t> file);

    [[nodiscard]] std::span<const std::uint8_t> rom() const { return rom_; }
    [[nodiscard]] std::string title() const;  // header 0x20-0x33, trailing spaces removed
    [[nodiscard]] Cic cic() const { return cic_; }
    [[nodiscard]] Region region() const { return region_; }
    [[nodiscard]] std::uint32_t crc32() const { return crc32_; }  // of the whole ROM: identifies the game
    // Big-endian word at a ROM offset; 0 past the end.
    [[nodiscard]] std::uint32_t read32(std::uint32_t offset) const;

private:
    std::vector<std::uint8_t> rom_;  // padded with zeros to a multiple of 4 bytes
    Cic cic_ = Cic::Cic6102;
    Region region_ = Region::Ntsc;
    std::uint32_t crc32_ = 0;
};

}  // namespace ezy::n64
