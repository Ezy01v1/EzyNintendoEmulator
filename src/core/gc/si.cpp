#include "gc/si.hpp"

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

#include "gc/gekko/gekko.hpp"
#include "gc/memory.hpp"
#include "gc/pi.hpp"
#include "gc/scheduler.hpp"
#include "gc/vi.hpp"

namespace ezy::gc {

namespace {
constexpr std::uint32_t kBase = Memory::mmio_block_base(MmioBlock::Si);
constexpr std::uint64_t kCyclesPerLine = 2 * Vi::kCyclesPerHalfLine;

// SICOMCSR bits (Dolphin's USIComCSR).
constexpr std::uint32_t kTstart = 1u << 0, kRdstIntMask = 1u << 27, kRdstInt = 1u << 28, kComErr = 1u << 29,
                        kTcintMask = 1u << 30, kTcint = 1u << 31;
constexpr std::uint32_t kComCsrStored = (3u << 1) | (0x7Fu << 8) | (0x7Fu << 16) | kRdstIntMask | kTcintMask;
// SISR (USIStatusReg): channel n's bits start at 24 - 8 n.
constexpr std::uint32_t kWr = 1u << 31;
constexpr unsigned shift(unsigned channel) { return 24 - 8 * channel; }
constexpr std::uint32_t kErrorBits = 0x0F;  // UNRUN, OVRUN, COLL, NOREP
constexpr std::uint32_t noresp(unsigned channel) { return 1u << (shift(channel) + 3); }
constexpr std::uint32_t wrst(unsigned channel) { return 1u << (shift(channel) + 4); }
constexpr std::uint32_t rdst(unsigned channel) { return 1u << (shift(channel) + 5); }
constexpr std::uint32_t kAnyRdst = rdst(0) | rdst(1) | rdst(2) | rdst(3);
// IN_HI error bits (USIChannelIn_Hi).
constexpr std::uint32_t kErrStat = 1u << 31, kErrLatch = 1u << 30;

// The standard controller's protocol (Dolphin's SI_DeviceGCController.cpp, SI_Device.h).
constexpr std::uint8_t kCmdId = 0x00, kCmdDirect = 0x40, kCmdOrigin = 0x41, kCmdRecalibrate = 0x42,
                       kCmdReset = 0xFF;
constexpr std::uint8_t kAnalogMode = 3;
constexpr std::uint16_t kUseOrigin = 0x0080;

[[noreturn]] void unmodeled(const char* what, std::uint32_t offset) {
    char b[16];
    std::snprintf(b, sizeof b, "%X", offset);
    throw BusError(std::string(what) + " of the unmodeled serial interface register at offset 0x" + b, kBase + offset);
}

[[noreturn]] void device_error(const std::string& what, std::uint32_t value, std::uint32_t offset) {
    char b[11];
    std::snprintf(b, sizeof b, "0x%02X", value);
    throw BusError("SI channel 0 (standard controller): " + what + " " + b + " is not modeled", kBase + offset);
}

bool is_buffer(std::uint32_t offset, std::uint32_t size) {
    return offset >= 0x80 && offset < 0x100 && (offset & (size - 1)) == 0;
}
}  // namespace

Si::Si(Scheduler& scheduler, Pi& pi) : sched_(scheduler), pi_(pi) {}

std::uint32_t Si::pad_hi() const {
    return std::uint32_t{pad_.stick_y} | (std::uint32_t{pad_.stick_x} << 8) |
           (std::uint32_t{static_cast<std::uint16_t>(pad_.buttons | kUseOrigin)} << 16);
}

std::uint32_t Si::pad_lo() const {  // analog mode 3
    return std::uint32_t{pad_.trigger_r} | (std::uint32_t{pad_.trigger_l} << 8) | (std::uint32_t{pad_.c_y} << 16) |
           (std::uint32_t{pad_.c_x} << 24);
}

std::uint32_t Si::buffer32(std::uint32_t at) const {
    return (std::uint32_t{buffer_[at]} << 24) | (std::uint32_t{buffer_[at + 1]} << 16) |
           (std::uint32_t{buffer_[at + 2]} << 8) | buffer_[at + 3];
}

void Si::update_irq() {
    // RDSTINT follows the channels' RDST bits (Dolphin's UpdateInterrupts).
    com_csr_ = (status_ & kAnyRdst) ? (com_csr_ | kRdstInt) : (com_csr_ & ~kRdstInt);
    const bool any = ((com_csr_ & kRdstInt) && (com_csr_ & kRdstIntMask)) ||
                     ((com_csr_ & kTcint) && (com_csr_ & kTcintMask));
    pi_.set_cause(Pi::kSi, any);
}

void Si::transfer() {
    const unsigned channel = (com_csr_ >> 1) & 3;
    const std::uint32_t out = ((com_csr_ >> 16) & 0x7F) ? (com_csr_ >> 16) & 0x7F : 128;
    const std::uint32_t in = ((com_csr_ >> 8) & 0x7F) ? (com_csr_ >> 8) & 0x7F : 128;
    com_csr_ &= ~kComErr;
    if (channel != 0) {  // no device: no response
        com_csr_ |= kComErr;
        status_ |= noresp(channel);
    } else {
        std::vector<std::uint8_t> answer;
        switch (buffer_[0]) {
            case kCmdId:
            case kCmdReset: answer = {0x09, 0x00, 0x00}; break;  // SI_GC_CONTROLLER
            case kCmdDirect: {
                if (out < 2 || buffer_[1] != kAnalogMode) {
                    device_error("direct status read in analog mode", out < 2 ? 0 : buffer_[1], 0x34);
                }
                const std::uint32_t hi = pad_hi(), lo = pad_lo();
                for (int i = 0; i < 4; ++i) answer.push_back(static_cast<std::uint8_t>(hi >> (24 - 8 * i)));
                for (int i = 0; i < 4; ++i) answer.push_back(static_cast<std::uint8_t>(lo >> (24 - 8 * i)));
                break;
            }
            case kCmdOrigin:
            case kCmdRecalibrate: answer = {0x00, 0x00, 0x80, 0x80, 0x80, 0x80, 0x00, 0x00, 0x00, 0x00}; break;
            default: device_error("command", buffer_[0], 0x34);
        }
        std::copy_n(answer.begin(), std::min<std::size_t>(answer.size(), in), buffer_.begin());
    }
    com_csr_ |= kTcint;
}

void Si::poll() {
    for (unsigned n = 0; n < 4; ++n) {
        if (!(poll_ & (0x80u >> n))) continue;  // EN0 is bit 7
        Channel& ch = channels_[n];
        if (n != 0) {  // no device (Dolphin: ErrorNoResponse)
            ch.in_hi |= kErrStat | kErrLatch;
            status_ |= noresp(n);
            continue;
        }
        if (((ch.out >> 16) & 0xFF) != kCmdDirect) {
            device_error("poll with the OUT command", (ch.out >> 16) & 0xFF, 0x00);
        }
        ch.in_hi = pad_hi();
        ch.in_lo = pad_lo();
        status_ |= rdst(0);
    }
    update_irq();
}

void Si::schedule_poll() {
    const std::uint32_t x = (poll_ >> 16) & 0x3FF;
    const std::uint64_t period = (poll_ & 0xF0) && x ? x * kCyclesPerLine : 0;
    if (period == 0) {
        sched_.cancel(EventType::SiPoll);
    } else if (!sched_.pending(EventType::SiPoll) || period != poll_period_) {
        sched_.schedule_in(EventType::SiPoll, period);
    }
    poll_period_ = period;
}

void Si::on_event(std::uint64_t when) {
    poll();
    if (poll_period_) sched_.schedule(EventType::SiPoll, when + poll_period_);  // from `when`: no drift
}

std::uint32_t Si::read32(std::uint32_t offset) {
    if (offset < 0x30 && (offset & 3) == 0) {
        Channel& ch = channels_[offset / 0x0C];
        switch (offset % 0x0C) {
            case 0x00: return ch.out;
            case 0x04:
            case 0x08:
                status_ &= ~rdst(offset / 0x0C);  // reading the input clears RDST (Dolphin)
                update_irq();
                return offset % 0x0C == 4 ? ch.in_hi : ch.in_lo;
        }
    }
    switch (offset) {
        case 0x30: return poll_;
        case 0x34: return com_csr_;
        case 0x38: return status_;
        case 0x3C: return exi_clock_lock_;
        default:
            if (is_buffer(offset, 4)) return buffer32(offset - kBufferBase);
            unmodeled("read", offset);
    }
}

void Si::write32(std::uint32_t offset, std::uint32_t value) {
    if (offset < 0x30 && (offset & 3) == 0) {
        Channel& ch = channels_[offset / 0x0C];
        switch (offset % 0x0C) {
            case 0x00:
                if (((value >> 16) & 0xFF) == kCmdDirect && ((value >> 8) & 0xFF) != kAnalogMode) {
                    device_error("poll in analog mode", (value >> 8) & 0xFF, offset);
                }
                ch.out = value;
                return;
            case 0x04: ch.in_hi = value; return;  // stored, as Dolphin
            case 0x08: ch.in_lo = value; return;
        }
    }
    switch (offset) {
        case 0x30:
            poll_ = value;
            schedule_poll();
            return;
        case 0x34:
            com_csr_ = (com_csr_ & ~kComCsrStored) | (value & kComCsrStored);
            if (value & kTcint) com_csr_ &= ~kTcint;
            if (value & kTstart) transfer();  // immediate: TSTART reads 0 again at once
            update_irq();
            return;
        case 0x38:
            for (unsigned n = 0; n < 4; ++n) status_ &= ~(value & (kErrorBits << shift(n)));
            if (value & kWr) status_ &= ~(wrst(0) | wrst(1) | wrst(2) | wrst(3));  // the copy is immediate
            update_irq();
            return;
        case 0x3C: exi_clock_lock_ = value; return;  // accepted, no effect (no 32 MHz EXI clock here)
        default:
            if (is_buffer(offset, 4)) {
                for (int i = 0; i < 4; ++i) {
                    buffer_[offset - kBufferBase + static_cast<std::uint32_t>(i)] =
                        static_cast<std::uint8_t>(value >> (24 - 8 * i));
                }
                return;
            }
            unmodeled("write", offset);
    }
}

std::uint16_t Si::read16(std::uint32_t offset) {
    if (!is_buffer(offset, 2)) unmodeled("16-bit read (the register is 32 bits)", offset);
    const std::uint32_t at = offset - kBufferBase;
    return static_cast<std::uint16_t>((buffer_[at] << 8) | buffer_[at + 1]);
}

void Si::write16(std::uint32_t offset, std::uint16_t value) {
    if (!is_buffer(offset, 2)) unmodeled("16-bit write (the register is 32 bits)", offset);
    const std::uint32_t at = offset - kBufferBase;
    buffer_[at] = static_cast<std::uint8_t>(value >> 8);
    buffer_[at + 1] = static_cast<std::uint8_t>(value);
}

}  // namespace ezy::gc
