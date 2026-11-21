#include "jit/jit.hpp"

namespace ezy::jit {

bool available() { return EZY_JIT_AVAILABLE == 1; }

}  // namespace ezy::jit
