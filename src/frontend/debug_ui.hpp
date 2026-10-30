#pragma once

#include <imgui.h>

// Small helpers shared by every console's debugger windows.
namespace ezy::frontend::debug_ui {

inline constexpr ImVec4 kHighlightPc{1.0F, 0.85F, 0.3F, 1.0F};
inline constexpr ImVec4 kHighlightI{0.4F, 0.8F, 1.0F, 1.0F};
inline constexpr ImVec4 kDim{0.5F, 0.5F, 0.5F, 1.0F};

// Default layout for a 1280x720 window; ImGui remembers changes in imgui.ini.
inline void place_window(float x, float y, float w, float h) {
    ImGui::SetNextWindowPos({x, y}, ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize({w, h}, ImGuiCond_FirstUseEver);
}

}  // namespace ezy::frontend::debug_ui
