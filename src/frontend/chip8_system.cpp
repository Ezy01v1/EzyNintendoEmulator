#include "chip8_system.hpp"

#include <imgui.h>

#include <array>
#include <format>
#include <string>

#include "chip8/disassembler.hpp"
#include "debug_ui.hpp"

namespace ezy::frontend {

namespace {

using chip8::Chip8;
using chip8::Quirks;

constexpr std::uint32_t kColorOn = 0xFFE0E0E0;
constexpr std::uint32_t kColorOff = 0xFF101018;

// CHIP-8 keypad      QWERTY keys (by physical position)
// 1 2 3 C            1 2 3 4
// 4 5 6 D            Q W E R
// 7 8 9 E            A S D F
// A 0 B F            Z X C V
constexpr std::array<SDL_Scancode, 16> kKeymap = {
    SDL_SCANCODE_X, SDL_SCANCODE_1, SDL_SCANCODE_2, SDL_SCANCODE_3,  // 0 1 2 3
    SDL_SCANCODE_Q, SDL_SCANCODE_W, SDL_SCANCODE_E, SDL_SCANCODE_A,  // 4 5 6 7
    SDL_SCANCODE_S, SDL_SCANCODE_D, SDL_SCANCODE_Z, SDL_SCANCODE_C,  // 8 9 A B
    SDL_SCANCODE_4, SDL_SCANCODE_R, SDL_SCANCODE_F, SDL_SCANCODE_V,  // C D E F
};

void draw_settings(Chip8& chip8, int& instructions_per_frame) {
    debug_ui::place_window(800, 130, 470, 150);
    ImGui::Begin("CHIP-8");
    ImGui::SliderInt("Instr/frame", &instructions_per_frame, 1, 100);
    ImGui::TextColored(debug_ui::kDim, "CPU ~%d Hz", instructions_per_frame * 60);

    ImGui::SeparatorText("Quirks");
    Quirks q = chip8.quirks();
    bool changed = false;
    changed |= ImGui::Checkbox("VF reset", &q.vf_reset);
    ImGui::SameLine(160);
    changed |= ImGui::Checkbox("Memory increment", &q.memory_increment);
    changed |= ImGui::Checkbox("Display wait", &q.display_wait);
    ImGui::SameLine(160);
    changed |= ImGui::Checkbox("Clipping", &q.clipping);
    changed |= ImGui::Checkbox("Shift uses VY", &q.shift_uses_vy);
    ImGui::SameLine(160);
    changed |= ImGui::Checkbox("Jump uses V0", &q.jump_uses_v0);
    if (ImGui::Button("COSMAC VIP")) {
        q = Quirks::cosmac_vip();
        changed = true;
    }
    ImGui::SameLine();
    if (ImGui::Button("SUPER-CHIP (modern)")) {
        q = Quirks::superchip_modern();
        changed = true;
    }
    if (changed) chip8.set_quirks(q);
    ImGui::End();
}

void draw_cpu(const Chip8& chip8) {
    debug_ui::place_window(800, 290, 470, 420);
    ImGui::Begin("CPU");

    if (ImGui::BeginTable("registers", 4, ImGuiTableFlags_Borders | ImGuiTableFlags_SizingStretchSame)) {
        for (std::size_t r = 0; r < 16; ++r) {
            ImGui::TableNextColumn();
            ImGui::Text("V%X  %02X", static_cast<unsigned>(r), chip8.v(r));
        }
        ImGui::EndTable();
    }

    ImGui::TextColored(debug_ui::kHighlightPc, "PC  %03X", chip8.pc());
    ImGui::SameLine(120);
    ImGui::TextColored(debug_ui::kHighlightI, "I   %03X", chip8.index());
    ImGui::SameLine(240);
    ImGui::Text("SP  %X", chip8.sp());
    ImGui::Text("DT  %02X", chip8.delay_timer());
    ImGui::SameLine(120);
    ImGui::Text("ST  %02X", chip8.sound_timer());

    ImGui::SeparatorText("Stack");
    if (chip8.sp() == 0) ImGui::TextColored(debug_ui::kDim, "(empty)");
    for (std::size_t level = 0; level < chip8.sp() && level < 16; ++level) {
        ImGui::Text("%X: %03X", static_cast<unsigned>(level), chip8.stack_at(level));
    }

    // Laid out like the original hex keypad, not like the PC keyboard.
    ImGui::SeparatorText("Keypad");
    constexpr std::array<std::uint8_t, 16> kLayout = {0x1, 0x2, 0x3, 0xC, 0x4, 0x5, 0x6, 0xD,
                                                      0x7, 0x8, 0x9, 0xE, 0xA, 0x0, 0xB, 0xF};
    for (std::size_t i = 0; i < kLayout.size(); ++i) {
        if (i % 4 != 0) ImGui::SameLine();
        ImGui::BeginDisabled(!chip8.key(kLayout[i]));
        ImGui::Button(std::format("{:X}", kLayout[i]).c_str(), {32, 32});
        ImGui::EndDisabled();
    }
    ImGui::End();
}

void draw_disassembly(const Chip8& chip8) {
    debug_ui::place_window(10, 410, 380, 300);
    ImGui::Begin("Disassembly");

    constexpr int kLinesBefore = 10;
    constexpr int kLines = 24;
    const int pc = chip8.pc();
    int start = pc - kLinesBefore * 2;
    if (start < 0) start = pc % 2;

    for (int k = 0; k < kLines; ++k) {
        const int addr = start + k * 2;
        if (addr + 1 >= static_cast<int>(chip8::kMemorySize)) break;
        const auto opcode = static_cast<std::uint16_t>((chip8.read(static_cast<std::uint16_t>(addr)) << 8) |
                                                       chip8.read(static_cast<std::uint16_t>(addr + 1)));
        const std::string line = std::format("{} {:03X}  {:04X}  {}", addr == pc ? '>' : ' ', addr, opcode,
                                             chip8::disassemble(opcode));
        if (addr == pc) {
            ImGui::TextColored(debug_ui::kHighlightPc, "%s", line.c_str());
        } else {
            ImGui::TextUnformatted(line.c_str());
        }
    }
    ImGui::End();
}

void draw_memory(const Chip8& chip8) {
    debug_ui::place_window(400, 410, 390, 300);
    ImGui::Begin("Memory");
    ImGui::TextColored(debug_ui::kHighlightPc, "PC");
    ImGui::SameLine();
    ImGui::TextColored(debug_ui::kHighlightI, "I");
    ImGui::Separator();

    constexpr int kBytesPerRow = 8;
    const auto memory = chip8.memory();
    ImGui::BeginChild("hex");
    ImGuiListClipper clipper;
    clipper.Begin(static_cast<int>(memory.size()) / kBytesPerRow);
    while (clipper.Step()) {
        for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row) {
            ImGui::TextColored(debug_ui::kDim, "%03X:", row * kBytesPerRow);
            for (int col = 0; col < kBytesPerRow; ++col) {
                const int addr = row * kBytesPerRow + col;
                const unsigned value = memory[static_cast<std::size_t>(addr)];
                ImGui::SameLine();
                if (addr == chip8.pc() || addr == chip8.pc() + 1) {
                    ImGui::TextColored(debug_ui::kHighlightPc, "%02X", value);
                } else if (addr == chip8.index()) {
                    ImGui::TextColored(debug_ui::kHighlightI, "%02X", value);
                } else {
                    ImGui::Text("%02X", value);
                }
            }
        }
    }
    ImGui::EndChild();
    ImGui::End();
}

}  // namespace

std::unique_ptr<Chip8System> Chip8System::create(std::span<const std::uint8_t> rom) {
    auto system = std::unique_ptr<Chip8System>(new Chip8System({rom.begin(), rom.end()}));
    if (!system->chip8_.load_rom(system->rom_)) return nullptr;
    return system;
}

void Chip8System::run_frame() {
    for (int i = 0; i < instructions_per_frame_; ++i) {
        chip8_.step();
    }
    chip8_.tick_timers();
}

void Chip8System::handle_key(SDL_Scancode scancode, bool pressed) {
    for (std::size_t key = 0; key < kKeymap.size(); ++key) {
        if (kKeymap[key] == scancode) {
            chip8_.set_key(static_cast<std::uint8_t>(key), pressed);
        }
    }
}

void Chip8System::render(std::span<std::uint32_t> argb_pixels) const {
    const auto fb = chip8_.framebuffer();
    for (std::size_t i = 0; i < argb_pixels.size(); ++i) {
        argb_pixels[i] = fb[i] ? kColorOn : kColorOff;
    }
}

void Chip8System::draw_debug_windows() {
    draw_settings(chip8_, instructions_per_frame_);
    draw_cpu(chip8_);
    draw_disassembly(chip8_);
    draw_memory(chip8_);
}

}  // namespace ezy::frontend
