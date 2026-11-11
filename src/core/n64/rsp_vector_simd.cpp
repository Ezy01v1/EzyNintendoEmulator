#include "n64/rsp_vector_simd.hpp"

#if EZY_N64_VECTOR_AVX2 && defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#endif

namespace ezy::n64::detail {
bool vmadn_avx2_available() noexcept {
#if !EZY_N64_VECTOR_AVX2
    return false;
#elif defined(_WIN32) && defined(PF_AVX2_INSTRUCTIONS_AVAILABLE)
    static const bool available=[] {
        if(!IsProcessorFeaturePresent(PF_AVX2_INSTRUCTIONS_AVAILABLE)) return false;
        using ReadXState=DWORD64 (WINAPI*)();
        const auto kernel=GetModuleHandleW(L"kernel32.dll");
        if(!kernel) return false;
        const auto enabled=reinterpret_cast<ReadXState>(GetProcAddress(kernel,"GetEnabledXStateFeatures"));
        return enabled && (enabled()&0x6)==0x6; // SSE and YMM context state enabled by the OS
    }();
    return available;
#elif defined(__x86_64__) && (defined(__GNUC__) || defined(__clang__))
    static const bool available=[] { __builtin_cpu_init(); return __builtin_cpu_supports("avx2")!=0; }();
    return available;
#else
    return false;
#endif
}
}
