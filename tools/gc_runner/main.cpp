// Headless GameCube runner (design, "Runner y ezyemu").
//
//   ezy_gc_runner <disc.iso|.gcm> --fields N [--screenshot out.bmp] [--trace N]
//       HLE-boots the disc and runs N fields (default 600), printing the stats every 60 fields
//       and at the end. --screenshot saves the XFB (what the VI shows) as a 24-bit BMP, also
//       when emulation stopped. On an unimplemented instruction, MMIO register or disc command
//       it prints the pc, the word or address, and the last --trace PCs executed (default 64),
//       and exits with code 1.
#include <charconv>
#include <cstdint>
#include <cstring>
#include <exception>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include "gc/gamecube.hpp"
#include "gc_run.hpp"

namespace {

void put_u16(std::vector<char>& out, std::uint16_t v) {
    out.push_back(static_cast<char>(v & 0xFF));
    out.push_back(static_cast<char>(v >> 8));
}
void put_u32(std::vector<char>& out, std::uint32_t v) {
    for (int i = 0; i < 4; ++i) out.push_back(static_cast<char>((v >> (8 * i)) & 0xFF));
}

// Minimal 24-bit uncompressed BMP (bottom-up rows, each padded to 4 bytes), as n64_runner.
std::vector<char> encode_bmp(const ezy::gc::Image& img) {
    const int row_size = (img.width * 3 + 3) & ~3;
    const auto data_size = static_cast<std::uint32_t>(row_size * img.height);
    std::vector<char> out;
    out.push_back('B'), out.push_back('M');
    put_u32(out, 54 + data_size), put_u32(out, 0), put_u32(out, 54), put_u32(out, 40);
    put_u32(out, static_cast<std::uint32_t>(img.width)), put_u32(out, static_cast<std::uint32_t>(img.height));
    put_u16(out, 1), put_u16(out, 24), put_u32(out, 0), put_u32(out, data_size);
    put_u32(out, 2835), put_u32(out, 2835), put_u32(out, 0), put_u32(out, 0);
    for (int y = img.height - 1; y >= 0; --y) {
        std::vector<char> row(static_cast<std::size_t>(row_size), 0);
        for (int x = 0; x < img.width; ++x) {
            const std::uint32_t p = img.argb[static_cast<std::size_t>(y * img.width + x)];
            const auto at = static_cast<std::size_t>(x * 3);
            row[at] = static_cast<char>(p & 0xFF);
            row[at + 1] = static_cast<char>((p >> 8) & 0xFF);
            row[at + 2] = static_cast<char>((p >> 16) & 0xFF);
        }
        out.insert(out.end(), row.begin(), row.end());
    }
    return out;
}

bool parse_int(const char* text, int& value) {
    const char* end = text + std::strlen(text);
    const auto [ptr, ec] = std::from_chars(text, end, value);
    return ec == std::errc() && ptr == end && value >= 0;
}

int usage() {
    std::cerr << "Usage: ezy_gc_runner <disc.iso|.gcm> --fields N [--screenshot out.bmp] [--trace N]\n";
    return 2;
}

}  // namespace

int main(int argc, char* argv[]) {
    if (argc < 2) return usage();
    int fields = 600, trace = 64;
    std::string screenshot;
    for (int i = 2; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--fields" && i + 1 < argc) {
            if (!parse_int(argv[++i], fields)) return usage();
        } else if (arg == "--trace" && i + 1 < argc) {
            if (!parse_int(argv[++i], trace)) return usage();
        } else if (arg == "--screenshot" && i + 1 < argc) {
            screenshot = argv[++i];
        } else {
            std::cerr << "Unknown option: " << arg << '\n';
            return usage();
        }
    }

    std::unique_ptr<ezy::gc::GameCube> gc;
    try {
        gc = ezy::gc::GameCube::create(argv[1]);
    } catch (const std::exception& e) {
        std::cerr << "The HLE boot failed: " << e.what() << '\n';
        return 1;
    }
    if (!gc) return 1;  // Disc::open printed why
    gc->set_pc_trace(static_cast<std::size_t>(trace));

    const ezy::gc_runner::RunReport report = ezy::gc_runner::run_fields(*gc, fields, std::cout);
    if (!screenshot.empty()) {
        const ezy::gc::Image img = gc->xfb();
        const std::vector<char> bmp = encode_bmp(img);
        std::ofstream file(screenshot, std::ios::binary);
        file.write(bmp.data(), static_cast<std::streamsize>(bmp.size()));
        if (!file) {
            std::cerr << "Could not write " << screenshot << '\n';
            return 1;
        }
        std::cout << "XFB " << img.width << "x" << img.height << " saved to " << screenshot << '\n';
    }
    return report.exit_code;
}
