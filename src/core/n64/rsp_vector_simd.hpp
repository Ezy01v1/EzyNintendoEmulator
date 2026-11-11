#pragma once
#include <array>
#include <cstdint>

namespace ezy::n64::detail {
using VectorLanes=std::array<std::uint16_t,8>;
using AccumulatorLanes=std::array<std::int64_t,8>;

// Includes build policy and OS support; the baseline caller checks this first.
[[nodiscard]] bool vmadn_avx2_available() noexcept;
#if EZY_N64_VECTOR_AVX2
// lhs/rhs may alias dest; accumulator is a distinct bank. Requires availability.
void vmadn_avx2(const VectorLanes& lhs,const VectorLanes& rhs,
                AccumulatorLanes& accumulator,VectorLanes& dest) noexcept;
#endif
}
