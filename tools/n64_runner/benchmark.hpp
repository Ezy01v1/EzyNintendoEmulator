#pragma once

namespace ezy::n64 { class N64; }

// Warm-up is excluded. Profiling is optional and its timings are diagnostic only.
// vi_output: also calls N64::output() once per timed field, as ezyemu does, and prints its
// time (total, mean and max per call; the RDP is drained before each call, outside the time).
bool benchmark_frames(ezy::n64::N64& n64, int first, int end, const char* profile_path, bool vi_output = false);
