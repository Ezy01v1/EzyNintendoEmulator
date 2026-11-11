#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <type_traits>

namespace ezy::n64 {

struct RspJitView {
    std::uint32_t* gpr;
    std::uint8_t* dmem;
    std::uint32_t* pc;
    std::uint32_t* next_pc;
    bool* phase;
    void* opaque;
    void (*vector_helper)(void*, std::uint32_t) noexcept; // no exceptions across native frames
};
struct RspJitRun {
    std::uint64_t when, limit; // limit is exclusive
    std::uint32_t maximum, retired;
};
static_assert(std::is_standard_layout_v<RspJitView> && std::is_standard_layout_v<RspJitRun>);

class RspJit {
public:
    struct Stats {
        std::uint64_t compiled_blocks = 0, native_instructions = 0, fallback_instructions = 0;
        std::uint64_t compile_ns = 0, invalidations = 0;
        std::uint64_t reused_blocks = 0, cache_flushes = 0;
    };
    explicit RspJit(bool deny_execution = false, std::size_t code_budget = 16 * 1024 * 1024);
    ~RspJit();
    RspJit(const RspJit&) = delete;
    RspJit& operator=(const RspJit&) = delete;
    [[nodiscard]] bool available() const;
    void run(RspJitView& view, std::span<const std::uint8_t, 4096> imem, RspJitRun& run);
    void invalidate();
    void record_fallback();
    [[nodiscard]] Stats stats() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace ezy::n64
