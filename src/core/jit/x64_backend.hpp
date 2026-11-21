#pragma once

#include <cstdint>

#include "jit/code_buffer.hpp"
#include "jit/ir.hpp"

// Public header of ezy_jit: it never includes AsmJit.
namespace ezy::jit {

enum class CompileError : std::uint8_t {
    None,
    OutOfSpace,   // the block does not fit the buffer's remaining budget: reset() and retry
    Unsupported,  // no native code in this build, or the block uses something the backend lacks
};

struct Compiled {
    BlockFn fn = nullptr;
    CompileError error = CompileError::None;
};

// How compile() emits code. Assembler: our linear register allocator (reg_alloc.hpp) and an
// x86::Assembler, cheap to run. Compiler: AsmJit's x86::Compiler and its register allocator,
// kept as the reference to compare against.
enum class Backend : std::uint8_t { Assembler, Compiler };

// Translates a verified block to x86-64 code in `buffer`; failures come back as CompileError.
[[nodiscard]] Compiled compile(const IrBlock& block, CodeBuffer& buffer, Backend backend = Backend::Assembler);

}  // namespace ezy::jit
