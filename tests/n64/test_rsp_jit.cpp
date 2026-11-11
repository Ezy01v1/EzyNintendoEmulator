#include <doctest/doctest.h>
#include "n64/rsp_regs.hpp"

TEST_CASE("RSP bounded memory writes preserve bank wrapping and mirrored offsets") {
    ezy::n64::RspRegs rsp;
    const std::array<std::uint8_t, 4> bytes{0x12, 0x34, 0x56, 0x78};
    rsp.write_memory(0x1FFE, bytes);
    CHECK(rsp.read_memory8(0x1FFE) == 0x12);
    CHECK(rsp.read_memory8(0x1FFF) == 0x34);
    CHECK(rsp.read_memory8(0) == 0x56);
    CHECK(rsp.read_memory8(1) == 0x78);
    CHECK(rsp.read_memory8(0x3FFE) == 0x12);
    rsp.write_memory(0xFFF, bytes);
    CHECK(rsp.read_memory8(0xFFF) == 0x12);
    CHECK(rsp.read_memory8(0x1000) == 0x34);
    CHECK(rsp.dmem()[0xFFF] == 0x12);
}

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
