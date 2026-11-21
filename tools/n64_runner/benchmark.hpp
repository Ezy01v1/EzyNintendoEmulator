#pragma once

#include "frame_stats.hpp"

#include <optional>

namespace ezy::n64 { class N64; }

// Warm-up is excluded. Profiling is optional and its timings are diagnostic only.
// vi_output: also calls N64::output() once per timed field, as ezyemu does, and prints its
// time (total, mean and max per call; the RDP is drained before each call, outside the time).
// Each timed run_frame() is timed on its own (and its CPU JIT compile time); "Frames:" summarizes
// them and frame_times_path, when set, gets a CSV row per field. hold: buttons held on port 0
// from hold->from_field on, including the warm-up.
bool benchmark_frames(ezy::n64::N64& n64, int first, int end, const char* profile_path, bool vi_output = false,
                      const char* frame_times_path = nullptr, std::optional<HoldSpec> hold = std::nullopt);
