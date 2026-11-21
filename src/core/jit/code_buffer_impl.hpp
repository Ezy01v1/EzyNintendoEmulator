#pragma once

// Private to ezy_jit (code_buffer.cpp and x64_backend.cpp): the only header that names AsmJit.
#include <cstddef>

#include "jit/code_buffer.hpp"

#if EZY_JIT_AVAILABLE
#include <asmjit/core.h>
#endif

namespace ezy::jit {

struct CodeBuffer::Impl {
    std::size_t budget = 0;
    std::size_t used = 0;
#if EZY_JIT_AVAILABLE
    asmjit::JitRuntime runtime;
#endif
};

}  // namespace ezy::jit
