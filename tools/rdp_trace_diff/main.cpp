// Compares two RDP command traces written by `ezy_n64_runner --rdp-trace`.
//
//   rdp_trace_diff <a.bin> <b.bin>
//
// Exit code 0 when they are identical. Otherwise prints the first command that differs
// (both versions, every word, with the opcode name) and the commands just before it.
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "n64/rdp.hpp"

namespace {

std::vector<std::uint64_t> read_trace(const char* path) {
    std::ifstream file(path, std::ios::binary);
    const std::vector<char> bytes{std::istreambuf_iterator<char>(file), {}};
    std::vector<std::uint64_t> words(bytes.size() / 8);
    for (std::size_t i = 0; i < words.size(); ++i) {
        std::uint64_t w = 0;
        for (int b = 7; b >= 0; --b) w = (w << 8) | static_cast<std::uint8_t>(bytes[i * 8 + static_cast<std::size_t>(b)]);
        words[i] = w;
    }
    return words;
}

const char* opcode_name(unsigned op) {
    switch (op) {
        case 0x08: return "Triangle";
        case 0x09: return "Triangle (Z)";
        case 0x0A: return "Triangle (texture)";
        case 0x0B: return "Triangle (texture, Z)";
        case 0x0C: return "Triangle (shade)";
        case 0x0D: return "Triangle (shade, Z)";
        case 0x0E: return "Triangle (shade, texture)";
        case 0x0F: return "Triangle (shade, texture, Z)";
        case 0x24: return "Texture Rectangle";
        case 0x25: return "Texture Rectangle Flip";
        case 0x26: return "Sync Load";
        case 0x27: return "Sync Pipe";
        case 0x28: return "Sync Tile";
        case 0x29: return "Sync Full";
        case 0x2A: return "Set Key GB";
        case 0x2B: return "Set Key R";
        case 0x2C: return "Set Convert";
        case 0x2D: return "Set Scissor";
        case 0x2E: return "Set Prim Depth";
        case 0x2F: return "Set Other Modes";
        case 0x30: return "Load TLUT";
        case 0x32: return "Set Tile Size";
        case 0x33: return "Load Block";
        case 0x34: return "Load Tile";
        case 0x35: return "Set Tile";
        case 0x36: return "Fill Rectangle";
        case 0x37: return "Set Fill Color";
        case 0x38: return "Set Fog Color";
        case 0x39: return "Set Blend Color";
        case 0x3A: return "Set Prim Color";
        case 0x3B: return "Set Env Color";
        case 0x3C: return "Set Combine";
        case 0x3D: return "Set Texture Image";
        case 0x3E: return "Set Mask Image";
        case 0x3F: return "Set Color Image";
        default: return "?";
    }
}

struct Command {
    std::size_t first;  // index of its first word
    std::size_t count;
};

std::vector<Command> split(const std::vector<std::uint64_t>& words) {
    std::vector<Command> commands;
    for (std::size_t i = 0; i < words.size();) {
        const unsigned op = static_cast<unsigned>(words[i] >> 56) & 63;
        std::size_t count = ezy::n64::Rdp::words_in_command(op);
        if (count == 0) count = 1;
        if (i + count > words.size()) count = words.size() - i;
        commands.push_back({i, count});
        i += count;
    }
    return commands;
}

void print(const char* label, const std::vector<std::uint64_t>& words, const Command& c) {
    const unsigned op = static_cast<unsigned>(words[c.first] >> 56) & 63;
    std::printf("  %s: 0x%02X %s\n", label, op, opcode_name(op));
    for (std::size_t i = 0; i < c.count; ++i) {
        std::printf("    [%2zu] %016llX\n", i, static_cast<unsigned long long>(words[c.first + i]));
    }
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 3) {
        std::fprintf(stderr, "Usage: rdp_trace_diff <a.bin> <b.bin>\n");
        return 2;
    }
    const auto a = read_trace(argv[1]);
    const auto b = read_trace(argv[2]);
    const auto ca = split(a);
    const auto cb = split(b);
    const std::size_t n = ca.size() < cb.size() ? ca.size() : cb.size();
    for (std::size_t i = 0; i < n; ++i) {
        bool same = ca[i].count == cb[i].count;
        for (std::size_t w = 0; same && w < ca[i].count; ++w) same = a[ca[i].first + w] == b[cb[i].first + w];
        if (same) continue;
        std::printf("First difference at command %zu (word %zu in %s, %zu in %s)\n", i, ca[i].first, argv[1],
                    cb[i].first, argv[2]);
        for (std::size_t k = i >= 3 ? i - 3 : 0; k < i; ++k) {
            std::printf("  before: 0x%02X %s\n", static_cast<unsigned>(a[ca[k].first] >> 56) & 63,
                        opcode_name(static_cast<unsigned>(a[ca[k].first] >> 56) & 63));
        }
        print("a", a, ca[i]);
        print("b", b, cb[i]);
        return 1;
    }
    if (ca.size() != cb.size()) {
        std::printf("Traces agree for %zu commands, then %s has %zu and %s has %zu\n", n, argv[1], ca.size(), argv[2],
                    cb.size());
        return 1;
    }
    std::printf("Identical: %zu commands, %zu words\n", ca.size(), a.size());
    return 0;
}
