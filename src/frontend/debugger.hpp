#pragma once

#include <SDL3/SDL.h>

#include "chip8/chip8.hpp"

namespace ezy::frontend {

// Run-control state shared between the main loop and the debugger UI.
// The UI only sets requests; the main loop applies them.
struct RunState {
    bool paused = false;
    int instructions_per_frame = 11;  // ~660 Hz at 60 frames per second
    bool step_instruction = false;
    bool step_frame = false;
    bool reset = false;
};

// Draws all debugger windows: Screen, Controls, CPU, Disassembly and Memory.
void draw_debugger(chip8::Chip8& chip8, RunState& run, SDL_Texture* screen);

}  // namespace ezy::frontend
