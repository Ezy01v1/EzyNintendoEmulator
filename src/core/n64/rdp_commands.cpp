#include "n64/rdp.hpp"

namespace ezy::n64 {

namespace {

constexpr std::uint32_t kXbus = 1U << 0, kFreeze = 1U << 1, kFlush = 1U << 2, kStartClock = 1U << 3,
                        kPipeBusy = 1U << 5, kCommandBusy = 1U << 6, kDmaBusy = 1U << 8, kStartValid = 1U << 10;
constexpr std::uint32_t kAddressMask = 0xFFFFF8, kCounterMask = 0xFFFFFF;
constexpr std::uint64_t kWordCycles = 3;  // approximate command fetch and execution in CPU cycles

std::uint64_t rdp_cycles(std::uint64_t cpu_cycles) {
    return (cpu_cycles / 3) * 2 + (cpu_cycles % 3) * 2 / 3;
}

unsigned opcode(std::uint64_t word) { return static_cast<unsigned>(word >> 56) & 63; }

unsigned command_words(unsigned op) {
    if (op >= 0x08 && op <= 0x0F) {  // Edges plus optional shade, texture and depth coefficients.
        return 4 + ((op & 4) ? 8U : 0U) + ((op & 2) ? 8U : 0U) + ((op & 1) ? 2U : 0U);
    }
    return (op == 0x24 || op == 0x25) ? 2 : 1;  // Texture Rectangle and Texture Rectangle Flip
}

} // namespace

unsigned Rdp::buffered_draw_words(unsigned op) {
    switch (op) {
        case 0x08: case 0x09: case 0x0A: case 0x0B: case 0x0C: case 0x0D: case 0x0E: case 0x0F:
            return command_words(op);
        case 0x24: return 2;
        default: return 0;
    }
}

std::uint32_t Rdp::read(std::uint32_t offset, std::uint64_t now) const {
    const auto elapsed = static_cast<std::uint32_t>(rdp_cycles(now) - rdp_cycles(last_update_));
    switch (offset & 0x1F) {
        case 0x00: return start_;
        case 0x04: return end_;
        case 0x08: return current_;
        case 0x0C: return status_;
        case 0x10: return static_cast<std::uint32_t>(rdp_cycles(now) - rdp_cycles(clock_origin_)) & kCounterMask;
        case 0x14: return (command_cycles_ + ((status_ & kCommandBusy) ? elapsed : 0)) & kCounterMask;
        case 0x18: return (pipe_cycles_ + ((status_ & kPipeBusy) ? elapsed : 0)) & kCounterMask;
        default: return 0;  // TMEM has no active load pipeline yet.
    }
}

void Rdp::update_counters(std::uint64_t when) {
    const auto elapsed = static_cast<std::uint32_t>(rdp_cycles(when) - rdp_cycles(last_update_));
    if (status_ & kCommandBusy) command_cycles_ = (command_cycles_ + elapsed) & kCounterMask;
    if (status_ & kPipeBusy) pipe_cycles_ = (pipe_cycles_ + elapsed) & kCounterMask;
    last_update_ = when;
}

void Rdp::write(std::uint32_t offset, std::uint32_t value, Scheduler& scheduler, std::uint64_t when) {
    switch (offset & 0x1F) {
        case 0x00:  // START latches once and becomes CURRENT on the next END write.
            if (!(status_ & kStartValid)) start_ = value & kAddressMask;
            status_ |= kStartValid;
            break;
        case 0x04:
            update_counters(when);
            end_ = value & kAddressMask;
            if (status_ & kStartValid) {
                current_ = start_;
                status_ &= ~kStartValid;
            }
            if (!(status_ & (kFreeze | kFlush))) status_ |= kStartClock | kPipeBusy;
            schedule_word(scheduler, when);
            break;
        case 0x0C: {
            update_counters(when);
            const auto apply_pair = [this, value](unsigned shift, std::uint32_t flag) {
                if (value & (1U << shift)) status_ &= ~flag;
                if (value & (2U << shift)) status_ |= flag;
            };
            apply_pair(0, kXbus);
            apply_pair(2, kFreeze);
            apply_pair(4, kFlush);
            if (value & (1U << 7)) pipe_cycles_ = 0;
            if (value & (1U << 8)) command_cycles_ = 0;
            if (value & (1U << 9)) clock_origin_ = when;
            schedule_word(scheduler, when);
            break;
        }
        default: break;
    }
}

void Rdp::schedule_word(Scheduler& scheduler, std::uint64_t when) {
    if ((status_ & (kFreeze | kFlush)) || current_ >= end_) {
        status_ &= ~(kDmaBusy | kCommandBusy);
        scheduler.cancel(EventType::RdpWord);
        return;
    }
    status_ |= kStartClock | kPipeBusy | kDmaBusy | kCommandBusy;
    if (!scheduler.pending(EventType::RdpWord)) scheduler.schedule(EventType::RdpWord, when + kWordCycles);
}

void Rdp::step(Mi& mi, Scheduler& scheduler, std::span<std::uint8_t> rdram,
               std::span<const std::uint8_t, 0x1000> dmem, std::uint64_t when) {
    if ((status_ & (kFreeze | kFlush)) || current_ >= end_) return;
    update_counters(when);
    if (words_to_skip_) {
        --words_to_skip_;
    } else {
        std::uint64_t word = 0;
        const bool xbus = (status_ & kXbus) != 0;
        const std::size_t address = xbus ? current_ & 0xFF8 : current_;
        const std::span<const std::uint8_t> source = xbus ? std::span<const std::uint8_t>(dmem)
                                                          : std::span<const std::uint8_t>(rdram);
        if (address + 8 <= source.size()) {
            for (unsigned i = 0; i < 8; ++i) word = (word << 8) | source[address + i];
        }
        const unsigned op = opcode(word);
        if (draw_words_ != 0 || buffered_draw_words(op) != 0) {
            draw_command_[draw_words_++] = word;
            const unsigned draw_op = opcode(draw_command_[0]);
            if (draw_words_ == buffered_draw_words(draw_op)) {
                execute_packet({draw_command_, draw_words_}, rdram);
                draw_words_ = 0;
            }
        } else {
            // Preserve packet framing for unsupported commands across END extensions.
            words_to_skip_ = command_words(op) - 1;
            if (words_to_skip_ == 0) {
                if (op == 0x29) {
                    status_ &= ~(kStartClock | kPipeBusy);
                    mi.raise(MiInterrupt::Dp);
                } else {
                    RdpPacket packet;
                    packet.words[0] = word;
                    packet.count = 1;
                    execute_packet(packet, rdram);
                }
            }
        }
    }
    current_ += 8;
    schedule_word(scheduler, when);
}


} // namespace ezy::n64
