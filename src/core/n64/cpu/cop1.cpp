// COP1: the VR4300 floating-point unit.
//
// Arithmetic runs on the host's IEEE float/double with the rounding mode taken from
// FCR31.RM, and the host's exception flags become FCR31's cause bits. What the VR4300 does
// differently from a plain IEEE unit (VR4300 manual, chapter 7, "FPU Exceptions"):
//  - NaN or denormal operands of arithmetic, and denormal results unless FCR31.FS flushes
//    them, raise the unimplemented operation exception (cause bit E). E is always enabled.
//    Compares do handle NaNs (PeterLemon FPUCompare), with the pre-2008 MIPS encoding:
//    the top fraction bit *set* means signalling, so the default NaN is quiet.
//  - Converting NaN, infinity or an out-of-range value to an integer is unimplemented too.
//  - An enabled exception leaves the destination untouched and doesn't set the flags.
//  - A NaN result is the VR4300's default NaN (0x7FBFFFFF / 0x7FF7FFFFFFFFFFFF).
#include "n64/cpu/cop1.hpp"

#include <bit>
#include <cfenv>
#include <cmath>

#include "n64/cpu/vr4300.hpp"

#if defined(_MSC_VER)
#pragma fenv_access(on)
#endif

namespace ezy::n64 {

std::uint32_t Cop1::read32(int reg, bool fr) const {
    if (fr) return static_cast<std::uint32_t>(fpr_[static_cast<std::size_t>(reg)]);
    const std::uint64_t pair = fpr_[static_cast<std::size_t>(reg & ~1)];
    return static_cast<std::uint32_t>((reg & 1) ? pair >> 32 : pair);
}

void Cop1::write32(int reg, std::uint32_t value, bool fr) {
    if (fr) {
        std::uint64_t& r = fpr_[static_cast<std::size_t>(reg)];
        r = (r & 0xFFFFFFFF00000000ULL) | value;
        return;
    }
    std::uint64_t& pair = fpr_[static_cast<std::size_t>(reg & ~1)];
    pair = (reg & 1) ? (pair & 0xFFFFFFFFULL) | (static_cast<std::uint64_t>(value) << 32)
                     : (pair & 0xFFFFFFFF00000000ULL) | value;
}

std::uint64_t Cop1::read64(int reg, bool fr) const { return fpr_[static_cast<std::size_t>(fr ? reg : reg & ~1)]; }

void Cop1::write64(int reg, std::uint64_t value, bool fr) { fpr_[static_cast<std::size_t>(fr ? reg : reg & ~1)] = value; }

namespace {

// Cause/flag/enable bits, in FCR31 field order.
constexpr std::uint32_t kI = 1, kU = 2, kO = 4, kZ = 8, kV = 16, kE = 32;

constexpr std::uint32_t kDefaultNanS = 0x7FBFFFFF;
constexpr std::uint64_t kDefaultNanD = 0x7FF7FFFFFFFFFFFFULL;

// Extra cycles (VR4300 manual, "FPU Instruction Execution Cycles": latency - 1).
constexpr int kAddExtra = 2, kMulSExtra = 4, kMulDExtra = 7, kDivSExtra = 28, kDivDExtra = 57, kCvtExtra = 4;

int host_rounding(std::uint32_t rm) {
    switch (rm & 3) {
        case 1: return FE_TOWARDZERO;
        case 2: return FE_UPWARD;
        case 3: return FE_DOWNWARD;
        default: return FE_TONEAREST;
    }
}

std::uint32_t host_cause() {
    const int e = std::fetestexcept(FE_ALL_EXCEPT);
    std::uint32_t c = 0;
    if (e & FE_INEXACT) c |= kI;
    if (e & FE_UNDERFLOW) c |= kU;
    if (e & FE_OVERFLOW) c |= kO;
    if (e & FE_DIVBYZERO) c |= kZ;
    if (e & FE_INVALID) c |= kV;
    return c;
}

// Runs fn under the guest rounding mode and returns its value plus the host flags.
template <typename F>
auto with_rounding(std::uint32_t rm, std::uint32_t& cause, F fn) {
    std::fesetround(host_rounding(rm));
    std::feclearexcept(FE_ALL_EXCEPT);
    const auto result = fn();
    cause |= host_cause();
    std::fesetround(FE_TONEAREST);
    return result;
}

template <typename T>
bool is_denormal(T v) { return std::fpclassify(v) == FP_SUBNORMAL; }

// MIPS (pre-2008) signalling NaN: the top fraction bit is set.
bool is_snan(float v) { return std::isnan(v) && (std::bit_cast<std::uint32_t>(v) & 0x00400000) != 0; }
bool is_snan(double v) { return std::isnan(v) && (std::bit_cast<std::uint64_t>(v) & 0x0008000000000000ULL) != 0; }

// Operand check shared by arithmetic and conversions: NaN or denormal -> E.
template <typename T>
std::uint32_t check_operand(T v) {
    return (is_denormal(v) || std::isnan(v)) ? kE : 0;
}

// Result fix-ups: NaN -> default NaN, denormal -> flush (FS) or E.
template <typename T>
T fix_result(T v, std::uint32_t& cause, bool flush) {
    if (std::isnan(v)) {
        if constexpr (sizeof(T) == 4) return std::bit_cast<float>(kDefaultNanS);
        else return std::bit_cast<double>(kDefaultNanD);
    }
    if (is_denormal(v)) {
        if (!flush) {
            cause |= kE;
            return v;
        }
        cause |= kU | kI;
        return std::signbit(v) ? T(-0.0) : T(0.0);
    }
    return v;
}

}  // namespace

// COP1 instructions (op 0x11). The loads/stores (LWC1 etc.) are in vr4300.cpp.
void Vr4300::execute_cop1(std::uint32_t instr) {
    const bool fr = (cop0_.status() & Cop0::kFR) != 0;
    const int fmt = static_cast<int>((instr >> 21) & 31);
    const int ft = static_cast<int>((instr >> 16) & 31);
    const int fs = static_cast<int>((instr >> 11) & 31);
    const int fd = static_cast<int>((instr >> 6) & 31);
    std::uint32_t& fcr31 = cop1_.fcr31_ref();

    switch (fmt) {
        case 0x00: set_gpr(ft, static_cast<std::uint64_t>(static_cast<std::int32_t>(cop1_.read32(fs, fr)))); return;  // MFC1
        case 0x01: if (require_64bit()) set_gpr(ft, cop1_.read64(fs, fr)); return;  // DMFC1
        case 0x02: set_gpr(ft, static_cast<std::uint64_t>(static_cast<std::int32_t>(fs == 31 ? fcr31 : fs == 0 ? Cop1::kFcr0 : 0))); return;  // CFC1
        case 0x04: cop1_.write32(fs, static_cast<std::uint32_t>(gpr(ft)), fr); return;  // MTC1
        case 0x05: if (require_64bit()) cop1_.write64(fs, gpr(ft), fr); return;          // DMTC1
        case 0x06:  // CTC1
            if (fs != 31) return;
            fcr31 = static_cast<std::uint32_t>(gpr(ft)) & Cop1::kWriteMask;
            {
                const std::uint32_t cause = (fcr31 >> Cop1::kCauseShift) & 0x3F;
                const std::uint32_t enabled = ((fcr31 >> Cop1::kEnableShift) & 0x1F) | kE;
                if (cause & enabled) raise(ExceptionCode::FPE);
            }
            return;
        case 0x03:  // DCFC1, DCTC1: not implemented by the VR4300
        case 0x07:
            fcr31 = (fcr31 & ~(0x3FU << Cop1::kCauseShift)) | (kE << Cop1::kCauseShift);
            raise(ExceptionCode::FPE);
            return;
        case 0x08: {  // BC1F, BC1T, BC1FL, BC1TL
            const bool condition = (fcr31 & Cop1::kCondition) != 0;
            const bool want_true = (ft & 1) != 0;
            const bool likely = (ft & 2) != 0;
            const std::uint64_t target = pc_ + (static_cast<std::uint64_t>(static_cast<std::int64_t>(static_cast<std::int16_t>(instr & 0xFFFF))) << 2);
            branch(condition == want_true, target, likely);
            return;
        }
        default: break;
    }

    // Arithmetic, compares and conversions address the registers differently from the
    // moves (n64-systemtest "full_vs_half_mode"): in FR=0 mode a source is read from the even
    // register of its pair, but the result goes to the exact destination register, and a
    // 32-bit result clears its upper half.
    const auto src32 = [&](int reg, bool full) { return static_cast<std::uint32_t>(cop1_.fpr_bits(full ? reg : reg & ~1)); };
    const auto src64 = [&](int reg, bool full) { return cop1_.fpr_bits(full ? reg : reg & ~1); };
    const auto dst32 = [&](int reg, std::uint32_t value, bool) { cop1_.set_fpr_bits(reg, value); };
    const auto dst64 = [&](int reg, std::uint64_t value, bool) { cop1_.set_fpr_bits(reg, value); };

    // Arithmetic. Every FPU operation starts by clearing the cause bits.
    fcr31 &= ~(0x3FU << Cop1::kCauseShift);
    std::uint32_t cause = 0;
    const std::uint32_t rm = fcr31 & Cop1::kRoundingMask;
    const bool flush = (fcr31 & Cop1::kFlushDenormals) != 0;
    const int funct = static_cast<int>(instr & 0x3F);

    // Commits the cause: an enabled exception (E is always enabled) traps and leaves fd as
    // it was; otherwise the flags accumulate and the result is written.
    const auto commit = [&]() -> bool {
        fcr31 |= (cause & 0x3F) << Cop1::kCauseShift;
        const std::uint32_t enabled = ((fcr31 >> Cop1::kEnableShift) & 0x1F) | kE;
        if (cause & enabled) {
            raise(ExceptionCode::FPE);
            return false;
        }
        fcr31 |= (cause & 0x1F) << Cop1::kFlagShift;
        return true;
    };
    const auto unimplemented = [&] {
        cause |= kE;
        commit();
    };

    const bool is_s = fmt == 0x10, is_d = fmt == 0x11, is_w = fmt == 0x14, is_l = fmt == 0x15;
    if (!(is_s || is_d || is_w || is_l)) {
        raise(ExceptionCode::RI);
        return;
    }

    // Compare: C.cond.fmt (funct 0x30-0x3F).
    if (funct >= 0x30) {
        if (!(is_s || is_d)) return unimplemented();
        bool unordered, less, equal, snan;
        if (is_s) {
            const float a = std::bit_cast<float>(src32(fs, fr)), b = std::bit_cast<float>(src32(ft, fr));
            if (is_denormal(a) || is_denormal(b)) return unimplemented();
            unordered = std::isnan(a) || std::isnan(b), less = a < b, equal = a == b, snan = is_snan(a) || is_snan(b);
        } else {
            const double a = std::bit_cast<double>(src64(fs, fr)), b = std::bit_cast<double>(src64(ft, fr));
            if (is_denormal(a) || is_denormal(b)) return unimplemented();
            unordered = std::isnan(a) || std::isnan(b), less = a < b, equal = a == b, snan = is_snan(a) || is_snan(b);
        }
        // Conditions 8-15 are the signalling ones: any NaN is invalid.
        const int cond = funct & 15;
        if (snan || (unordered && (cond & 8))) cause |= kV;
        const bool result = (less && (cond & 4)) || (equal && (cond & 2)) || (unordered && (cond & 1));
        if (!commit()) return;
        fcr31 = result ? (fcr31 | Cop1::kCondition) : (fcr31 & ~Cop1::kCondition);
        return;
    }

    // MOV: a plain copy, no checks.
    if (funct == 0x06) {
        if (is_s) dst32(fd, src32(fs, fr), fr);
        else if (is_d) dst64(fd, src64(fs, fr), fr);
        else unimplemented();
        return;
    }

    // Integer formats only convert to S/D.
    if (is_w || is_l) {
        if (funct != 0x20 && funct != 0x21) return unimplemented();
        if (is_l) {
            const auto v = static_cast<std::int64_t>(src64(fs, fr));
            if (v >= (std::int64_t{1} << 55) || v < -(std::int64_t{1} << 55)) return unimplemented();
        }
        cycles_ += kCvtExtra;
        if (funct == 0x20) {  // CVT.S.W / CVT.S.L
            const float r = with_rounding(rm, cause, [&] {
                return is_w ? static_cast<float>(static_cast<std::int32_t>(src32(fs, fr)))
                            : static_cast<float>(static_cast<std::int64_t>(src64(fs, fr)));
            });
            if (commit()) dst32(fd, std::bit_cast<std::uint32_t>(r), fr);
        } else {  // CVT.D.W / CVT.D.L
            const double r = with_rounding(rm, cause, [&] {
                return is_w ? static_cast<double>(static_cast<std::int32_t>(src32(fs, fr)))
                            : static_cast<double>(static_cast<std::int64_t>(src64(fs, fr)));
            });
            if (commit()) dst64(fd, std::bit_cast<std::uint64_t>(r), fr);
        }
        return;
    }

    // From here the source is S or D. Read it as a double for the conversions.
    const double src = is_s ? static_cast<double>(std::bit_cast<float>(src32(fs, fr)))
                            : std::bit_cast<double>(src64(fs, fr));

    // Float -> integer: ROUND/TRUNC/CEIL/FLOOR.L (0x08-0x0B), .W (0x0C-0x0F), CVT.W (0x24),
    // CVT.L (0x25).
    if ((funct >= 0x08 && funct <= 0x0F) || funct == 0x24 || funct == 0x25) {
        const bool to_l = funct == 0x25 || (funct >= 0x08 && funct <= 0x0B);
        const std::uint32_t mode = (funct == 0x24 || funct == 0x25) ? rm : static_cast<std::uint32_t>(funct & 3);
        // Mode numbering: ROUND = nearest (0), TRUNC = zero (1), CEIL = +inf (2), FLOOR = -inf (3).
        if (std::isnan(src) || std::isinf(src) || (is_s ? is_denormal(static_cast<float>(src)) : is_denormal(src))) {
            return unimplemented();
        }
        cycles_ += kCvtExtra;
        double rounded = 0;
        switch (mode) {
            case 0: rounded = std::nearbyint(src); break;  // the default (nearest even) host mode
            case 1: rounded = std::trunc(src); break;
            case 2: rounded = std::ceil(src); break;
            default: rounded = std::floor(src); break;
        }
        const double limit = to_l ? 9007199254740992.0 : 2147483648.0;  // 2^53 / 2^31
        if (to_l ? (rounded >= limit || rounded <= -limit) : (rounded >= limit || rounded < -limit)) {
            return unimplemented();
        }
        if (rounded != src) cause |= kI;
        if (!commit()) return;
        if (to_l) dst64(fd, static_cast<std::uint64_t>(static_cast<std::int64_t>(rounded)), fr);
        else dst32(fd, static_cast<std::uint32_t>(static_cast<std::int32_t>(rounded)), fr);
        return;
    }

    if (funct == 0x20 || funct == 0x21) {  // CVT.S.D / CVT.D.S
        if ((funct == 0x20) == is_s) return unimplemented();  // CVT.S.S / CVT.D.D
        cause |= is_s ? check_operand(std::bit_cast<float>(src32(fs, fr))) : check_operand(src);
        if (cause & kE) return unimplemented();
        cycles_ += kCvtExtra;
        if (funct == 0x20) {
            float r = with_rounding(rm, cause, [&] { return static_cast<float>(src); });
            r = fix_result(r, cause, flush);
            if (commit()) dst32(fd, std::bit_cast<std::uint32_t>(r), fr);
        } else {
            double r = fix_result(src, cause, flush);
            if (commit()) dst64(fd, std::bit_cast<std::uint64_t>(r), fr);
        }
        return;
    }

    if (funct > 0x07) return unimplemented();

    // ADD SUB MUL DIV SQRT ABS (MOV above) NEG, in S or D.
    const auto arith = [&]<typename T>(T a, T b) -> T {
        const bool binary = funct <= 0x03;
        cause |= check_operand(a);
        if (binary) cause |= check_operand(b);
        if (cause & kE) return a;
        T r = with_rounding(rm, cause, [&]() -> T {
            switch (funct) {
                case 0x00: return a + b;
                case 0x01: return a - b;
                case 0x02: return a * b;
                case 0x03: return a / b;
                case 0x04: return std::sqrt(a);
                case 0x05: return std::fabs(a);
                default: return -a;  // NEG
            }
        });
        return fix_result(r, cause, flush);
    };
    switch (funct) {
        case 0x00: case 0x01: cycles_ += kAddExtra; break;
        case 0x02: cycles_ += is_s ? kMulSExtra : kMulDExtra; break;
        case 0x03: case 0x04: cycles_ += is_s ? kDivSExtra : kDivDExtra; break;
        default: break;
    }
    if (is_s) {
        const float r = arith(std::bit_cast<float>(src32(fs, fr)), std::bit_cast<float>(src32(ft, fr)));
        if (commit()) dst32(fd, std::bit_cast<std::uint32_t>(r), fr);
    } else {
        const double r = arith(std::bit_cast<double>(src64(fs, fr)), std::bit_cast<double>(src64(ft, fr)));
        if (commit()) dst64(fd, std::bit_cast<std::uint64_t>(r), fr);
    }
}

}  // namespace ezy::n64
