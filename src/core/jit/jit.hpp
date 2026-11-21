#pragma once

// Public header of ezy_jit: it never includes AsmJit.
namespace ezy::jit {

// True when this build can generate native code (CPU JIT enabled on an x86-64 host).
[[nodiscard]] bool available();

}  // namespace ezy::jit
