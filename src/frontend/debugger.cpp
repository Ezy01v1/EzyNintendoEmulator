#include "debugger.hpp"

#include <imgui.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <format>
#include <string>

#include "chip8/disassembler.hpp"

namespace ezy::frontend {

namespace {

using chip8::Chip8;
using chip8::Quirks;

constexpr ImVec4 kHighlightPc{1.0F, 0.85F, 0.3F, 1.0F};
constexpr ImVec4 kHighlightI{0.4F, 0.8F, 1.0F, 1.0F};
constexpr ImVec4 kDim{0.5F, 0.5F, 0.5F, 1.0F};

// Default layout for a 1280x720 window; ImGui remembers changes in imgui.ini.
void place_window(float x, float y, float w, float h) {
    ImGui::SetNextWindowPos({x, y}, ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize({w, h}, ImGuiCond_FirstUseEver);
}

void draw_screen(SDL_Texture* screen) {
    place_window(10, 10, 780, 390);
    ImGui::Begin("Screen");
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    const float scale = std::max(1.0F, std::min(avail.x / chip8::kScreenWidth, avail.y / chip8::kScreenHeight));
    ImGui::Image(static_cast<ImTextureID>(reinterpret_cast<std::intptr_t>(screen)),
                 {chip8::kScreenWidth * scale, chip8::kScreenHeight * scale});
    ImGui::End();
}

void draw_controls(Chip8& chip8, RunState& run) {
    place_window(800, 10, 470, 250);
    ImGui::Begin("Controls");

    if (ImGui::Button(run.paused ? "Resume (P)" : "Pause (P)")) run.paused = !run.paused;
    ImGui::SameLine();
    ImGui::BeginDisabled(!run.paused);
    if (ImGui::Button("Step (N)")) run.step_instruction = true;
    ImGui::SameLine();
    if (ImGui::Button("Step frame")) run.step_frame = true;
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Reset (F5)")) run.reset = true;

    ImGui::SliderInt("Instr/frame", &run.instructions_per_frame, 1, 100);
    ImGui::TextColored(kDim, "CPU ~%d Hz | UI %.0f FPS", run.instructions_per_frame * 60,
                       static_cast<double>(ImGui::GetIO().Framerate));

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
    place_window(800, 270, 470, 440);
    ImGui::Begin("CPU");

    if (ImGui::BeginTable("registers", 4, ImGuiTableFlags_Borders | ImGuiTableFlags_SizingStretchSame)) {
        for (std::size_t r = 0; r < 16; ++r) {
            ImGui::TableNextColumn();
            ImGui::Text("V%X  %02X", static_cast<unsigned>(r), chip8.v(r));
        }
        ImGui::EndTable();
    }

    ImGui::TextColored(kHighlightPc, "PC  %03X", chip8.pc());
    ImGui::SameLine(120);
    ImGui::TextColored(kHighlightI, "I   %03X", chip8.index());
    ImGui::SameLine(240);
    ImGui::Text("SP  %X", chip8.sp());
    ImGui::Text("DT  %02X", chip8.delay_timer());
    ImGui::SameLine(120);
    ImGui::Text("ST  %02X", chip8.sound_timer());

    ImGui::SeparatorText("Stack");
    if (chip8.sp() == 0) ImGui::TextColored(kDim, "(empty)");
    for (std::size_t level = 0; level < chip8.sp() && level < 16; ++level) {
        ImGui::Text("%X: %03X", static_cast<unsigned>(level), chip8.stack_at(level));
    }

    // Laid out like the original hex keypad, not like the PC keyboard.
    ImGui::SeparatorText("Keypad");
    constexpr std::array<std::uint8_t, 16> kLayout = {0x1, 0x2, 0x3, 0xC, 0x4, 0x5, 0x6, 0xD,
                                                      0x7, 0x8, 0x9, 0xE, 0xA, 0x0, 0xB, 0xF};
    for (std::size_t i = 0; i < kLayout.size(); ++i) {
        if (i % 4 != 0) ImGui::SameLine();
        const bool down = chip8.key(kLayout[i]);
        ImGui::BeginDisabled(!down);
        ImGui::Button(std::format("{:X}", kLayout[i]).c_str(), {32, 32});
        ImGui::EndDisabled();
    }

    ImGui::End();
}

void draw_disassembly(const Chip8& chip8) {
    place_window(10, 410, 380, 300);
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
            ImGui::TextColored(kHighlightPc, "%s", line.c_str());
        } else {
            ImGui::TextUnformatted(line.c_str());
        }
    }

    ImGui::End();
}

void draw_memory(const Chip8& chip8) {
    place_window(400, 410, 390, 300);
    ImGui::Begin("Memory");
    ImGui::TextColored(kHighlightPc, "PC");
    ImGui::SameLine();
    ImGui::TextColored(kHighlightI, "I");
    ImGui::Separator();

    constexpr int kBytesPerRow = 8;
    const auto memory = chip8.memory();
    ImGui::BeginChild("hex");
    ImGuiListClipper clipper;
    clipper.Begin(static_cast<int>(memory.size()) / kBytesPerRow);
    while (clipper.Step()) {
        for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row) {
            ImGui::TextColored(kDim, "%03X:", row * kBytesPerRow);
            for (int col = 0; col < kBytesPerRow; ++col) {
                const int addr = row * kBytesPerRow + col;
                const unsigned value = memory[static_cast<std::size_t>(addr)];
                ImGui::SameLine();
                if (addr == chip8.pc() || addr == chip8.pc() + 1) {
                    ImGui::TextColored(kHighlightPc, "%02X", value);
                } else if (addr == chip8.index()) {
                    ImGui::TextColored(kHighlightI, "%02X", value);
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

void draw_debugger(Chip8& chip8, RunState& run, SDL_Texture* screen) {
    draw_screen(screen);
    draw_controls(chip8, run);
    draw_cpu(chip8);
    draw_disassembly(chip8);
    draw_memory(chip8);
}

}  // namespace ezy::frontend
