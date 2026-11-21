#pragma once

#include <cstdint>
#include <functional>
#include <span>

namespace ezy::n64 {

// The OSTask libultra leaves at DMEM 0xFC0 before it starts the RSP (addresses are RDRAM
// offsets; libultra may leave KSEG0 bits in them).
struct RspTask {
    static constexpr std::uint32_t kAudio = 2;
    std::uint32_t type = 0, flags = 0;
    std::uint32_t ucode = 0, ucode_size = 0, ucode_data = 0, ucode_data_size = 0;
    std::uint32_t data_ptr = 0, data_size = 0;
    static RspTask read(std::span<const std::uint8_t, 0x1000> dmem);
};

// High-level emulation of the audio microcode Mario Kart 64 ships (the "NEAD" command set,
// 32 opcodes). Runs a whole audio command list at once instead of interpreting the
// microcode; the results in RDRAM match the microcode's.
class AudioHle {
public:
    // Called before every RDRAM range the task reads or writes, so a threaded RDP can
    // finish the packets that overlap it and a checker can record what was written.
    using Access = std::function<void(std::uint32_t address, std::uint32_t bytes, bool write)>;

    // True when the task runs a microcode this class reproduces exactly.
    [[nodiscard]] static bool supports(const RspTask& task, std::span<const std::uint8_t> rdram);
    // Executes the task's command list. DMEM is used as scratch, like the microcode does.
    static void run(const RspTask& task, std::span<std::uint8_t> rdram, std::span<std::uint8_t, 0x1000> dmem,
                    const Access& access);
};

}  // namespace ezy::n64
