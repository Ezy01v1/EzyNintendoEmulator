#include "n64_system.hpp"

#include <imgui.h>

#include <array>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>

#include "debug_ui.hpp"
#include "file_util.hpp"

namespace ezy::frontend {

namespace {

// Controller buttons (joybus read order): A B Z Start Up Down Left Right, -, -, L R,
// C-Up C-Down C-Left C-Right.
struct KeyBinding {
    SDL_Scancode key;
    std::uint16_t bit;
};

constexpr std::array<KeyBinding, 14> kKeymap = {{
    {SDL_SCANCODE_Z, 0x8000},       // A
    {SDL_SCANCODE_X, 0x4000},       // B
    {SDL_SCANCODE_Q, 0x2000},       // Z trigger
    {SDL_SCANCODE_RETURN, 0x1000},  // Start
    {SDL_SCANCODE_UP, 0x0800},
    {SDL_SCANCODE_DOWN, 0x0400},
    {SDL_SCANCODE_LEFT, 0x0200},
    {SDL_SCANCODE_RIGHT, 0x0100},
    {SDL_SCANCODE_A, 0x0020},  // L
    {SDL_SCANCODE_S, 0x0010},  // R
    {SDL_SCANCODE_T, 0x0008},  // C-Up
    {SDL_SCANCODE_G, 0x0004},  // C-Down
    {SDL_SCANCODE_F, 0x0002},  // C-Left
    {SDL_SCANCODE_H, 0x0001},  // C-Right
}};

// I/J/K/L move the analog stick to +-80 (about what a real stick reaches).
constexpr std::array<SDL_Scancode, 4> kStickKeys = {SDL_SCANCODE_I, SDL_SCANCODE_K, SDL_SCANCODE_J, SDL_SCANCODE_L};
constexpr std::int8_t kStickRange = 80;

const char* const kGprNames[32] = {"r0", "at", "v0", "v1", "a0", "a1", "a2", "a3", "t0", "t1", "t2",
                                   "t3", "t4", "t5", "t6", "t7", "s0", "s1", "s2", "s3", "s4", "s5",
                                   "s6", "s7", "t8", "t9", "k0", "k1", "gp", "sp", "s8", "ra"};

// A short MIPS disassembly for the debugger: the common opcodes by name, the rest raw.
std::string disassemble(std::uint32_t w) {
    const unsigned op = w >> 26, rs = (w >> 21) & 31, rt = (w >> 16) & 31, rd = (w >> 11) & 31, sa = (w >> 6) & 31;
    const auto imm = static_cast<std::int16_t>(w & 0xFFFF);
    char buf[64];
    const auto r3 = [&](const char* name) { std::snprintf(buf, sizeof buf, "%-7s %s, %s, %s", name, kGprNames[rd], kGprNames[rs], kGprNames[rt]); };
    const auto ri = [&](const char* name) { std::snprintf(buf, sizeof buf, "%-7s %s, %s, %d", name, kGprNames[rt], kGprNames[rs], imm); };
    const auto mem = [&](const char* name) { std::snprintf(buf, sizeof buf, "%-7s %s, %d(%s)", name, kGprNames[rt], imm, kGprNames[rs]); };
    const auto br = [&](const char* name) { std::snprintf(buf, sizeof buf, "%-7s %s, %s, %+d", name, kGprNames[rs], kGprNames[rt], imm * 4 + 4); };
    if (w == 0) return "nop";
    switch (op) {
        case 0x00:
            switch (w & 0x3F) {
                case 0x00: std::snprintf(buf, sizeof buf, "%-7s %s, %s, %u", "sll", kGprNames[rd], kGprNames[rt], sa); return buf;
                case 0x08: std::snprintf(buf, sizeof buf, "%-7s %s", "jr", kGprNames[rs]); return buf;
                case 0x09: std::snprintf(buf, sizeof buf, "%-7s %s", "jalr", kGprNames[rs]); return buf;
                case 0x21: r3("addu"); return buf;
                case 0x23: r3("subu"); return buf;
                case 0x24: r3("and"); return buf;
                case 0x25: r3("or"); return buf;
                case 0x2A: r3("slt"); return buf;
                case 0x2B: r3("sltu"); return buf;
                case 0x2D: r3("daddu"); return buf;
                default: break;
            }
            break;
        case 0x02: std::snprintf(buf, sizeof buf, "%-7s 0x%07X", "j", (w & 0x3FFFFFF) << 2); return buf;
        case 0x03: std::snprintf(buf, sizeof buf, "%-7s 0x%07X", "jal", (w & 0x3FFFFFF) << 2); return buf;
        case 0x04: br("beq"); return buf;
        case 0x05: br("bne"); return buf;
        case 0x09: ri("addiu"); return buf;
        case 0x0C: ri("andi"); return buf;
        case 0x0D: ri("ori"); return buf;
        case 0x0F: std::snprintf(buf, sizeof buf, "%-7s %s, 0x%04X", "lui", kGprNames[rt], w & 0xFFFF); return buf;
        case 0x10: std::snprintf(buf, sizeof buf, "%-7s %s, $%u", rs == 4 ? "mtc0" : rs == 0 ? "mfc0" : "cop0", kGprNames[rt], rd); return buf;
        case 0x20: mem("lb"); return buf;
        case 0x21: mem("lh"); return buf;
        case 0x23: mem("lw"); return buf;
        case 0x24: mem("lbu"); return buf;
        case 0x25: mem("lhu"); return buf;
        case 0x28: mem("sb"); return buf;
        case 0x29: mem("sh"); return buf;
        case 0x2B: mem("sw"); return buf;
        case 0x2F: mem("cache"); return buf;
        case 0x37: mem("ld"); return buf;
        case 0x3F: mem("sd"); return buf;
        default: break;
    }
    std::snprintf(buf, sizeof buf, ".word   0x%08X", w);
    return buf;
}

}  // namespace

std::unique_ptr<N64System> N64System::create(std::span<const std::uint8_t> rom, std::filesystem::path rom_path) {
    auto n64 = n64::N64::create(rom);
    if (!n64) return nullptr;
    return std::unique_ptr<N64System>(new N64System({rom.begin(), rom.end()}, std::move(n64), std::move(rom_path)));
}

double N64System::frame_seconds() const {
    return n64_->cartridge().region() == n64::Region::Pal ? 1.0 / 50.0 : 1.001 / 60.0;
}

void N64System::run_frame() {
    n64_->run_frame();
    collect_audio();
}

void N64System::collect_audio() {
    n64::Ai& ai = n64_->bus().ai();
    const auto in = ai.samples();
    const int rate = ai.sample_rate();
    if (rate > 0 && rate < 200'000) {
        const double step = static_cast<double>(rate) / kAudioRate;
        const std::size_t frames = in.size() / 2;
        for (; resample_phase_ < static_cast<double>(frames); resample_phase_ += step) {
            const auto i = static_cast<std::size_t>(resample_phase_) * 2;
            audio_.push_back(in[i]);
            audio_.push_back(in[i + 1]);
        }
        resample_phase_ -= static_cast<double>(frames);
    }
    ai.clear_samples();
}

void N64System::reset() {
    n64_ = n64::N64::create(rom_);
    resample_phase_ = 0;
    update_controller();
}

void N64System::update_controller() {
    stick_x_ = static_cast<std::int8_t>(((stick_keys_ & 8) ? kStickRange : 0) - ((stick_keys_ & 4) ? kStickRange : 0));
    stick_y_ = static_cast<std::int8_t>(((stick_keys_ & 1) ? kStickRange : 0) - ((stick_keys_ & 2) ? kStickRange : 0));
    n64_->bus().pif().set_buttons(0, buttons_, stick_x_, stick_y_);
}

void N64System::handle_key(SDL_Scancode scancode, bool pressed) {
    if (pressed && scancode == SDL_SCANCODE_F2) save_state();
    if (pressed && scancode == SDL_SCANCODE_F4) load_state();
    for (const auto& [key, bit] : kKeymap) {
        if (key == scancode) buttons_ = static_cast<std::uint16_t>(pressed ? buttons_ | bit : buttons_ & ~bit);
    }
    for (std::size_t i = 0; i < kStickKeys.size(); ++i) {
        if (kStickKeys[i] != scancode) continue;
        const auto bit = static_cast<std::uint8_t>(1U << i);
        stick_keys_ = static_cast<std::uint8_t>(pressed ? stick_keys_ | bit : stick_keys_ & ~bit);
    }
    update_controller();
}

void N64System::render(std::span<std::uint32_t> argb_pixels) const {
    const n64::Vi::Image img = n64_->image();
    if (img.width <= 0 || img.height <= 0) {
        std::fill(argb_pixels.begin(), argb_pixels.end(), 0xFF000000);
        return;
    }
    // Nearest-neighbour scale of the VI image to the 640x480 canvas.
    for (int y = 0; y < kCanvasHeight; ++y) {
        const int sy = y * img.height / kCanvasHeight;
        for (int x = 0; x < kCanvasWidth; ++x) {
            const int sx = x * img.width / kCanvasWidth;
            argb_pixels[static_cast<std::size_t>(y * kCanvasWidth + x)] =
                img.argb[static_cast<std::size_t>(sy * img.width + sx)];
        }
    }
}

void N64System::save_state() {
    if (rom_path_.empty()) return;
    const std::filesystem::path path = state_path(rom_path_);
    if (write_file_atomically(path, n64_->save_state())) std::cerr << "Saved state to " << path.string() << "\n";
}

void N64System::load_state() {
    if (rom_path_.empty()) return;
    const std::filesystem::path path = state_path(rom_path_);
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        std::cerr << "No save state at " << path.string() << "\n";
        return;
    }
    const std::vector<std::uint8_t> data(std::istreambuf_iterator<char>(file), {});
    if (!n64_->load_state(data)) {
        std::cerr << path.string() << " is not a save state of this game and version; ignoring it\n";
        return;
    }
    update_controller();  // the buttons held now, not the ones in the state
    std::cerr << "Loaded state from " << path.string() << "\n";
}

void N64System::draw_debug_windows() {
    const n64::Vr4300& cpu = n64_->cpu();
    debug_ui::place_window(800, 130, 470, 330);
    ImGui::Begin("VR4300");
    for (int i = 0; i < 32; i += 2) {
        ImGui::Text("%-2s %016llX  %-2s %016llX", kGprNames[i], static_cast<unsigned long long>(cpu.gpr(i)),
                    kGprNames[i + 1], static_cast<unsigned long long>(cpu.gpr(i + 1)));
    }
    ImGui::Text("HI %016llX  LO %016llX", static_cast<unsigned long long>(cpu.hi()),
                static_cast<unsigned long long>(cpu.lo()));
    ImGui::Text("cycles %llu", static_cast<unsigned long long>(n64_->cycles()));
    ImGui::End();

    debug_ui::place_window(800, 470, 470, 240);
    ImGui::Begin("COP0 / COP1 / code");
    const n64::Cop0& c0 = cpu.cop0();
    ImGui::Text("Status %08llX  Cause %08llX", static_cast<unsigned long long>(c0.read(n64::Cop0::kStatus)),
                static_cast<unsigned long long>(c0.read(n64::Cop0::kCause)));
    ImGui::Text("EPC %016llX  Count %08llX", static_cast<unsigned long long>(c0.read(n64::Cop0::kEpc)),
                static_cast<unsigned long long>(c0.read(n64::Cop0::kCount)));
    ImGui::Text("FCR31 %08X", cpu.cop1().fcr31());
    // Code at PC, when it's in kseg0/kseg1 (no TLB lookups from the debugger).
    const std::uint64_t pc = cpu.pc();
    for (int i = 0; i < 6; ++i) {
        const std::uint64_t addr = pc + static_cast<std::uint64_t>(i) * 4;
        if (addr < 0xFFFFFFFF80000000ULL || addr >= 0xFFFFFFFFC0000000ULL) break;
        const auto word = n64_->bus().read32(static_cast<std::uint32_t>(addr & 0x1FFFFFFF));
        const std::string text = disassemble(word);
        if (i == 0) ImGui::TextColored(debug_ui::kHighlightPc, "%08X  %s", static_cast<std::uint32_t>(addr), text.c_str());
        else ImGui::Text("%08X  %s", static_cast<std::uint32_t>(addr), text.c_str());
    }
    ImGui::End();
}

}  // namespace ezy::frontend
