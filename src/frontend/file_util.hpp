#pragma once

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <span>
#include <system_error>

namespace ezy::frontend {

// Writes to a temp file and renames it, so a crash mid-write never corrupts the old file.
inline bool write_file_atomically(const std::filesystem::path& path, std::span<const std::uint8_t> data) {
    std::filesystem::path tmp = path;
    tmp += ".tmp";
    {
        std::ofstream file(tmp, std::ios::binary | std::ios::trunc);
        file.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
        if (!file) {
            std::cerr << "Could not write " << tmp.string() << "\n";
            return false;
        }
    }
    std::error_code ec;
    std::filesystem::rename(tmp, path, ec);
    if (ec) {
        std::cerr << "Could not replace " << path.string() << ": " << ec.message() << "\n";
        return false;
    }
    return true;
}

// The save state slot that goes with a .sav file: <rom>.ss1.
inline std::filesystem::path state_path(const std::filesystem::path& save_path) {
    return std::filesystem::path(save_path).replace_extension(".ss1");
}

}  // namespace ezy::frontend
