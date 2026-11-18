// Runs a game's real F3DEX microcode on the emulated RSP with hand-made display lists and
// compares the vertex buffer it leaves in DMEM (texture coordinates and lit colors) with
// what the graphics HLE computes for the same lists.
//
//   gfx_oracle <rom> [--fields N] [--dump <experiment>] [--data] [--walk]
//
// The ROM boots for N VI fields (default 300) so its microcode is in RDRAM; the microcode is
// then copied into a second, idle console where only the RSP runs. Exit code 0 when every
// value matches within 1. --dump prints DMEM after one experiment, --data the lights area of
// the microcode's data segment, --walk the lighting setup of the game's current display list.
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <optional>
#include <string_view>
#include <vector>

#include "n64/gfx_hle/f3dex.hpp"
#include "n64/gfx_hle/task.hpp"
#include "n64/n64.hpp"
#include "n64_test_helpers.hpp"

using namespace ezy::n64;

namespace {

constexpr std::uint32_t kDl = 0x600000, kMtx = 0x601000, kVtx = 0x602000, kVp = 0x603000, kLights = 0x604000;

std::uint32_t rd32(std::span<const std::uint8_t> m, std::uint32_t a) {
    return (std::uint32_t{m[a]} << 24) | (std::uint32_t{m[a + 1]} << 16) | (std::uint32_t{m[a + 2]} << 8) | m[a + 3];
}
void wr32(std::span<std::uint8_t> m, std::uint32_t a, std::uint32_t v) {
    m[a] = std::uint8_t(v >> 24), m[a + 1] = std::uint8_t(v >> 16), m[a + 2] = std::uint8_t(v >> 8), m[a + 3] = std::uint8_t(v);
}
void wr16(std::span<std::uint8_t> m, std::uint32_t a, std::uint16_t v) { m[a] = std::uint8_t(v >> 8), m[a + 1] = std::uint8_t(v); }

std::optional<OsTask> find_gfx_task(std::span<const std::uint8_t> rdram) {
    constexpr std::string_view kTag = "RSP Gfx ucode F3D";
    const std::string_view all(reinterpret_cast<const char*>(rdram.data()), rdram.size());
    const std::size_t tag = all.find(kTag);
    if (tag == std::string_view::npos) return std::nullopt;
    std::printf("Version string at 0x%06zX: %.40s\n", tag, all.data() + tag);
    for (std::uint32_t a = 0; a + 64 <= rdram.size(); a += 4) {
        if (rd32(rdram, a) != kOsTaskGraphics) continue;
        const std::uint32_t data = rd32(rdram, a + 24) & 0x7FFFFF, size = rd32(rdram, a + 28);
        if (data > tag || data + size <= tag || size > 0x2000) continue;
        std::array<std::uint32_t, 16> f{};
        for (unsigned i = 0; i < 16; ++i) f[i] = rd32(rdram, a + i * 4);
        std::printf("OSTask at 0x%06X: boot %08X+%X ucode %08X+%X data %08X+%X\n", a, f[2], f[3], f[4], f[5], f[6], f[7]);
        return OsTask{f[0], f[1], f[2], f[3], f[4], f[5], f[6], f[7], f[8], f[9], f[10], f[11], f[12], f[13], f[14], f[15]};
    }
    return std::nullopt;
}

struct Experiment {
    const char* name;
    std::array<double, 9> modelview;  // row-major 3x3 (row vector convention: v * M)
    std::array<std::int8_t, 3> lookat_x, lookat_y;
    std::uint32_t geometry;
    std::vector<std::array<std::int8_t, 3>> normals;
    bool y_first = false;
    int variant = 0;  // command order, see build()
    std::uint8_t y_green = 0x80;  // gdSPDefLookAt's "up" light color is {0, 0x80, 0}
};

void write_mtx(std::span<std::uint8_t> m, std::uint32_t a, const std::array<double, 16>& v) {
    for (unsigned i = 0; i < 16; ++i) {
        const auto fixed = static_cast<std::int32_t>(std::lround(v[i] * 65536.0));
        wr16(m, a + i * 2, static_cast<std::uint16_t>(fixed >> 16));
        wr16(m, a + 32 + i * 2, static_cast<std::uint16_t>(fixed));
    }
}

void build(std::span<std::uint8_t> m, const Experiment& e) {
    std::array<double, 16> id{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    std::array<double, 16> mv = id;
    for (unsigned r = 0; r < 3; ++r) for (unsigned c = 0; c < 3; ++c) mv[r * 4 + c] = e.modelview[r * 3 + c];
    write_mtx(m, kMtx, id);
    write_mtx(m, kMtx + 64, mv);
    const std::int16_t vp[8] = {640, 480, 511, 0, 640, 480, 511, 0};
    for (unsigned i = 0; i < 8; ++i) wr16(m, kVp + i * 2, static_cast<std::uint16_t>(vp[i]));
    // L0: white from +Z, L1 (ambient): dark grey. LookAt X and Y as Lights.
    const auto light = [&](std::uint32_t a, std::array<std::uint8_t, 3> col, std::array<std::int8_t, 3> dir) {
        for (unsigned i = 0; i < 3; ++i) m[a + i] = m[a + 4 + i] = col[i], m[a + 8 + i] = std::uint8_t(dir[i]);
    };
    light(kLights, {0xFF, 0xFF, 0xFF}, {0, 0, 127});
    light(kLights + 16, {0x20, 0x20, 0x20}, {0, 0, 0});
    light(kLights + 32, {0, 0, 0}, e.lookat_x);
    light(kLights + 48, {0, e.y_green, 0}, e.lookat_y);
    for (std::size_t i = 0; i < e.normals.size(); ++i) {
        const std::uint32_t a = kVtx + std::uint32_t(i) * 16;
        wr16(m, a, std::uint16_t(i * 3)), wr16(m, a + 2, std::uint16_t(i * 2)), wr16(m, a + 4, 0);
        wr32(m, a + 8, 0x12340000U + std::uint32_t(i) * 0x40);  // s, t when texture generation is off
        m[a + 12] = std::uint8_t(e.normals[i][0]), m[a + 13] = std::uint8_t(e.normals[i][1]);
        m[a + 14] = std::uint8_t(e.normals[i][2]), m[a + 15] = 0xFF;
    }
    const unsigned n = static_cast<unsigned>(e.normals.size());
    const std::uint64_t projection = (0x01030040ULL << 32) | kMtx, modelview = (0x01020040ULL << 32) | (kMtx + 64);
    const std::uint64_t viewport = (0x03800010ULL << 32) | kVp, numlight = (0xBC000002ULL << 32) | (0x80000000U | 64);
    const std::uint64_t l0 = (0x03860010ULL << 32) | kLights, l1 = (0x03880010ULL << 32) | (kLights + 16);
    const std::uint64_t lx = (0x03840010ULL << 32) | (kLights + 32), ly = (0x03820010ULL << 32) | (kLights + 48);
    const std::uint64_t geometry = (0xB7000000ULL << 32) | e.geometry, texture = (0xBB000001ULL << 32) | 0x80008000U;
    std::vector<std::uint64_t> dl;
    switch (e.variant) {
        case 0: dl = {projection, modelview, viewport, numlight, l0, l1, e.y_first ? ly : lx, e.y_first ? lx : ly, geometry, texture}; break;
        case 1: dl = {l0, l1, lx, ly, numlight, projection, modelview, viewport, geometry, texture}; break;
        case 2: dl = {projection, modelview, viewport, numlight, l0, l1, lx, ly, modelview, geometry, texture}; break;
        case 3: dl = {projection, modelview, viewport, l0, l1, lx, ly, numlight, geometry, texture}; break;
        case 4: dl = {geometry, texture, projection, modelview, viewport, numlight, l0, l1, lx, ly}; break;
        case 6: dl = {projection, modelview, viewport, numlight, l0, l1, geometry, texture}; break;  // default lookat
        case 7: dl = {projection, modelview, viewport, lx, ly, geometry, texture}; break;  // default lights
        default: dl = {projection, modelview, viewport, geometry, texture}; break;  // the ucode's default lights
    }
    dl.push_back((std::uint64_t{0x04000000U | (n << 10) | (n * 16 - 1)} << 32) | kVtx);
    dl.push_back(0xB800000000000000ULL);
    for (std::size_t i = 0; i < dl.size(); ++i) {
        wr32(m, kDl + std::uint32_t(i) * 8, std::uint32_t(dl[i] >> 32));
        wr32(m, kDl + std::uint32_t(i) * 8 + 4, std::uint32_t(dl[i]));
    }
}

std::vector<std::uint8_t> read_file(const char* path) {
    std::ifstream f(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(f), {}};
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: gfx_oracle <rom> [--fields N] [--dump <experiment>] [--data] [--walk]\n");
        return 2;
    }
    int fields = 300;
    bool show_data = false, walk = false;
    std::string_view dump_name;
    for (int i = 2; i < argc; ++i) {
        const std::string_view arg = argv[i];
        if (arg == "--fields" && i + 1 < argc) fields = std::atoi(argv[++i]);
        else if (arg == "--dump" && i + 1 < argc) dump_name = argv[++i];
        else if (arg == "--data") show_data = true;
        else if (arg == "--walk") walk = true;
        else { std::fprintf(stderr, "unknown option %s\n", argv[i]); return 2; }
    }
    const auto rom = read_file(argv[1]);
    auto game = N64::create(rom, {}, RdpMode::Synchronous, RspMode::Interpreter);
    if (!game) return 2;
    for (int i = 0; i < fields; ++i) game->run_frame();
    const auto task = find_gfx_task(game->bus().rdram());
    if (!task) { std::fprintf(stderr, "no graphics task found\n"); return 1; }
    if (show_data) {  // the ucode's data segment as loaded into DMEM
        const std::uint32_t base = task->ucode_data & 0x7FFFFF;
        for (std::uint32_t a = 0x180; a < 0x240; a += 16) {
            std::printf("%03X:", a);
            for (std::uint32_t k = 0; k < 16; ++k) std::printf(" %02X", game->bus().rdram()[base + a + k]);
            std::printf("\n");
        }
        return 0;
    }
    if (walk) {
        // The game's own display list: the commands that set up lighting and texture generation.
        const auto ram = game->bus().rdram();
        std::array<std::uint32_t, 16> seg{};
        std::vector<std::uint32_t> stack;
        std::uint32_t pc = task->data_ptr & 0x7FFFF8;
        const auto phys = [&](std::uint32_t a) { return (seg[(a >> 24) & 15] + (a & 0xFFFFFF)) & 0x7FFFFF; };
        for (int i = 0; i < 200000; ++i) {
            const std::uint32_t w0 = rd32(ram, pc), w1 = rd32(ram, pc + 4);
            pc += 8;
            const unsigned op = w0 >> 24;
            if (op == 0x06) { if (((w0 >> 16) & 0xFF) == 0) stack.push_back(pc); pc = phys(w1) & ~7U; continue; }
            if (op == 0xB8) { if (stack.empty()) break; pc = stack.back(); stack.pop_back(); continue; }
            if (op == 0xBC && (w0 & 0xFF) == 6) seg[((w0 >> 8) & 0xFFFF) / 4 & 15] = w1 & 0xFFFFFF;
            if (op == 0x03 || op == 0xBC || op == 0xB7 || op == 0xB6 || op == 0xBB) {
                std::printf("%08X %08X", w0, w1);
                if (op == 0x03) {
                    const std::uint32_t a = phys(w1);
                    std::printf("  ->");
                    for (unsigned k = 0; k < 16; ++k) std::printf(" %02X", ram[a + k]);
                }
                std::printf("\n");
            }
        }
        return 0;
    }

    const auto idle_rom = n64_test::counting_rom("GFX ORACLE");
    auto lab = N64::create(idle_rom, {}, RdpMode::Synchronous, RspMode::Interpreter);
    auto& bus = lab->bus();
    const auto copy = [&](std::uint32_t at, std::uint32_t size) {
        at &= 0x7FFFFF;
        for (std::uint32_t i = 0; i < size && at + i < Bus::kRdramSize; ++i) bus.rdram()[at + i] = game->bus().rdram()[at + i];
    };
    copy(task->ucode_boot, task->ucode_boot_size);
    copy(task->ucode, 0x4000);  // text plus overlays
    copy(task->ucode_data, task->ucode_data_size);

    const std::array<std::int8_t, 3> x{127, 0, 0}, y{0, 127, 0};
    const double c = std::cos(0.5), s = std::sin(0.5);
    const std::vector<std::array<std::int8_t, 3>> normals = {
        {0, 0, 127}, {127, 0, 0}, {0, 127, 0}, {-127, 0, 0}, {0, -127, 0}, {90, 0, 90}, {0, 90, 90}, {50, 60, 70},
        {0, 0, 64},  {64, 0, 0},  {-100, 40, 20}, {20, -100, 40}, {30, 30, -110}, {0, 0, -127}, {127, 127, 0}, {10, 0, 0},
    };
    const std::uint32_t lit = 0x20000 | 0x4, gen = lit | 0x40000;
    const std::vector<Experiment> experiments = {
        {"calibration (no lighting)", {1, 0, 0, 0, 1, 0, 0, 0, 1}, x, y, 0x4, normals},
        {"calibration (no texgen)", {1, 0, 0, 0, 1, 0, 0, 0, 1}, x, y, lit, normals},
        {"lighting, rotate about Y", {c, 0, -s, 0, 1, 0, s, 0, c}, x, y, lit, normals},
        {"lighting, scale 2", {2, 0, 0, 0, 2, 0, 0, 0, 2}, x, y, lit, normals},
        {"lighting, default lights", {1, 0, 0, 0, 1, 0, 0, 0, 1}, x, y, lit, normals, false, 7},
        {"identity", {1, 0, 0, 0, 1, 0, 0, 0, 1}, x, y, gen, normals},
        {"rotate about Y", {c, 0, -s, 0, 1, 0, s, 0, c}, x, y, gen, normals},
        {"rotate about X, scale 2", {2, 0, 0, 0, 2 * c, 2 * s, 0, -2 * s, 2 * c}, x, y, gen, normals},
        {"lookat diagonal", {1, 0, 0, 0, 1, 0, 0, 0, 1}, {90, 0, 90}, {0, 90, -90}, gen, normals},
        {"linear", {1, 0, 0, 0, 1, 0, 0, 0, 1}, x, y, gen | 0x80000, normals},
        {"lookat Y loaded first", {1, 0, 0, 0, 1, 0, 0, 0, 1}, x, y, gen, normals, true},
        {"both axes along X", {1, 0, 0, 0, 1, 0, 0, 0, 1}, x, x, gen, normals},
        {"order: lights, numlight, matrices", {1, 0, 0, 0, 1, 0, 0, 0, 1}, x, y, gen, normals, false, 1},
        {"order: modelview again after lookat", {1, 0, 0, 0, 1, 0, 0, 0, 1}, x, y, gen, normals, false, 2},
        {"order: numlight after lookat", {1, 0, 0, 0, 1, 0, 0, 0, 1}, x, y, gen, normals, false, 3},
        {"order: geometry mode first", {1, 0, 0, 0, 1, 0, 0, 0, 1}, x, y, gen, normals, false, 4},
        {"defaults, identity", {1, 0, 0, 0, 1, 0, 0, 0, 1}, x, y, gen, normals, false, 5},
        {"defaults, rotate about Y", {c, 0, -s, 0, 1, 0, s, 0, c}, x, y, gen, normals, false, 5},
        {"Y color 0x40", {1, 0, 0, 0, 1, 0, 0, 0, 1}, x, y, gen, normals, false, 0, 0x40},
        {"Y color 0", {1, 0, 0, 0, 1, 0, 0, 0, 1}, x, y, gen, normals, false, 0, 0},
        {"linear sweep", {1, 0, 0, 0, 1, 0, 0, 0, 1}, x, y, gen | 0x80000,
         {{-127, 0, 0}, {-112, 0, 0}, {-96, 0, 0}, {-80, 0, 0}, {-64, 0, 0}, {-48, 0, 0}, {-32, 0, 0}, {-16, 0, 0},
          {0, 0, 0}, {16, 0, 0}, {32, 0, 0}, {48, 0, 0}, {64, 0, 0}, {80, 0, 0}, {96, 0, 0}, {112, 0, 0}}},
        {"one light, default lookat", {1, 0, 0, 0, 1, 0, 0, 0, 1}, x, y, gen, normals, false, 6},
        {"lookat loaded, default lights", {1, 0, 0, 0, 1, 0, 0, 0, 1}, x, y, gen, normals, false, 7},
    };

    std::optional<std::uint32_t> base;
    std::uint32_t stride = 0, s_offset = 0;
    std::optional<std::int32_t> color_offset;
    int worst = 0;
    for (const auto& e : experiments) {
        build(bus.rdram(), e);
        OsTask t = *task;
        t.flags = 0, t.data_ptr = kDl, t.data_size = 0x100;
        const std::array<std::uint32_t, 16> f = {t.type, t.flags, t.ucode_boot, t.ucode_boot_size, t.ucode, t.ucode_size,
                                                 t.ucode_data, t.ucode_data_size, t.dram_stack, t.dram_stack_size,
                                                 t.output_buff, t.output_buff_size, t.data_ptr, t.data_size,
                                                 t.yield_data_ptr, t.yield_data_size};
        for (unsigned i = 0; i < 16; ++i) bus.write32(0x04000FC0 + i * 4, f[i]);
        for (std::uint32_t i = 0; i < t.ucode_boot_size; i += 4) bus.write32(0x04001000 + i, rd32(bus.rdram(), (t.ucode_boot & 0x7FFFFF) + i));
        bus.write32(0x04080000, 0);                 // SP PC
        bus.write32(0x04040010, 1 | 4 | 0x100);    // clear halt and broke, interrupt on break
        std::uint64_t steps = 0;
        while ((bus.read32(0x04040010) & 1) == 0 && steps < 50'000'000) lab->step(), ++steps;
        std::array<std::uint8_t, 0x1000> dmem{};
        for (std::uint32_t i = 0; i < 0x1000; i += 4) wr32(dmem, i, bus.read32(0x04000000 + i));

        if (dump_name == e.name) {
            for (std::uint32_t a = 0; a < 0x4B0; a += 16) {
                std::printf("%03X:", a);
                for (std::uint32_t i = 0; i < 16; ++i) std::printf(" %02X", dmem[a + i]);
                std::printf("\n");
            }
        }
        F3dex hle;
        hle.run_task(t, identify_gfx_ucode(t, bus.rdram()), bus.rdram(), bus.rdp(), bus.mi());

        std::printf("\n== %s (RSP halted after %llu steps)\n", e.name, static_cast<unsigned long long>(steps));
        if (!base) {
            // s of vertex i is 0x1234 * 0x8000 >> 16 = 0x091A, t is (i * 0x40) * 0x8000 >> 16.
            for (std::uint32_t a = 0; a + 2 <= 0x1000 && !base; a += 2) {
                if (((dmem[a] << 8) | dmem[a + 1]) != 0x091A || ((dmem[a + 2] << 8) | dmem[a + 3]) != 0) continue;
                for (std::uint32_t st = 16; st <= 64 && !base; st += 2) {
                    if (a + st + 4 > 0x1000) break;
                    if (((dmem[a + st] << 8) | dmem[a + st + 1]) == 0x091A && ((dmem[a + st + 2] << 8) | dmem[a + st + 3]) == 0x20) {
                        base = a, stride = st, s_offset = 0;
                    }
                }
            }
            if (!base) { std::printf("vertex buffer not found\n"); return 1; }
            std::printf("vertex buffer: s,t of vertex 0 at DMEM 0x%03X, stride %u\n", *base, stride);
        }
        if (!color_offset && e.geometry == 0x4) {  // unlit: the color is the normal's bytes, alpha 0xFF
            for (std::int32_t off = -std::int32_t(stride); off < std::int32_t(stride) && !color_offset; ++off) {
                bool all = true;
                for (std::size_t i = 0; i < e.normals.size() && all; ++i) {
                    const auto a = static_cast<std::uint32_t>(std::int32_t(*base + i * stride) + off);
                    all = dmem[a] == std::uint8_t(e.normals[i][0]) && dmem[a + 1] == std::uint8_t(e.normals[i][1]) &&
                          dmem[a + 2] == std::uint8_t(e.normals[i][2]) && dmem[a + 3] == 0xFF;
                }
                if (all) color_offset = off;
            }
            std::printf("color at s %+d\n", color_offset ? *color_offset : 999);
        }
        for (std::size_t i = 0; i < e.normals.size(); ++i) {
            const std::uint32_t a = *base + std::uint32_t(i) * stride + s_offset;
            const auto us = std::int16_t((dmem[a] << 8) | dmem[a + 1]), ut = std::int16_t((dmem[a + 2] << 8) | dmem[a + 3]);
            const auto& v = hle.vertex(unsigned(i));
            const auto rgb = static_cast<std::uint32_t>(std::int32_t(a) + color_offset.value_or(0));
            int diff = std::max(std::abs(us - v.s), std::abs(ut - v.t));
            if (color_offset) {
                diff = std::max({diff, std::abs(dmem[rgb] - v.r), std::abs(dmem[rgb + 1] - v.g), std::abs(dmem[rgb + 2] - v.b)});
            }
            worst = std::max(worst, diff);
            std::printf("n %4d %4d %4d  ucode s %6d t %6d  hle s %6d t %6d  rgb ucode %3u %3u %3u hle %3u %3u %3u%s\n",
                        e.normals[i][0], e.normals[i][1], e.normals[i][2], us, ut, v.s, v.t, dmem[rgb], dmem[rgb + 1],
                        dmem[rgb + 2], v.r, v.g, v.b, diff > 1 ? "  <-" : "");
        }
    }
    std::printf("\nLargest difference between the microcode and the HLE: %d\n", worst);
    return worst <= 1 ? 0 : 1;
}
