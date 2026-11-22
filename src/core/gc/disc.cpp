#include "gc/disc.hpp"

#include <iostream>
#include <system_error>

#include "gc/dol.hpp"

namespace ezy::gc {

namespace {

std::uint32_t be32(std::span<const std::uint8_t> bytes, std::size_t at) {
    return (static_cast<std::uint32_t>(bytes[at]) << 24) | (static_cast<std::uint32_t>(bytes[at + 1]) << 16) |
           (static_cast<std::uint32_t>(bytes[at + 2]) << 8) | bytes[at + 3];
}

bool fits(std::uint64_t offset, std::uint64_t length, std::uint64_t size) {
    return offset <= size && length <= size - offset;
}

}  // namespace

std::unique_ptr<Disc> Disc::open(const std::filesystem::path& path) {
    const auto reject = [&path](const std::string& why) {
        std::cerr << "GameCube disc " << path.string() << ": " << why << "\n";
        return std::unique_ptr<Disc>{};
    };
    std::error_code ec;
    const std::uintmax_t size = std::filesystem::file_size(path, ec);
    if (ec) return reject("cannot read the file (" + ec.message() + ")");
    if (size < kHeaderSize) return reject("too short for a disc header");

    std::unique_ptr<Disc> disc(new Disc());
    disc->file_.open(path, std::ios::binary);
    if (!disc->file_) return reject("cannot open the file");
    disc->size_ = size;
    if (!disc->read(0, disc->header_)) return reject("cannot read the header");

    const std::span<const std::uint8_t> h = disc->header_;
    if (be32(h, 0x1C) != kMagic) return reject("no GameCube magic 0xC2339F3D at 0x1C (not a GCM image)");
    disc->dol_offset_ = be32(h, 0x420);
    disc->fst_offset_ = be32(h, 0x424);
    disc->fst_size_ = be32(h, 0x428);
    disc->fst_max_size_ = be32(h, 0x42C);
    if (!fits(disc->dol_offset_, Dol::kHeaderSize, size)) return reject("the DOL header is outside the file");
    if (!fits(disc->fst_offset_, disc->fst_size_, size)) return reject("the FST is outside the file");
    return disc;
}

bool Disc::read(std::uint64_t offset, std::span<std::uint8_t> out) {
    if (!fits(offset, out.size(), size_)) return false;
    file_.clear();
    file_.seekg(static_cast<std::streamoff>(offset));
    file_.read(reinterpret_cast<char*>(out.data()), static_cast<std::streamsize>(out.size()));
    if (!file_) {
        file_.clear();
        return false;
    }
    return true;
}

std::string Disc::game_id() const { return std::string(header_.begin(), header_.begin() + 6); }

}  // namespace ezy::gc
