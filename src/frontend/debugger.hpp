#pragma once

#include <SDL3/SDL.h>

#include "system.hpp"

namespace ezy::frontend {

// Run-control state shared between the main loop and the debugger UI.
// The UI only sets requests; the main loop applies them.
struct RunState {
    bool paused = false;
    bool step_instruction = false;
    bool step_frame = false;
    bool reset = false;
};

// Draws the shared windows (Screen, Controls) plus the console's own debug windows.
void draw_debugger(System& system, RunState& run, SDL_Texture* screen);

}  // namespace ezy::frontend
