#include <doctest/doctest.h>
#include "n64/rsp_vector_simd.hpp"
#include "n64/rsp_vector.hpp"
#include "common/state.hpp"
#include <array>
#include <cstdint>

namespace {
std::int64_t vmadn_reference(std::int64_t acc,std::uint16_t lhs,std::uint16_t rhs) {
    const auto product=static_cast<std::int64_t>(lhs)*static_cast<std::int16_t>(rhs);
    const auto bits=(static_cast<std::uint64_t>(acc)+static_cast<std::uint64_t>(product))&0xFFFFFFFFFFFFULL;
    return bits<0x800000000000ULL?static_cast<std::int64_t>(bits):static_cast<std::int64_t>(bits)-0x1000000000000LL;
}
std::uint16_t vmadn_result(std::int64_t acc) {
    if(acc < -2147483648LL) return 0;
    if(acc > 2147483647LL) return 0xFFFF;
    return static_cast<std::uint16_t>(acc);
}
}

#if EZY_N64_VECTOR_AVX2
TEST_CASE("RSP VMADN SIMD matches mixed signedness 48-bit wrap and low-slice saturation") {
    using namespace ezy::n64::detail;
    if(!vmadn_avx2_available()) { MESSAGE("AVX2 unavailable; scalar backend remains active"); return; }
    constexpr std::array<std::int64_t,12> edges{
        -140737488355328LL,140737488355327LL,-2147483649LL,-2147483648LL,
        -2147483647LL,2147483646LL,2147483647LL,2147483648LL,-65536,-1,0,65535};
    constexpr std::array<std::uint16_t,8> values{0,1,0x7FFF,0x8000,0xFFFF,0x1234,0xABCD,0xFFFE};
    std::uint32_t random=0xC0FFEE99;
    for(unsigned sample=0;sample<2048;++sample) for(unsigned alias=0;alias<3;++alias) {
        CAPTURE(sample); CAPTURE(alias);
        VectorLanes lhs{},rhs{},dest{};
        AccumulatorLanes acc{},expected_acc{};
        VectorLanes expected{};
        for(unsigned lane=0;lane<8;++lane) {
            random^=random<<13; random^=random>>17; random^=random<<5;
            lhs[lane]=sample<96?values[(sample+lane)&7]:static_cast<std::uint16_t>(random);
            rhs[lane]=sample<96?values[(sample/8+lane)&7]:static_cast<std::uint16_t>(random>>16);
            acc[lane]=sample<96?edges[(sample+lane)%edges.size()]:
                static_cast<std::int64_t>((static_cast<std::uint64_t>(random)*0x9E3779B1ULL)&0xFFFFFFFFFFFFULL)-0x800000000000LL;
            expected_acc[lane]=vmadn_reference(acc[lane],lhs[lane],rhs[lane]);
            expected[lane]=vmadn_result(expected_acc[lane]);
        }
        const auto original_lhs=lhs,original_rhs=rhs;
        auto& output=alias==1?lhs:alias==2?rhs:dest;
        vmadn_avx2(lhs,rhs,acc,output);
        REQUIRE(acc==expected_acc);
        REQUIRE(output==expected);
        if(alias!=1) CHECK(lhs==original_lhs);
        if(alias!=2) CHECK(rhs==original_rhs);
    }
}
#endif

namespace {
struct VectorImage {
    std::array<ezy::n64::detail::VectorLanes,32> registers{};
    ezy::n64::detail::AccumulatorLanes accumulator{};
    std::uint16_t vco=0xA55A,vcc=0x5AA5;
    std::uint8_t vce=0x3C;
    std::uint16_t div_input=0x1234,div_output=0xABCD;
    bool div_pending=true;
    template<class Ar> void serialize(Ar& ar) {
        ar(registers); ar(accumulator); ar(vco); ar(vcc); ar(vce);
        ar(div_input); ar(div_output); ar(div_pending);
    }
    std::vector<std::uint8_t> bytes() {
        ezy::StateWriter writer; serialize(writer); return writer.take();
    }
};
constexpr std::array<std::array<unsigned,8>,16> selections{{
    {{0,1,2,3,4,5,6,7}},{{0,1,2,3,4,5,6,7}},
    {{0,0,2,2,4,4,6,6}},{{1,1,3,3,5,5,7,7}},
    {{0,0,0,0,4,4,4,4}},{{1,1,1,1,5,5,5,5}},
    {{2,2,2,2,6,6,6,6}},{{3,3,3,3,7,7,7,7}},
    {{0,0,0,0,0,0,0,0}},{{1,1,1,1,1,1,1,1}},
    {{2,2,2,2,2,2,2,2}},{{3,3,3,3,3,3,3,3}},
    {{4,4,4,4,4,4,4,4}},{{5,5,5,5,5,5,5,5}},
    {{6,6,6,6,6,6,6,6}},{{7,7,7,7,7,7,7,7}}}};
}

TEST_CASE("RSP VMADN public dispatch preserves all selectors aliases flags and division state") {
    std::uint32_t random=0x81726354;
    constexpr std::array<std::int64_t,8> edges{
        -140737488355328LL,140737488355327LL,-2147483649LL,-2147483648LL,
        2147483647LL,2147483648LL,-1,0};
    for(unsigned sample=0;sample<64;++sample) for(unsigned element=0;element<16;++element)
        for(unsigned dest : {1U,2U,3U}) {
            CAPTURE(sample); CAPTURE(element); CAPTURE(dest);
            VectorImage initial;
            for(auto& reg:initial.registers) for(auto& lane:reg) {
                random^=random<<13; random^=random>>17; random^=random<<5;
                lane=static_cast<std::uint16_t>(random);
            }
            for(unsigned lane=0;lane<8;++lane) initial.accumulator[lane]=sample<32?
                edges[(sample+lane)&7]:static_cast<std::int64_t>((static_cast<std::uint64_t>(random)*0x9E3779B1ULL+lane)&0xFFFFFFFFFFFFULL)-0x800000000000LL;
            auto expected=initial;
            for(unsigned lane=0;lane<8;++lane) {
                expected.accumulator[lane]=vmadn_reference(initial.accumulator[lane],initial.registers[1][lane],initial.registers[2][selections[element][lane]]);
                expected.registers[dest][lane]=vmadn_result(expected.accumulator[lane]);
            }
            const auto state=initial.bytes();
            ezy::n64::RspVector vector;
            ezy::StateReader reader(state);
            vector.serialize(reader);
            REQUIRE_FALSE(reader.failed());
            REQUIRE(reader.at_end());
            std::uint32_t scalar=0xDEADBEEF;
            vector.execute((0x12U<<26)|(1U<<25)|(element<<21)|(2U<<16)|(1U<<11)|(dest<<6)|14,scalar);
            ezy::StateWriter actual;
            vector.serialize(actual);
            CHECK(actual.take()==expected.bytes());
            CHECK(scalar==0xDEADBEEF);
        }
}
