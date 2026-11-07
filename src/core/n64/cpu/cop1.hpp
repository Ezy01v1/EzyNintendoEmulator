#pragma once

#include <array>
#include <cstdint>

namespace ezy::n64 {

// The VR4300's floating-point unit registers. The instructions themselves are decoded by
// Vr4300::execute_cop1 (cop1.cpp), which needs the GPRs and the exception machinery.
//
// Register file (Status.FR): with FR = 1 there are 32 registers of 64 bits. With FR = 0
// there are 32 of 32 bits, paired even/odd for doubles: register 2n+1 is the high half of
// the 64-bit slot 2n. Storage is always 32 slots of 64 bits; the views below map them.
class Cop1 {
public:
    // FCR31 fields.
    static constexpr std::uint32_t kRoundingMask = 0x3;
    static constexpr int kFlagShift = 2, kEnableShift = 7, kCauseShift = 12;
    static constexpr std::uint32_t kCondition = 1U << 23, kFlushDenormals = 1U << 24;
    static constexpr std::uint32_t kWriteMask = 0x0183FFFF;
    static constexpr std::uint32_t kFcr0 = 0x00000A00;  // implementation/revision of the VR4300 FPU

    [[nodiscard]] std::uint32_t fcr31() const { return fcr31_; }
    [[nodiscard]] std::uint32_t& fcr31_ref() { return fcr31_; }
    [[nodiscard]] std::uint64_t fpr_bits(int index) const { return fpr_[static_cast<std::size_t>(index)]; }
    void set_fpr_bits(int index, std::uint64_t value) { fpr_[static_cast<std::size_t>(index)] = value; }

    [[nodiscard]] std::uint32_t read32(int reg, bool fr) const;
    void write32(int reg, std::uint32_t value, bool fr);
    [[nodiscard]] std::uint64_t read64(int reg, bool fr) const;
    void write64(int reg, std::uint64_t value, bool fr);

    template <class Ar>
    void serialize(Ar& ar) {
        ar(fpr_), ar(fcr31_);
    }

private:
    std::array<std::uint64_t, 32> fpr_{};
    std::uint32_t fcr31_ = 0;
};

}  // namespace ezy::n64
