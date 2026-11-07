#include "n64/cartridge.hpp"

#include <array>
#include <cstdio>

namespace ezy::n64 {

namespace {

constexpr std::size_t kHeaderAndIpl3 = 0x1000;

// CRC-32 (zlib polynomial 0xEDB88320, reflected).
std::uint32_t crc32(std::span<const std::uint8_t> data) {
    static const auto table = [] {
        std::array<std::uint32_t, 256> t{};
        for (std::uint32_t i = 0; i < 256; ++i) {
            std::uint32_t c = i;
            for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320U ^ (c >> 1) : c >> 1;
            t[i] = c;
        }
        return t;
    }();
    std::uint32_t crc = 0xFFFFFFFFU;
    for (const std::uint8_t byte : data) crc = table[(crc ^ byte) & 0xFF] ^ (crc >> 8);
    return crc ^ 0xFFFFFFFFU;
}

// Each CIC pairs with its own IPL3 (ROM 0x40-0xFFF), so a CRC of the IPL3 identifies it.
Cic detect_cic(std::span<const std::uint8_t> rom, Region region) {
    switch (crc32(rom.subspan(0x40, kHeaderAndIpl3 - 0x40))) {
        case 0x6170A4A1: return Cic::Cic6101;
        case 0x90BB6CB5: return region == Region::Pal ? Cic::Cic7101 : Cic::Cic6102;
        case 0x0B050EE0: return Cic::Cic6103;
        case 0x98BC2C86: return Cic::Cic6105;
        case 0xACC8580A: return Cic::Cic6106;
        default:
            std::fprintf(stderr, "N64: unknown IPL3, assuming CIC 6102\n");
            return region == Region::Pal ? Cic::Cic7101 : Cic::Cic6102;
    }
}

// Header byte 0x3E, the destination code.
Region region_from_code(std::uint8_t code) {
    switch (code) {
        case 'D': case 'F': case 'I': case 'P': case 'S': case 'U': case 'X': case 'Y': return Region::Pal;
        case 'B': return Region::Mpal;
        default: return Region::Ntsc;
    }
}

}  // namespace

std::uint8_t cic_seed(Cic cic) {
    switch (cic) {
        case Cic::Cic6103: return 0x78;
        case Cic::Cic6105: return 0x91;
        case Cic::Cic6106: return 0x85;
        default: return 0x3F;  // 6101, 6102, 7101
    }
}

std::optional<Cartridge> Cartridge::load(std::span<const std::uint8_t> file) {
    if (file.size() < kHeaderAndIpl3) return std::nullopt;
    Cartridge cart;
    cart.rom_.assign(file.begin(), file.end());
    std::vector<std::uint8_t>& rom = cart.rom_;
    const std::uint32_t magic = (rom[0] << 24) | (rom[1] << 16) | (rom[2] << 8) | rom[3];
    switch (magic) {
        case 0x80371240: break;
        case 0x37804012:
            if (rom.size() % 2 != 0) return std::nullopt;
            for (std::size_t i = 0; i < rom.size(); i += 2) std::swap(rom[i], rom[i + 1]);
            break;
        case 0x40123780:
            if (rom.size() % 4 != 0) return std::nullopt;
            for (std::size_t i = 0; i < rom.size(); i += 4) {
                std::swap(rom[i], rom[i + 3]);
                std::swap(rom[i + 1], rom[i + 2]);
            }
            break;
        default: return std::nullopt;
    }
    rom.resize((rom.size() + 3) & ~std::size_t{3}, 0);
    cart.region_ = region_from_code(rom[0x3E]);
    cart.cic_ = detect_cic(rom, cart.region_);
    return cart;
}

std::string Cartridge::title() const {
    std::string title(reinterpret_cast<const char*>(&rom_[0x20]), 20);
    while (!title.empty() && (title.back() == ' ' || title.back() == '\0')) title.pop_back();
    return title;
}

std::uint32_t Cartridge::read32(std::uint32_t offset) const {
    if (offset > rom_.size() - 4) return 0;
    return (rom_[offset] << 24) | (rom_[offset + 1] << 16) | (rom_[offset + 2] << 8) | rom_[offset + 3];
}

}  // namespace ezy::n64
