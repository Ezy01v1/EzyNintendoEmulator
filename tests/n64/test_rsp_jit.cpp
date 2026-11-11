#include <doctest/doctest.h>
#include "n64/rsp_regs.hpp"
#include "n64/rsp_jit.hpp"
#include "mips_asm.hpp"
#include "common/state.hpp"
#include <vector>

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

TEST_CASE("RSP selected JIT executes and recompiles modified instruction memory") {
    using namespace ezy::n64;
    RspRegs rsp(RspMode::Jit);
    REQUIRE(rsp.jit_active());
    Mi mi;
    Rdp rdp;
    Scheduler scheduler;
    const auto program = [&](unsigned constant) {
        const std::array words{mips::addiu(1,0,static_cast<int>(constant)),mips::sw(1,0,0),mips::r(0,0,0,0,0,13)};
        for(unsigned w=0;w<words.size();++w) {
            std::array<std::uint8_t,4> bytes{};
            for(unsigned b=0;b<4;++b) bytes[b]=static_cast<std::uint8_t>(words[w]>>(24-b*8));
            rsp.write_memory(0x1000+w*4,bytes);
        }
    };
    const auto run = [&](std::uint64_t bound) {
        rsp.write_sp(0x40000,0,mi,scheduler,scheduler.now());
        rsp.write_sp(0x10,5,mi,scheduler,scheduler.now());
        scheduler.advance_to(bound);
        while(const auto due=scheduler.pop_due()) rsp.run(mi,rdp,scheduler,due->when,false);
    };
    program(17); run(100);
    CHECK(rsp.read_memory8(3)==17);
    CHECK(rsp.jit_stats().native_instructions>=2);
    const auto invalidations=rsp.jit_stats().invalidations;
    program(42); run(200);
    CHECK(rsp.read_memory8(3)==42);
    CHECK(rsp.jit_stats().invalidations>invalidations);
}

TEST_CASE("RSP JIT arithmetic aliases and wrapped loads match the interpreter") {
    using namespace ezy::n64;
    RspRegs reference(RspMode::Interpreter),native(RspMode::Jit);
    REQUIRE(native.jit_active());
    Scheduler a,b;
    Mi ma,mb;
    Rdp ra,rb;
    std::vector<std::uint32_t> words;
    std::uint32_t random=0xF00DFEED;
    for(int reg=1;reg<32;++reg) {
        random^=random<<13; random^=random>>17; random^=random<<5;
        words.push_back(mips::lui(reg,static_cast<int>(random>>16)));
        words.push_back(mips::ori(reg,reg,static_cast<int>(random&0xFFFF)));
    }
    for(int fn : {0,2,3,4,6,7,0x20,0x21,0x22,0x23,0x24,0x25,0x26,0x27,0x2A,0x2B})
        for(int dest : {0,1,2,5}) words.push_back(mips::r(0,1,2,dest,31,fn));
    for(int op=8;op<=15;++op)
        for(int immediate : {-32768,-1,0,32767}) words.push_back(mips::i(op,3,3,immediate));
    for(int offset : {-1,0,1,2,3}) {
        words.push_back(mips::addiu(1,0,0xFFD));
        words.push_back(mips::lui(2,0xABCD));
        words.push_back(mips::ori(2,2,0xEF98));
        for(int op : {0x28,0x29,0x2B,0x20,0x24,0x21,0x25,0x23,0x27})
            words.push_back(mips::i(op,1,(op>=0x28)?2:4,offset));
    }
    words.push_back(mips::r(0,0,0,0,0,13));
    std::vector<std::uint8_t> bytes;
    for(auto word:words) for(unsigned i=0;i<4;++i) bytes.push_back(static_cast<std::uint8_t>(word>>(24-8*i)));
    reference.write_memory(0x1000,bytes); native.write_memory(0x1000,bytes);
    reference.write_sp(0x10,0x101,ma,a,0); native.write_sp(0x10,0x101,mb,b,0);
    a.schedule(EventType::ViLine,17); b.schedule(EventType::ViLine,17);
    for(std::uint64_t bound=0;bound<words.size()*2+10;bound+=7) {
        CAPTURE(bound);
        a.advance_to(bound); b.advance_to(bound);
        while(auto event=a.pop_due()) if(event->type==EventType::RspStep) reference.step(ma,ra,a,event->when);
        while(auto event=b.pop_due()) if(event->type==EventType::RspStep) native.run(mb,rb,b,event->when,false);
        ezy::StateWriter expected,actual;
        reference.serialize(expected,11,ra); ma.serialize(expected);
        native.serialize(actual,11,rb); mb.serialize(actual);
        CHECK(actual.take()==expected.take());
        CHECK(a.next_event_time()==b.next_event_time());
    }
    CHECK(native.jit_stats().native_instructions==words.size()-1);
}

TEST_CASE("RSP JIT respects an already active interrupt and an equal-time event") {
    using namespace ezy::n64;
    for(bool interrupt : {false,true}) {
        RspRegs rsp(RspMode::Jit);
        Scheduler scheduler;
        Mi mi; Rdp rdp;
        const std::array<std::uint8_t,4> instruction{0x24,0x01,0x00,0x01}; // ADDIU r1,r0,1
        rsp.write_memory(0x1000,instruction);
        rsp.write_sp(0x10,1,mi,scheduler,0);
        if(interrupt) { mi.write(12,2); mi.raise(MiInterrupt::Sp); }
        else scheduler.schedule(EventType::RdpWord,2);
        scheduler.advance_to(100);
        const auto due=scheduler.pop_due(); REQUIRE(due);
        const auto stopped=rsp.run(mi,rdp,scheduler,due->when,interrupt);
        CHECK(rsp.read_sp(0x40000)==4);
        CHECK(scheduler.pending(EventType::RspStep));
        if(interrupt) { CHECK(stopped==std::optional<std::uint64_t>{2}); CHECK(rsp.jit_stats().native_instructions==1); }
        else { CHECK_FALSE(stopped); CHECK(rsp.jit_stats().native_instructions==0); }
    }
}

TEST_CASE("RSP JIT invalidation remains safe after multiple executable allocator blocks") {
    using namespace ezy::n64;
    std::array<std::uint8_t,4096> imem{},dmem{};
    std::array<std::uint32_t,32> gpr{};
    std::uint32_t pc=0,next=4;
    bool phase=false;
    RspJitView view{gpr.data(),dmem.data(),&pc,&next,&phase,nullptr,nullptr};
    RspJit jit;
    for(unsigned generation=0;generation<3;++generation) {
        for(unsigned entry=0;entry<224;++entry) {
            pc=entry*4; next=pc+4;
            RspJitRun run{0,10,1,0};
            jit.run(view,imem,run);
            REQUIRE(run.retired==1);
        }
        jit.invalidate();
    }
    CHECK(jit.stats().compiled_blocks==3*224);
}
#endif
