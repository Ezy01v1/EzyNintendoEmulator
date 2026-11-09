#include "n64/gfx_hle/f3dex.hpp"

namespace ezy::n64 {

namespace {

constexpr unsigned G_ENDDL = 0xB8;
constexpr std::uint32_t kMaxCommands = 1U << 20;  // a runaway list ends the task

std::uint64_t read64(std::span<const std::uint8_t> rdram, std::uint32_t address) {
    std::uint64_t value = 0;
    for (unsigned i = 0; i < 8; ++i) value = (value << 8) | rdram[address + i];
    return value;
}

}  // namespace

F3dex::Result F3dex::run_task(const OsTask& task, GfxUcode /*ucode*/, std::span<std::uint8_t> rdram, Rdp& /*rdp*/,
                              Mi& /*mi*/) {
    std::uint32_t pc = task.data_ptr & 0xFFFFF8;
    for (std::uint32_t done = 0; done < kMaxCommands; ++done) {
        if (pc + 8 > rdram.size()) return {done, false};
        const std::uint64_t command = read64(rdram, pc);
        pc += 8;
        if ((command >> 56) == G_ENDDL) return {done + 1, true};
    }
    return {kMaxCommands, false};
}

}  // namespace ezy::n64
