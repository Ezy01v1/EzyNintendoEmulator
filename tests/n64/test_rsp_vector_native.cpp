#include <doctest/doctest.h>
#include "n64/rsp_jit.hpp"
#include "n64/rsp_vector.hpp"
#include <array>
#include <cstdint>

#if EZY_N64_RSP_JIT
TEST_CASE("RSP native vector transfers preserve byte order truncation and both wrap boundaries") {
    using namespace ezy::n64;
    std::array<std::uint8_t,4096> imem{},dmem{};
    std::array<std::uint8_t,512> vectors{};
    std::array<std::uint32_t,32> gpr{};
    std::uint32_t pc=0,next=4;
    bool phase=false;
    RspJit jit;
    REQUIRE(jit.available());
    RspJitView view{gpr.data(),dmem.data(),&pc,&next,&phase,nullptr,nullptr,vectors.data()};
    constexpr std::array<std::uint32_t,8> bases{0,1,7,8,15,0xFFD,0xFFF,0x80000FFF};
    for(bool store : {false,true}) for(unsigned op=0;op<4;++op)
        for(unsigned element=0;element<16;++element) for(int offset : {-64,-1,0,63}) {
            const auto word=((store?0x3AU:0x32U)<<26)|(1U<<21)|(5U<<16)|(op<<11)|(element<<7)|
                            (static_cast<unsigned>(offset)&127);
            for(unsigned byte=0;byte<4;++byte) imem[byte]=static_cast<std::uint8_t>(word>>(24-byte*8));
            imem[7]=13; // BREAK terminates the native block after this transfer.
            jit.invalidate();
            for(auto base : bases) {
                CAPTURE(store); CAPTURE(op); CAPTURE(element); CAPTURE(offset); CAPTURE(base);
                for(unsigned i=0;i<dmem.size();++i) dmem[i]=static_cast<std::uint8_t>(i*13+7);
                for(unsigned i=0;i<vectors.size();++i) vectors[i]=static_cast<std::uint8_t>(i*29+3);
                auto expected_memory=dmem;
                auto expected_vectors=vectors;
                const unsigned width=1U<<op;
                const auto address=base+static_cast<std::uint32_t>(offset*static_cast<int>(width));
                for(unsigned i=0;i<width && (store || element+i<16);++i) {
                    // Big-endian vector bytes reside in little-endian uint16 host lanes.
                    const auto vbyte=5*16+(((element+i)&15)^1);
                    const auto mbyte=(address+i)&4095;
                    if(store) expected_memory[mbyte]=vectors[vbyte];
                    else expected_vectors[vbyte]=dmem[mbyte];
                }
                pc=0; next=4; phase=(base&1)!=0;
                const bool initial_phase=phase;
                gpr={}; gpr[1]=base;
                RspJitRun run{0,100,1,0};
                jit.run(view,imem,run);
                REQUIRE(run.retired==1);
                CHECK(run.when==(initial_phase?1U:2U));
                CHECK(phase!=initial_phase);
                CHECK(pc==4);
                CHECK(next==8);
                CHECK(gpr[1]==base);
                CHECK(dmem==expected_memory);
                CHECK(vectors==expected_vectors);
            }
        }
    CHECK(jit.stats().native_instructions==4096);
}

TEST_CASE("RSP vector memory keeps the helper fallback when a native register view is absent") {
    using namespace ezy::n64;
    std::array<std::uint8_t,4096> imem{};
    struct Context {
        RspVector vector;
        std::array<std::uint8_t,4096> dmem{};
    } context;
    context.dmem[0]=0x12; context.dmem[1]=0x34;
    std::array<std::uint32_t,32> gpr{};
    std::uint32_t pc=0,next=4;
    bool phase=false;
    const std::uint32_t word=(0x32U<<26)|(1U<<11); // LSV
    for(unsigned byte=0;byte<4;++byte) imem[byte]=static_cast<std::uint8_t>(word>>(24-byte*8));
    imem[7]=13;
    const auto helper=+[](void* opaque,std::uint32_t instruction) noexcept {
        auto& state=*static_cast<Context*>(opaque);
        state.vector.access_memory(instruction,0,state.dmem);
    };
    RspJit jit;
    RspJitView view{gpr.data(),context.dmem.data(),&pc,&next,&phase,&context,helper};
    RspJitRun run{0,100,1,0};
    jit.run(view,imem,run);
    CHECK(run.retired==1);
    context.vector.access_memory((0x3AU<<26)|(1U<<11),16,context.dmem); // SSV v0
    CHECK(context.dmem[16]==0x12);
    CHECK(context.dmem[17]==0x34);
    CHECK(pc==4);
}

TEST_CASE("RSP native vector memory uses the current register view on a cache hit") {
    using namespace ezy::n64;
    std::array<std::uint8_t,4096> imem{},dmem{};
    std::array<std::uint16_t,256> first{},second{};
    std::array<std::uint32_t,32> gpr{};
    std::uint32_t pc=0,next=4;
    bool phase=false;
    const std::uint32_t word=(0x32U<<26)|(1U<<21)|(31U<<16)|(3U<<11)|(15U<<7); // LDV v31[15]
    for(unsigned byte=0;byte<4;++byte) imem[byte]=static_cast<std::uint8_t>(word>>(24-byte*8));
    imem[7]=13;
    gpr[1]=4095; dmem[4095]=0xAB;
    first.fill(0x1234); second.fill(0x5678);
    RspJit jit;
    RspJitView view{gpr.data(),dmem.data(),&pc,&next,&phase,nullptr,nullptr,first.data()};
    RspJitRun run{0,100,1,0};
    jit.run(view,imem,run);
    REQUIRE(run.retired==1);
    CHECK(first[255]==0x12AB);
    CHECK(first[254]==0x1234);
    pc=0; next=4; view.vpr=second.data();
    run={0,100,1,0};
    jit.run(view,imem,run);
    REQUIRE(run.retired==1);
    CHECK(second[255]==0x56AB);
    CHECK(second[254]==0x5678);
    CHECK(first[255]==0x12AB);
    CHECK(jit.stats().compiled_blocks==1);
}
#endif
