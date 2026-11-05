#pragma once

#include <SDL3/SDL.h>

#include <cstdint>
#include <span>
#include <string_view>

namespace ezy::frontend {

// One emulated console, as seen by the frontend. The main loop, audio, screen texture and
// debugger shell are shared; each console plugs in by implementing this interface.
// Adding GBA later means one more implementation, not a new main().
class System {
public:
    virtual ~System() = default;

    [[nodiscard]] virtual std::string_view name() const = 0;
    [[nodiscard]] virtual int screen_width() const = 0;
    [[nodiscard]] virtual int screen_height() const = 0;
    [[nodiscard]] virtual double frame_seconds() const = 0;  // real duration of one emulated frame

    virtual void run_frame() = 0;
    virtual void step_instruction() = 0;
    virtual void reset() = 0;

    virtual void handle_key(SDL_Scancode scancode, bool pressed) = 0;
    virtual void render(std::span<std::uint32_t> argb_pixels) const = 0;
    [[nodiscard]] virtual bool sound_active() const = 0;  // for the shared beeper

    // Systems with a real sound chip produce interleaved stereo float samples instead.
    [[nodiscard]] virtual int audio_sample_rate() const { return 0; }  // 0: uses the beeper
    [[nodiscard]] virtual std::span<const float> audio_samples() const { return {}; }
    virtual void clear_audio_samples() {}

    virtual void draw_debug_windows() = 0;  // console-specific ImGui windows
};

}  // namespace ezy::frontend
