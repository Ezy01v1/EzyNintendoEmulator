#include <doctest/doctest.h>

#if !defined(EZY_N64_RSP_JIT) || EZY_N64_RSP_JIT
#include <asmjit/x86.h>

TEST_CASE("RSP JIT dependency allocates and executes native x64 code") {
    asmjit::JitRuntime runtime;
    asmjit::CodeHolder code;
    REQUIRE(static_cast<unsigned>(code.init(runtime.environment(), runtime.cpu_features())) == 0);
    asmjit::x86::Assembler assembler(&code);
    REQUIRE(static_cast<unsigned>(assembler.mov(asmjit::x86::eax, 42)) == 0);
    REQUIRE(static_cast<unsigned>(assembler.ret()) == 0);
    int (*function)() = nullptr;
    REQUIRE(static_cast<unsigned>(runtime.add(&function, &code)) == 0);
    REQUIRE(function != nullptr);
    CHECK(function() == 42);
    CHECK(static_cast<unsigned>(runtime.release(function)) == 0);
}
#endif
