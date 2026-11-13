#pragma once

namespace ezy::n64 { class N64; }

// Warm-up is excluded. Profiling is optional and its timings are diagnostic only.
bool benchmark_frames(ezy::n64::N64& n64, int first, int end, const char* profile_path);
