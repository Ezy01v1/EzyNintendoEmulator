#pragma once

#include <array>
#include <cstdint>
#include <span>
#include <utility>

namespace ezy::n64 {

class RspVector {
public:
    void execute(std::uint32_t word, std::uint32_t& scalar);
    void access_memory(std::uint32_t word, std::uint32_t base, std::span<std::uint8_t, 0x1000> dmem);

    template <class Ar>
    void serialize(Ar& ar) {
        ar(registers_), ar(accumulator_), ar(vco_), ar(vcc_), ar(vce_);
        ar(div_input_), ar(div_output_), ar(div_high_pending_);
        if constexpr (Ar::kLoading) {
            for (const auto lane : accumulator_) {
                if (lane < -(1LL << 47) || lane >= (1LL << 47)) ar.fail();
            }
        }
    }

private:
    using Vector = std::array<std::uint16_t, 8>;

    static std::uint8_t byte(const Vector& vector, unsigned index);
    static void set_byte(Vector& vector, unsigned index, std::uint8_t value);
    void set_accumulator_low(unsigned lane, std::uint16_t value);
    void execute_alu(std::uint32_t word);
    template <unsigned op>
    std::uint16_t multiply(unsigned source, unsigned lane, std::uint16_t lhs, std::uint16_t rhs);
    template <unsigned op>
    std::uint16_t alu_lane(unsigned lane, std::uint16_t lhs, std::uint16_t rhs);
    template <unsigned op>
    void run_lanes(Vector& dest, const Vector& vs, const Vector& selected, unsigned source);
    using LaneOp = void (RspVector::*)(Vector&, const Vector&, const Vector&, unsigned);
    template <std::size_t... ops>
    static constexpr std::array<LaneOp, 64> lane_ops(std::index_sequence<ops...>);
    static const std::array<LaneOp, 64> kLaneOps;
    std::uint16_t clip_lane(unsigned op, unsigned lane, std::uint16_t lhs, std::uint16_t rhs);
    void divide(unsigned op, Vector& dest, unsigned lane, std::uint16_t input);

    std::array<Vector, 32> registers_{};
    std::array<std::int64_t, 8> accumulator_{};  // signed 48-bit accumulator per lane
    std::uint16_t vco_ = 0, vcc_ = 0;
    std::uint8_t vce_ = 0;
    std::uint16_t div_input_ = 0, div_output_ = 0;
    bool div_high_pending_ = false;
};

}  // namespace ezy::n64
