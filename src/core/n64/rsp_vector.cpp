#include "n64/rsp_vector.hpp"

#include <algorithm>
#include <bit>
#include <utility>

#ifdef _MSC_VER
// The lane functions are instantiated per opcode, so their op tests are constant on purpose.
#pragma warning(disable : 4127)
#endif

namespace ezy::n64 {

namespace {

enum VectorOp : unsigned {
    VMULF = 0x00, VMULU, VRNDP, VMULQ, VMUDL, VMUDM, VMUDN, VMUDH,
    VMACF, VMACU, VRNDN, VMACQ, VMADL, VMADM, VMADN, VMADH,
    VADD = 0x10, VSUB, VABS = 0x13, VADDC, VSUBC, VSAR = 0x1D,
    VLT = 0x20, VEQ, VNE, VGE, VCL, VCH, VCR, VMRG,
    VAND, VNAND, VOR, VNOR, VXOR, VNXOR,
    VRCP = 0x30, VRCPL, VRCPH, VMOV, VRSQ, VRSQL, VRSQH, VNOP, VNULL = 0x3F,
};

unsigned select_lane(unsigned element, unsigned lane) {
    if (element < 2) return lane;
    if (element < 4) return (lane & 6) | (element & 1);
    if (element < 8) return (lane & 4) | (element & 3);
    return element & 7;
}

std::int64_t signed48(std::int64_t value) {
    return static_cast<std::int64_t>(static_cast<std::uint64_t>(value) << 16) >> 16;
}

std::uint16_t clamp_signed(std::int64_t value) {
    return static_cast<std::uint16_t>(std::clamp<std::int64_t>(value, -32768, 32767));
}

void set_flag(std::uint16_t& flags, unsigned index, bool set) {
    const auto mask = static_cast<std::uint16_t>(1U << index);
    flags = static_cast<std::uint16_t>(set ? flags | mask : flags & ~mask);
}

// Generate the mantissas for the RSP's integer reciprocal and reciprocal square root.
constexpr auto reciprocal_rom() {
    std::array<std::uint16_t, 512> result{};
    result[0] = 0xFFFF;
    for (unsigned i = 1; i < result.size(); ++i) {
        result[i] = static_cast<std::uint16_t>(((1ULL << 34) / (512 + i) + 1) >> 8);
    }
    return result;
}

constexpr auto square_root_rom() {
    std::array<std::uint16_t, 512> result{};
    for (unsigned i = 0; i < result.size(); ++i) {
        const std::uint64_t divisor = (512 + i) >> (i & 1);
        unsigned low = 1U << 17, high = 1U << 18;
        while (low + 1 < high) {
            const unsigned mid = (low + high) / 2;
            if (divisor * mid * mid < (1ULL << 44)) low = mid;
            else high = mid;
        }
        result[i] = static_cast<std::uint16_t>(low >> 1);
    }
    return result;
}

std::uint32_t reciprocal(std::int32_t input, bool square_root) {
    if (input == 0) return 0x7FFFFFFF;
    if (input == -32768) return 0xFFFF0000;
    const std::uint32_t sign = input < 0 ? 0xFFFFFFFF : 0;
    std::uint32_t magnitude = static_cast<std::uint32_t>(input) ^ sign;
    if (input > -32768) magnitude -= sign;
    const unsigned leading = static_cast<unsigned>(std::countl_zero(magnitude));
    const unsigned index = ((magnitude << leading) >> 22) & 0x1FF;
    static constexpr auto reciprocals = reciprocal_rom();
    static constexpr auto roots = square_root_rom();
    const std::uint32_t fraction = square_root ? roots[(index & ~1U) | (leading & 1)] : reciprocals[index];
    const unsigned shift = square_root ? (31 - leading) / 2 : 31 - leading;
    return (((0x10000 | fraction) << 14) >> shift) ^ sign;
}

}  // namespace

std::uint8_t RspVector::byte(const Vector& vector, unsigned index) {
    return static_cast<std::uint8_t>(vector[(index & 15) / 2] >> ((index & 1) ? 0 : 8));
}

void RspVector::set_byte(Vector& vector, unsigned index, std::uint8_t value) {
    auto& lane = vector[(index & 15) / 2];
    const unsigned shift = (index & 1) ? 0 : 8;
    lane = static_cast<std::uint16_t>((lane & ~(0xFFU << shift)) | (std::uint32_t{value} << shift));
}

void RspVector::set_accumulator_low(unsigned lane, std::uint16_t value) {
    accumulator_[lane] = (accumulator_[lane] & ~0xFFFFLL) | value;
}

void RspVector::execute(std::uint32_t word, std::uint32_t& scalar) {
    if (word & (1U << 25)) {
        execute_alu(word);
        return;
    }
    const unsigned reg = (word >> 11) & 31, element = (word >> 7) & 15;
    auto& vector = registers_[reg];
    auto& flags = (reg & 3) == 0 ? vco_ : vcc_;
    switch ((word >> 21) & 15) {
        case 0: {  // MFC2 wraps the second byte at element 15.
            const auto half = static_cast<std::int16_t>((byte(vector, element) << 8) | byte(vector, element + 1));
            scalar = static_cast<std::uint32_t>(half);
            break;
        }
        case 2:  // CFC2
            scalar = (reg & 2) ? vce_ : static_cast<std::uint32_t>(static_cast<std::int16_t>(flags));
            break;
        case 4:  // MTC2 truncates at element 15.
            set_byte(vector, element, static_cast<std::uint8_t>(scalar >> 8));
            if (element != 15) set_byte(vector, element + 1, static_cast<std::uint8_t>(scalar));
            break;
        case 6:  // CTC2
            if (reg & 2) vce_ = static_cast<std::uint8_t>(scalar);
            else flags = static_cast<std::uint16_t>(scalar);
            break;
        default: break;
    }
}

void RspVector::execute_alu(std::uint32_t word) {
    const unsigned op = word & 63;
    if (op == VNOP || op == VNULL) return;
    const unsigned element = (word >> 21) & 15, source = (word >> 11) & 31;
    const auto& vt = registers_[(word >> 16) & 31];
    const auto& vs = registers_[source];
    auto& dest = registers_[(word >> 6) & 31];

    if (op == VSAR) {
        for (unsigned lane = 0; lane < dest.size(); ++lane) {
            dest[lane] = element >= 8 && element <= 10
                             ? static_cast<std::uint16_t>(accumulator_[lane] >> ((10 - element) * 16))
                             : 0;
        }
        return;
    }

    // Snapshot the selected VT lanes before writing VD, which can alias VT.
    Vector selected{};
    for (unsigned lane = 0; lane < selected.size(); ++lane) selected[lane] = vt[select_lane(element, lane)];
    if (op >= VRCP && op <= VRSQH) {
        const unsigned lane = source & 7;
        for (unsigned i = 0; i < selected.size(); ++i) set_accumulator_low(i, selected[i]);
        if (op == VMOV) dest[lane] = selected[lane];
        else divide(op, dest, lane, vt[element & 7]);
        return;
    }

    (this->*kLaneOps[op])(dest, vs, selected, source);
}

// One instantiation per opcode: the per-lane switch folds away, leaving a plain 8-lane loop.
template <unsigned op>
void RspVector::run_lanes(Vector& dest, const Vector& vs, const Vector& selected, unsigned source) {
    for (unsigned lane = 0; lane < 8; ++lane) {
        if constexpr (op <= VMADH) dest[lane] = multiply<op>(source, lane, vs[lane], selected[lane]);
        else dest[lane] = alu_lane<op>(lane, vs[lane], selected[lane]);
    }
    if constexpr (op == VADD || op == VSUB || (op >= VLT && op <= VMRG && op != VCH)) vco_ = 0;
    if constexpr (op >= VLT && op <= VGE) vcc_ &= 0xFF;
    if constexpr (op == VCL || op == VCR) vce_ = 0;
}

template <std::size_t... ops>
constexpr std::array<RspVector::LaneOp, 64> RspVector::lane_ops(std::index_sequence<ops...>) {
    return {&RspVector::run_lanes<static_cast<unsigned>(ops)>...};
}

const std::array<RspVector::LaneOp, 64> RspVector::kLaneOps = RspVector::lane_ops(std::make_index_sequence<64>{});

template <unsigned op>
std::uint16_t RspVector::multiply(unsigned source, unsigned lane, std::uint16_t lhs, std::uint16_t rhs) {
    const std::int64_t a = static_cast<std::int16_t>(lhs), b = static_cast<std::int16_t>(rhs);
    auto& acc = accumulator_[lane];
    switch (op) {
        case VMULF: case VMULU: case VMACF: case VMACU: {
            const bool accumulate = op >= VMACF;
            acc = signed48(2 * a * b + (accumulate ? acc : 0x8000));
            const auto middle = acc >> 16;
            if (op == VMULF || op == VMACF) return clamp_signed(middle);
            if (middle < 0) return 0;
            return middle > 32767 ? 0xFFFF : static_cast<std::uint16_t>(middle);
        }
        case VRNDP: case VRNDN: {
            const auto addend = (source & 1) ? b * 65536 : b;
            if ((acc < 0) == (op == VRNDN)) acc = signed48(acc + addend);
            return clamp_signed(acc >> 16);
        }
        case VMULQ: {
            auto product = a * b;
            if (product < 0) product += 31;
            acc = product * 65536;
            return clamp_signed(product >> 1) & 0xFFF0;
        }
        case VMACQ: {
            auto product = acc >> 16;
            if (!(product & 32)) {
                if (product < 0) product += 32;
                else if (product >= 32) product -= 32;
            }
            acc = signed48(product * 65536 + (acc & 0xFFFF));
            return clamp_signed(product >> 1) & 0xFFF0;
        }
        default: break;
    }

    // The four VMUD/VMAD modes differ in operand signedness and product alignment.
    const unsigned mode = op & 3;
    std::int64_t product = 0;
    switch (mode) {
        case 0: product = (std::uint32_t{lhs} * rhs) >> 16; break;
        case 1: product = a * rhs; break;
        case 2: product = lhs * b; break;
        case 3: product = a * b * 65536; break;
    }
    const bool accumulate = op >= VMADL;
    acc = signed48(product + (accumulate ? acc : 0));
    const auto middle = acc >> 16;
    if (mode == 3 || (mode == 1 && accumulate)) return clamp_signed(middle);
    if (mode == 1) return static_cast<std::uint16_t>(middle);
    if (accumulate && middle < -32768) return 0;
    if (accumulate && middle > 32767) return 0xFFFF;
    return static_cast<std::uint16_t>(acc);
}

template <unsigned op>
std::uint16_t RspVector::alu_lane(unsigned lane, std::uint16_t lhs, std::uint16_t rhs) {
    const std::int32_t a = static_cast<std::int16_t>(lhs), b = static_cast<std::int16_t>(rhs);
    const bool carry = (vco_ >> lane) & 1, unequal = (vco_ >> (lane + 8)) & 1;
    std::uint16_t result = 0, low = 0;
    switch (op) {
        case VADD: case VSUB: case VABS: {
            std::int32_t value = 0;
            if (op == VADD) value = a + b + carry;
            if (op == VSUB) value = a - b - carry;
            if (op == VABS) value = a < 0 ? -b : (a > 0 ? b : 0);
            low = static_cast<std::uint16_t>(value);
            result = clamp_signed(value);
            break;
        }
        case VADDC: case VSUBC: {
            const int value = op == VADDC ? lhs + rhs : lhs - rhs;
            set_flag(vco_, lane, value < 0 || value > 0xFFFF);
            set_flag(vco_, lane + 8, op == VSUBC && value != 0);
            result = low = static_cast<std::uint16_t>(value);
            break;
        }
        case VLT: case VEQ: case VNE: case VGE: {
            bool selected = false;
            switch (op) {
                case VLT: selected = a < b || (a == b && carry && unequal); break;
                case VEQ: selected = a == b && !unequal; break;
                case VNE: selected = a != b || unequal; break;
                case VGE: selected = a > b || (a == b && !(carry && unequal)); break;
            }
            set_flag(vcc_, lane, selected);
            result = low = selected ? lhs : rhs;
            break;
        }
        case VCL: case VCH: case VCR: result = low = clip_lane(op, lane, lhs, rhs); break;
        case VMRG: result = low = (vcc_ & (1U << lane)) ? lhs : rhs; break;
        case VAND:  result = low = lhs & rhs; break;
        case VNAND: result = low = static_cast<std::uint16_t>(~(lhs & rhs)); break;
        case VOR:   result = low = lhs | rhs; break;
        case VNOR:  result = low = static_cast<std::uint16_t>(~(lhs | rhs)); break;
        case VXOR:  result = low = lhs ^ rhs; break;
        case VNXOR: result = low = static_cast<std::uint16_t>(~(lhs ^ rhs)); break;
        default: low = static_cast<std::uint16_t>(a + b); break;  // Reserved ALU operations (VZERO).
    }
    set_accumulator_low(lane, low);
    return result;
}

std::uint16_t RspVector::clip_lane(unsigned op, unsigned lane, std::uint16_t lhs, std::uint16_t rhs) {
    const auto a = static_cast<std::int16_t>(lhs), b = static_cast<std::int16_t>(rhs);
    const unsigned mask = 1U << lane;
    bool opposite_signs = (a < 0) != (b < 0);
    bool select_low = (vcc_ & mask) != 0, select_high = (vcc_ & (mask << 8)) != 0;
    if (op == VCL) {
        opposite_signs = (vco_ & mask) != 0;
        if (!(vco_ & (mask << 8))) {
            if (opposite_signs) {
                const unsigned sum = unsigned{lhs} + rhs;
                select_low = (vce_ & mask) ? sum <= 0x10000 : sum == 0;
            } else {
                select_high = lhs >= rhs;
            }
        }
    } else {
        const int difference = opposite_signs ? a + b : a - b;
        select_low = opposite_signs ? difference < (op == VCH ? 1 : 0) : b < 0;
        select_high = opposite_signs ? b < 0 : difference >= 0;
        if (op == VCH) {
            set_flag(vco_, lane, opposite_signs);
            set_flag(vco_, lane + 8, difference != 0 && (lhs ^ rhs) != 0xFFFF);
            const auto extension = static_cast<std::uint8_t>(opposite_signs && difference == -1 ? mask : 0);
            vce_ = static_cast<std::uint8_t>((vce_ & ~mask) | extension);
        }
    }
    set_flag(vcc_, lane, select_low);
    set_flag(vcc_, lane + 8, select_high);
    if (!opposite_signs) return select_high ? rhs : lhs;
    if (!select_low) return lhs;
    return static_cast<std::uint16_t>(op == VCR ? ~rhs : -rhs);
}

void RspVector::divide(unsigned op, Vector& dest, unsigned lane, std::uint16_t input) {
    if (op == VRCPH || op == VRSQH) {
        div_input_ = input;
        div_high_pending_ = true;
        dest[lane] = div_output_;
        return;
    }
    std::int32_t operand = static_cast<std::int16_t>(input);
    if (div_high_pending_ && (op == VRCPL || op == VRSQL)) {
        operand = static_cast<std::int32_t>((std::uint32_t{div_input_} << 16) | input);
    }
    const auto result = reciprocal(operand, op == VRSQ || op == VRSQL);
    div_high_pending_ = false;
    div_output_ = static_cast<std::uint16_t>(result >> 16);
    dest[lane] = static_cast<std::uint16_t>(result);
}

}  // namespace ezy::n64
