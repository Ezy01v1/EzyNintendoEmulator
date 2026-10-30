#include "debugger.hpp"

#include <imgui.h>

#include <algorithm>
#include <cstdint>

#include "debug_ui.hpp"

namespace ezy::frontend {

namespace {

void draw_screen(const System& system, SDL_Texture* screen) {
    debug_ui::place_window(10, 10, 780, 390);
    ImGui::Begin("Screen");
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    const auto w = static_cast<float>(system.screen_width());
    const auto h = static_cast<float>(system.screen_height());
    const float scale = std::max(1.0F, std::min(avail.x / w, avail.y / h));
    ImGui::Image(static_cast<ImTextureID>(reinterpret_cast<std::intptr_t>(screen)), {w * scale, h * scale});
    ImGui::End();
}

void draw_controls(const System& system, RunState& run) {
    debug_ui::place_window(800, 10, 470, 110);
    ImGui::Begin("Controls");
    ImGui::Text("%.*s", static_cast<int>(system.name().size()), system.name().data());

    if (ImGui::Button(run.paused ? "Resume (P)" : "Pause (P)")) run.paused = !run.paused;
    ImGui::SameLine();
    ImGui::BeginDisabled(!run.paused);
    if (ImGui::Button("Step (N)")) run.step_instruction = true;
    ImGui::SameLine();
    if (ImGui::Button("Step frame")) run.step_frame = true;
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Reset (F5)")) run.reset = true;

    ImGui::TextColored(debug_ui::kDim, "Emulated %.2f Hz | UI %.0f FPS", 1.0 / system.frame_seconds(),
                       static_cast<double>(ImGui::GetIO().Framerate));
    ImGui::End();
}

}  // namespace

void draw_debugger(System& system, RunState& run, SDL_Texture* screen) {
    draw_screen(system, screen);
    draw_controls(system, run);
    system.draw_debug_windows();
}

}  // namespace ezy::frontend
