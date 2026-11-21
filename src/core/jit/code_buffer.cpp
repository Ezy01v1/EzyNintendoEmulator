#include "jit/code_buffer.hpp"

#include "jit/code_buffer_impl.hpp"

namespace ezy::jit {

CodeBuffer::CodeBuffer(std::size_t budget_bytes) : impl_(std::make_unique<Impl>()) { impl_->budget = budget_bytes; }

CodeBuffer::~CodeBuffer() = default;

bool CodeBuffer::available() const { return EZY_JIT_AVAILABLE == 1; }

std::size_t CodeBuffer::used() const { return impl_->used; }

std::size_t CodeBuffer::budget() const { return impl_->budget; }

void CodeBuffer::reset() {
#if EZY_JIT_AVAILABLE
    impl_->runtime.reset(asmjit::ResetPolicy::kHard);
#endif
    impl_->used = 0;
}

}  // namespace ezy::jit
