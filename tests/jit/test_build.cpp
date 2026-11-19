#include <doctest/doctest.h>

#include "jit/jit.hpp"

TEST_CASE("ezy_jit links and reports whether native code is available") {
    CHECK(ezy::jit::available() == (EZY_JIT_AVAILABLE == 1));
}
