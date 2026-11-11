#include <doctest/doctest.h>
#include "n64/rsp_regs.hpp"
#include "n64/rsp_jit.hpp"
#include "mips_asm.hpp"

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

TEST_CASE("RSP native blocks retire only the budgeted prefix and invalidate cached code") {
    using namespace ezy::n64;
    std::array<std::uint8_t,4096> imem{}, dmem{};
    std::array<std::uint32_t,32> gpr{};
    std::uint32_t pc = 0, next = 4;
    bool phase = true;
    const auto put = [&](unsigned at, std::uint32_t word) {
        for (unsigned i=0;i<4;++i) imem[at+i]=static_cast<std::uint8_t>(word>>(24-8*i));
    };
    put(0, mips::addiu(1, 0, -1));
    put(4, mips::addiu(2, 1, 2));
    put(8, mips::sw(2, 1, 0)); // DMEM wraps from 0xFFF to 0.
    put(12, mips::lw(3, 1, 0));
    put(16, mips::r(0,0,0,0,0,0x0D)); // BREAK is interpreted.
    RspJit jit;
    REQUIRE(jit.available());
    RspJitView view{gpr.data(), dmem.data(), &pc, &next, &phase, nullptr, nullptr};
    RspJitRun run{2, 3, 32, 0};
    jit.run(view, imem, run);
    CHECK(run.retired == 1);
    CHECK(run.when == 3);
    CHECK(pc == 4);
    CHECK(next == 8);
    CHECK(gpr[1] == 0xFFFFFFFF);
    run = {3, 100, 32, 0};
    jit.run(view, imem, run);
    CHECK(run.retired == 3);
    CHECK(pc == 16);
    CHECK(gpr[3] == 1);
    CHECK(dmem[2] == 1);
    CHECK(jit.stats().native_instructions == 4);
    put(0, mips::addiu(1, 0, 73));
    jit.invalidate();
    pc=0; next=4; phase=true;
    run={0,1,32,0};
    jit.run(view,imem,run);
    CHECK(gpr[1] == 73);
}

TEST_CASE("RSP JIT allocation denial safely leaves execution to the interpreter") {
    ezy::n64::RspJit denied(true);
    CHECK_FALSE(denied.available());
}

TEST_CASE("RSP JIT cache budget and final IMEM instruction stay bounded") {
    using namespace ezy::n64;
    std::array<std::uint8_t,4096> imem{},dmem{};
    std::array<std::uint32_t,32> gpr{};
    std::uint32_t pc=0,next=4;
    bool phase=false;
    RspJitView view{gpr.data(),dmem.data(),&pc,&next,&phase,nullptr,nullptr};
    RspJit jit(false,4096);
    for(unsigned i=0;i<8;++i) {
        RspJitRun run{0,10000,32,0};
        jit.run(view,imem,run);
        CHECK(run.retired==32);
    }
    CHECK(jit.available());
    CHECK(jit.stats().invalidations>0);
    pc=0xFFC; next=0;
    RspJitRun last{0,100,32,0};
    jit.run(view,imem,last);
    CHECK(last.retired==1);
    CHECK(pc==0);
    CHECK(next==4);
    RspJit too_small(false,1);
    pc=0; next=4;
    last={0,100,32,0};
    too_small.run(view,imem,last);
    CHECK(last.retired==0);
    CHECK(pc==0);
    CHECK_FALSE(too_small.available());
}
#endif
