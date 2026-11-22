#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <span>
#include <string>

namespace ezy::gc {

// A GameCube disc image in GCM layout (a plain .iso/.gcm, or an NKit ISO, which keeps the
// layout but drops the padding: design, "Disco"). Read by streaming from the file, never
// loaded whole. The header fields (YAGCD, "DVD"; Dolphin's DiscIO): game id at 0x000, the
// GameCube magic 0xC2339F3D at 0x01C, the DOL offset at 0x420, the FST offset, size and
// maximum size at 0x424/0x428/0x42C, BI2 (0x2000 bytes) at 0x440.
class Disc {
public:
    static constexpr std::uint32_t kMagic = 0xC2339F3D;
    static constexpr std::uint32_t kBi2Offset = 0x440;
    static constexpr std::uint32_t kBi2Size = 0x2000;
    // The header through BI2: what open() reads and checks.
    static constexpr std::uint32_t kHeaderSize = kBi2Offset + kBi2Size;

    // nullptr (with the reason on stderr) when the file cannot be read, is too short for the
    // header, lacks the magic, or has a DOL header or FST that falls outside the file. The
    // full-size check of a retail image (1,459,978,240 bytes) is deliberately not made, so
    // NKit images open.
    static std::unique_ptr<Disc> open(const std::filesystem::path& path);

    // Reads out.size() bytes at `offset`. False (out unspecified) when the range does not fit
    // in the file or the read fails; the reader stays usable.
    bool read(std::uint64_t offset, std::span<std::uint8_t> out);

    [[nodiscard]] std::string game_id() const;  // 6 characters, e.g. "GM4E01"
    [[nodiscard]] std::uint32_t dol_offset() const { return dol_offset_; }
    [[nodiscard]] std::uint32_t fst_offset() const { return fst_offset_; }
    [[nodiscard]] std::uint32_t fst_size() const { return fst_size_; }
    [[nodiscard]] std::uint32_t fst_max_size() const { return fst_max_size_; }
    [[nodiscard]] std::uint64_t size() const { return size_; }
    // The header and BI2 (kHeaderSize bytes from offset 0), as read by open().
    [[nodiscard]] std::span<const std::uint8_t> header() const { return header_; }

private:
    Disc() = default;

    std::ifstream file_;
    std::uint64_t size_ = 0;
    std::array<std::uint8_t, kHeaderSize> header_{};
    std::uint32_t dol_offset_ = 0, fst_offset_ = 0, fst_size_ = 0, fst_max_size_ = 0;
};

}  // namespace ezy::gc
