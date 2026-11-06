// Headless Game Boy runner.
//
//   ezy_gb_runner <rom.gb> [max-seconds]
//       Runs the ROM and prints everything sent over the serial port. Blargg's test ROMs
//       report "Passed" / "Failed" there. Exit code 0 on "Passed".
//
//   ezy_gb_runner <rom.gb> --mooneye [max-seconds]
//       Runs a mooneye-test-suite ROM until it executes LD B,B (opcode 0x40), its
//       "test finished" breakpoint. Pass = registers hold Fibonacci numbers
//       B=3 C=5 D=8 E=13 H=21 L=34; fail = all of them 0x42. Exit code 0 on pass.
//
//   ezy_gb_runner <rom.gb> --screenshot <out.bmp> [frames]
//       Runs `frames` frames (default 120) and saves the screen as a BMP, e.g. to compare
//       dmg-acid2 against its reference image.
//
//   ezy_gb_runner <rom.gb> --compare <reference.bmp> [frames]
//       Runs `frames` frames (default 120) and compares the screen with a 24-bit BMP of
//       the same size, such as one written by --screenshot. Exit code 0 on an exact match.
//
// Options, anywhere after the ROM: --model auto|dmg|cgb (default auto = Game Boy Color),
// --boot-rom <file> (a real CGB boot ROM).

#include <algorithm>
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

// RGB555 -> 8-bit channels. The DMG greys map to exactly kGrey; any other colour is
// expanded by repeating its top bits.
std::array<std::uint8_t, 3> to_rgb888(std::uint16_t color) {
    for (std::size_t shade = 0; shade < 4; ++shade) {
        if (ezy::gb::kDmgGreys[shade] == color) return {kGrey[shade], kGrey[shade], kGrey[shade]};
    }
    const auto expand = [](int c) { return static_cast<std::uint8_t>((c << 3) | (c >> 2)); };
    return {expand(color & 0x1F), expand((color >> 5) & 0x1F), expand((color >> 10) & 0x1F)};
}

void put_u16(std::ofstream& out, std::uint16_t v) { out.write(reinterpret_cast<const char*>(&v), 2); }
void put_u32(std::ofstream& out, std::uint32_t v) { out.write(reinterpret_cast<const char*>(&v), 4); }

// Minimal 24-bit uncompressed BMP (bottom-up rows, each padded to 4 bytes).
bool write_bmp(const char* path, std::span<const std::uint16_t> pixels, int width, int height) {
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
            const auto [r, g, b] = to_rgb888(pixels[static_cast<std::size_t>(y * width + x)]);
            const auto at = static_cast<std::size_t>(x * 3);
            row[at] = static_cast<char>(b);  // BMP stores BGR
            row[at + 1] = static_cast<char>(g);
            row[at + 2] = static_cast<char>(r);
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

// Encodes the screen exactly as write_bmp() does, so the bytes can be compared.
std::vector<char> encode_bmp(std::span<const std::uint16_t> pixels, int width, int height) {
    const int row_size = (width * 3 + 3) & ~3;
    std::vector<char> data;
    for (int y = height - 1; y >= 0; --y) {
        std::vector<char> row(static_cast<std::size_t>(row_size), 0);
        for (int x = 0; x < width; ++x) {
            const auto [r, g, b] = to_rgb888(pixels[static_cast<std::size_t>(y * width + x)]);
            const auto at = static_cast<std::size_t>(x * 3);
            row[at] = static_cast<char>(b);
            row[at + 1] = static_cast<char>(g);
            row[at + 2] = static_cast<char>(r);
        }
        data.insert(data.end(), row.begin(), row.end());
    }
    return data;
}

int run_compare(ezy::gb::GameBoy& gb, const char* reference, int frames) {
    std::ifstream file(reference, std::ios::binary);
    const std::vector<char> bmp(std::istreambuf_iterator<char>(file), {});
    if (bmp.size() < 54) {
        std::cerr << "Could not read " << reference << '\n';
        return 2;
    }
    std::uint32_t offset = 0;
    std::memcpy(&offset, bmp.data() + 10, 4);
    for (int i = 0; i < frames; ++i) gb.run_frame();
    const auto mine = encode_bmp(gb.framebuffer(), ezy::gb::kLcdWidth, ezy::gb::kLcdHeight);
    const bool match = bmp.size() >= offset + mine.size() &&
                       std::equal(mine.begin(), mine.end(), bmp.begin() + static_cast<std::ptrdiff_t>(offset));
    std::cout << (match ? "Passed" : "Failed") << ": screen vs " << reference << " after " << frames << " frames\n";
    return match ? 0 : 1;
}

int run_mooneye(ezy::gb::GameBoy& gb, double max_seconds) {
    constexpr std::uint64_t kClockHz = 4'194'304;
    constexpr std::uint8_t kLdBB = 0x40;
    const auto max_cycles = static_cast<std::uint64_t>(max_seconds * kClockHz);
    const auto& bus = gb.bus();
    const auto& r = gb.cpu().regs();

    // Stop right after LD B,B executes (the ROM keeps looping on it afterwards).
    while (bus.t_cycles() < max_cycles && !gb.cpu().locked()) {
        const bool at_breakpoint = !gb.cpu().halted() && bus.peek(r.pc) == kLdBB;
        gb.step();
        if (at_breakpoint) break;
    }
    const bool pass = r.b == 3 && r.c == 5 && r.d == 8 && r.e == 13 && r.h == 21 && r.l == 34;
    std::cout << (pass ? "Passed" : "Failed") << std::hex << "  B=" << int{r.b} << " C=" << int{r.c}
              << " D=" << int{r.d} << " E=" << int{r.e} << " H=" << int{r.h} << " L=" << int{r.l} << std::dec
              << "  [" << static_cast<double>(bus.t_cycles()) / kClockHz << " emulated s]\n";
    return pass ? 0 : 1;
}

}  // namespace

int main(int raw_argc, char* raw_argv[]) {
    // Pull out the options; what is left is the positional form described at the top.
    ezy::gb::Options options;
    std::vector<char*> args;
    for (int i = 0; i < raw_argc; ++i) {
        if (std::strcmp(raw_argv[i], "--model") == 0 && i + 1 < raw_argc) {
            const std::string model = raw_argv[++i];
            if (model == "dmg") {
                options.model = ezy::gb::ModelChoice::Dmg;
            } else if (model == "cgb") {
                options.model = ezy::gb::ModelChoice::Cgb;
            } else if (model != "auto") {
                std::cerr << "Unknown model: " << model << '\n';
                return 2;
            }
        } else if (std::strcmp(raw_argv[i], "--boot-rom") == 0 && i + 1 < raw_argc) {
            std::ifstream boot(raw_argv[++i], std::ios::binary);
            options.boot_rom = std::vector<std::uint8_t>(std::istreambuf_iterator<char>(boot), {});
        } else {
            args.push_back(raw_argv[i]);
        }
    }
    const int argc = static_cast<int>(args.size());
    char** argv = args.data();

    if (argc < 2) {
        std::cerr << "Usage: ezy_gb_runner <rom.gb> [max-seconds]\n"
                     "       ezy_gb_runner <rom.gb> --mooneye [max-seconds]\n"
                     "       ezy_gb_runner <rom.gb> --screenshot <out.bmp> [frames]\n"
                     "       ezy_gb_runner <rom.gb> --compare <reference.bmp> [frames]\n"
                     "Options: --model auto|dmg|cgb, --boot-rom <file>\n";
        return 2;
    }
    std::ifstream file(argv[1], std::ios::binary);
    const std::vector<std::uint8_t> rom(std::istreambuf_iterator<char>(file), {});
    auto gb = ezy::gb::GameBoy::create(rom, std::move(options));
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

    if (argc >= 4 && std::strcmp(argv[2], "--compare") == 0) {
        return run_compare(*gb, argv[3], argc > 4 ? std::atoi(argv[4]) : 120);
    }

    if (argc >= 3 && std::strcmp(argv[2], "--mooneye") == 0) {
        return run_mooneye(*gb, argc > 3 ? std::atof(argv[3]) : 30.0);
    }

    return run_serial(*gb, argc > 2 ? std::atof(argv[2]) : 120.0);
}
