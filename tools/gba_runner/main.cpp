// Headless GBA runner.
//
//   ezy_gba_runner <rom.gba> --screenshot <out.bmp> [frames] [--bios <bios.bin>]
//       Runs `frames` frames (default 60) and saves the screen as a 24-bit BMP. Test ROMs
//       such as jsmolka/gba-tests show their result on screen.
//
// The BIOS is optional; without it the cartridge starts in the state the BIOS leaves.

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <iterator>
#include <span>
#include <string>
#include <vector>

#include "gba/gba.hpp"

namespace {

std::vector<std::uint8_t> read_file(const char* path) {
    std::ifstream file(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(file), {}};
}

void put_u16(std::ofstream& out, std::uint16_t v) { out.write(reinterpret_cast<const char*>(&v), 2); }
void put_u32(std::ofstream& out, std::uint32_t v) { out.write(reinterpret_cast<const char*>(&v), 4); }

std::uint8_t expand5(std::uint16_t c5) { return static_cast<std::uint8_t>((c5 << 3) | (c5 >> 2)); }

// 24-bit uncompressed BMP from BGR555 pixels (bottom-up rows, padded to 4 bytes).
bool write_bmp(const char* path, std::span<const std::uint16_t> pixels, int width, int height) {
    std::ofstream out(path, std::ios::binary);
    if (!out) return false;
    const int row_size = (width * 3 + 3) & ~3;
    const auto data_size = static_cast<std::uint32_t>(row_size * height);
    out.write("BM", 2);
    put_u32(out, 54 + data_size);
    put_u32(out, 0);
    put_u32(out, 54);
    put_u32(out, 40);
    put_u32(out, static_cast<std::uint32_t>(width));
    put_u32(out, static_cast<std::uint32_t>(height));
    put_u16(out, 1);
    put_u16(out, 24);
    put_u32(out, 0);
    put_u32(out, data_size);
    put_u32(out, 2835);
    put_u32(out, 2835);
    put_u32(out, 0);
    put_u32(out, 0);
    std::vector<char> row(static_cast<std::size_t>(row_size), 0);
    for (int y = height - 1; y >= 0; --y) {
        for (int x = 0; x < width; ++x) {
            const std::uint16_t c = pixels[static_cast<std::size_t>(y * width + x)];
            const auto i = static_cast<std::size_t>(x * 3);
            row[i] = static_cast<char>(expand5((c >> 10) & 0x1F));     // B
            row[i + 1] = static_cast<char>(expand5((c >> 5) & 0x1F));  // G
            row[i + 2] = static_cast<char>(expand5(c & 0x1F));         // R
        }
        out.write(row.data(), row_size);
    }
    return static_cast<bool>(out);
}

}  // namespace

int main(int argc, char* argv[]) {
    if (argc < 4 || std::strcmp(argv[2], "--screenshot") != 0) {
        std::cerr << "Usage: ezy_gba_runner <rom.gba> --screenshot <out.bmp> [frames] [--bios <bios.bin>]\n";
        return 2;
    }
    const auto rom = read_file(argv[1]);
    int frames = 60;
    std::vector<std::uint8_t> bios;
    for (int i = 4; i < argc; ++i) {
        if (std::strcmp(argv[i], "--bios") == 0 && i + 1 < argc) {
            bios = read_file(argv[++i]);
        } else {
            frames = std::atoi(argv[i]);
        }
    }
    auto gba = ezy::gba::Gba::create(rom, bios);
    if (!gba) {
        std::cerr << "Not a valid GBA ROM: " << argv[1] << '\n';
        return 2;
    }
    std::cout << "Title: " << gba->header().title << " (" << gba->header().game_code << ")\n";
    for (int i = 0; i < frames; ++i) gba->run_frame();
    if (!write_bmp(argv[3], gba->bus().ppu().framebuffer(), ezy::gba::kScreenWidth, ezy::gba::kScreenHeight)) {
        std::cerr << "Could not write " << argv[3] << '\n';
        return 2;
    }
    const auto& cpu = gba->cpu();
    std::cout << "Saved " << argv[3] << " after " << frames << " frames. PC=" << std::hex << cpu.reg(15)
              << (cpu.thumb() ? " (Thumb)" : " (ARM)") << '\n';
    return 0;
}
