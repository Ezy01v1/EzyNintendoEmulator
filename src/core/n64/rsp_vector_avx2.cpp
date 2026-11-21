#include "n64/rsp_vector_simd.hpp"
#include "n64/rsp_vector.hpp"
#include <immintrin.h>

namespace ezy::n64::detail {
void vmadn_avx2(const VectorLanes& lhs,const VectorLanes& rhs,
                AccumulatorLanes& accumulator,VectorLanes& dest) noexcept {
    // Snapshot both inputs before any destination writes, including aliased VD.
    const auto a=_mm256_cvtepu16_epi32(_mm_loadu_si128(reinterpret_cast<const __m128i*>(lhs.data())));
    const auto b=_mm256_cvtepi16_epi32(_mm_loadu_si128(reinterpret_cast<const __m128i*>(rhs.data())));
    const auto products=_mm256_mullo_epi32(a,b); // unsigned16 * signed16 fits signed32
    const __m256i addends[2]{
        _mm256_cvtepi32_epi64(_mm256_castsi256_si128(products)),
        _mm256_cvtepi32_epi64(_mm256_extracti128_si256(products,1))};
    const auto even_words=_mm256_setr_epi32(0,2,4,6,0,2,4,6);
    __m128i middles[2],lows[2];
    for(unsigned half=0;half<2;++half) {
        const auto old=_mm256_loadu_si256(reinterpret_cast<const __m256i*>(accumulator.data()+half*4));
        const auto sum=_mm256_add_epi64(old,addends[half]);
        const auto shifted=_mm256_slli_epi64(sum,16);
        const auto sign=_mm256_shuffle_epi32(_mm256_srai_epi32(shifted,31),0xF5);
        const auto wrapped=_mm256_or_si256(_mm256_srli_epi64(shifted,16),_mm256_slli_epi64(sign,48));
        _mm256_storeu_si256(reinterpret_cast<__m256i*>(accumulator.data()+half*4),wrapped);
        // Bits 16..47 form the signed middle after the modulo-48 addition.
        middles[half]=_mm256_castsi256_si128(_mm256_permutevar8x32_epi32(_mm256_srli_epi64(sum,16),even_words));
        const auto signed_lows=_mm256_srai_epi32(_mm256_slli_epi32(sum,16),16);
        lows[half]=_mm256_castsi256_si128(_mm256_permutevar8x32_epi32(signed_lows,even_words));
    }
    const auto middle=_mm256_inserti128_si256(_mm256_castsi128_si256(middles[0]),middles[1],1);
    const auto low=_mm256_inserti128_si256(_mm256_castsi128_si256(lows[0]),lows[1],1);
    const auto above=_mm256_cmpgt_epi32(middle,_mm256_set1_epi32(32767));
    const auto below=_mm256_cmpgt_epi32(_mm256_set1_epi32(-32768),middle);
    const auto outside=_mm256_or_si256(above,below);
    const auto result=_mm256_or_si256(_mm256_andnot_si256(outside,low),above);
    const auto packed=_mm256_packs_epi32(result,result);
    const auto lanes=_mm_unpacklo_epi64(_mm256_castsi256_si128(packed),_mm256_extracti128_si256(packed,1));
    _mm_storeu_si128(reinterpret_cast<__m128i*>(dest.data()),lanes);
    _mm256_zeroupper();
}
}

namespace ezy::n64 {
void RspVector::run_vmadn_avx2(Vector& dest,const Vector& vs,const Vector& selected,unsigned) {
    detail::vmadn_avx2(vs,selected,accumulator_,dest);
}
}
