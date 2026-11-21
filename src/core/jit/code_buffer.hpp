#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>

#include "jit/context.hpp"

// Public header of ezy_jit: it never includes AsmJit.
namespace ezy::jit {

class IrBlock;
struct Compiled;
enum class Backend : std::uint8_t;

using BlockFn = std::uint32_t (*)(Context*) noexcept;      // returns the id of the exit taken

// Executable memory for compiled blocks, with a byte budget. When compile() reports
// OutOfSpace the owner calls reset(), which frees every block at once.
class CodeBuffer {
public:
    explicit CodeBuffer(std::size_t budget_bytes);
    ~CodeBuffer();
    CodeBuffer(const CodeBuffer&) = delete;
    CodeBuffer& operator=(const CodeBuffer&) = delete;

    [[nodiscard]] bool available() const;                   // false: compile() reports Unsupported
    [[nodiscard]] std::size_t used() const;
    [[nodiscard]] std::size_t budget() const;
    // Hard reset of the allocator (its soft reset can keep metadata of freed blocks).
    // Every BlockFn from this buffer becomes invalid; never call it from native code.
    void reset();

private:
    friend Compiled compile(const IrBlock& block, CodeBuffer& buffer, Backend backend);
    struct Impl;  // jit/code_buffer_impl.hpp, private to ezy_jit
    std::unique_ptr<Impl> impl_;
};

}  // namespace ezy::jit
