// Headless Game Boy runner.
//
//   ezy_gb_runner <rom.gb> [max-seconds]
//       Runs the ROM and prints everything sent over the serial port. Blargg's test ROMs
//       report "Passed" / "Failed" there. Exit code 0 on "Passed".
//
//   ezy_gb_runner <rom.gb> --screenshot <out.bmp> [frames]
//       Runs `frames` frames (default 120) and saves the screen as a BMP, e.g. to compare
//       dmg-acid2 against its reference image.

#include <array>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <iterator>
#include <span>
#include <string>
#include <vector>

#include "gb/gameboy.hpp"

namespace {

// The 4 DMG shades as plain greys (what dmg-acid2's reference image uses).
constexpr std::array<std::uint8_t, 4> kGrey = {0xFF, 0xAA, 0x55, 0x00};

void put_u16(std::ofstream& out, std::uint16_t v) { out.write(reinterpret_cast<const char*>(&v), 2); }
void put_u32(std::ofstream& out, std::uint32_t v) { out.write(reinterpret_cast<const char*>(&v), 4); }

// Minimal 24-bit uncompressed BMP (bottom-up rows, each padded to 4 bytes).
bool write_bmp(const char* path, std::span<const std::uint8_t> shades, int width, int height) {
    std::ofstream out(path, std::ios::binary);
    if (!out) return false;
    const int row_size = (width * 3 + 3) & ~3;
    const auto data_size = static_cast<std::uint32_t>(row_size * height);
    out.write("BM", 2);
    put_u32(out, 54 + data_size);
    put_u32(out, 0);
    put_u32(out, 54);
    put_u32(out, 40);  // BITMAPINFOHEADER
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
            const auto grey = static_cast<char>(kGrey[shades[static_cast<std::size_t>(y * width + x)] & 3]);
            row[static_cast<std::size_t>(x * 3)] = row[static_cast<std::size_t>(x * 3 + 1)] =
                row[static_cast<std::size_t>(x * 3 + 2)] = grey;
        }
        out.write(row.data(), row_size);
    }
    return static_cast<bool>(out);
}

int run_serial(ezy::gb::GameBoy& gb, double max_seconds) {
    constexpr std::uint64_t kClockHz = 4'194'304;  // T-cycles per emulated second
    const auto max_cycles = static_cast<std::uint64_t>(max_seconds * kClockHz);
    const auto& bus = gb.bus();

    std::size_t printed = 0;
    while (bus.t_cycles() < max_cycles && !gb.cpu().locked()) {
        gb.step();
        const std::string& out = bus.serial_output();
        if (out.size() > printed) {
            std::cout << out.substr(printed) << std::flush;
            printed = out.size();
            if (out.find("Passed") != std::string::npos || out.find("Failed") != std::string::npos) break;
        }
    }

    std::cout << "\n[" << static_cast<double>(bus.t_cycles()) / kClockHz << " emulated s]\n";
    if (gb.cpu().locked()) std::cout << "CPU locked at illegal opcode, PC=" << std::hex << gb.cpu().regs().pc << '\n';
    return bus.serial_output().find("Passed") != std::string::npos ? 0 : 1;
}

}  // namespace

int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::cerr << "Usage: ezy_gb_runner <rom.gb> [max-seconds]\n"
                     "       ezy_gb_runner <rom.gb> --screenshot <out.bmp> [frames]\n";
        return 2;
    }
    std::ifstream file(argv[1], std::ios::binary);
    const std::vector<std::uint8_t> rom(std::istreambuf_iterator<char>(file), {});
    auto gb = ezy::gb::GameBoy::create(rom);
    if (!gb) {
        std::cerr << "Not a valid Game Boy ROM: " << argv[1] << '\n';
        return 2;
    }
    const auto& header = gb->bus().cartridge().header();
    std::cout << "Title: " << header.title << " (" << ezy::gb::cart_type_name(header.cart_type) << ")\n";

    if (argc >= 4 && std::strcmp(argv[2], "--screenshot") == 0) {
        const int frames = argc > 4 ? std::atoi(argv[4]) : 120;
        for (int i = 0; i < frames; ++i) gb->run_frame();
        if (!write_bmp(argv[3], gb->framebuffer(), ezy::gb::kLcdWidth, ezy::gb::kLcdHeight)) {
            std::cerr << "Could not write " << argv[3] << '\n';
            return 2;
        }
        std::cout << "Saved " << argv[3] << " after " << frames << " frames\n";
        return 0;
    }

    return run_serial(*gb, argc > 2 ? std::atof(argv[2]) : 120.0);
}
