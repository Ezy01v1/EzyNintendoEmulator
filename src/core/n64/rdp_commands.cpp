#include "n64/rdp.hpp"

#include <algorithm>
#include <atomic>
#include <system_error>
#include <thread>

namespace ezy::n64 {

namespace {

constexpr std::uint32_t kXbus = 1U << 0, kFreeze = 1U << 1, kFlush = 1U << 2, kStartClock = 1U << 3,
                        kPipeBusy = 1U << 5, kCommandBusy = 1U << 6, kDmaBusy = 1U << 8, kStartValid = 1U << 10;
constexpr std::uint32_t kAddressMask = 0xFFFFF8, kCounterMask = 0xFFFFFF;
constexpr std::uint64_t kWordCycles = 3;  // approximate command fetch and execution in CPU cycles
std::atomic<RdpMode> default_mode{RdpMode::Synchronous};

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

Rdp::~Rdp() = default; // The worker joins before the base raster state is destroyed.
RdpMode default_rdp_mode() { return default_mode.load(std::memory_order_relaxed); }
void set_default_rdp_mode(RdpMode mode) { default_mode.store(mode, std::memory_order_relaxed); }

bool Rdp::enable_threaded(std::span<std::uint8_t> rdram, unsigned bands) {
    if (worker_) return true;
    // One band per four hardware threads: more bands slow the emulation thread down (on a
    // 4-core/8-thread i7-4790, MK64's race: 1 band 34.6 fps, 2 bands 41.1, 3 39.0, 4 35.8).
    if (bands == 0) bands = std::clamp(std::thread::hardware_concurrency() / 4, 1U, 4U);
    rebuild_memory_state();
    worker_ram_ = rdram;
    bands_.clear();
    for (unsigned i = 1; i < bands; ++i) bands_.push_back(std::make_unique<RdpBand>());
    band_count_ = bands;
    sync_bands();
    std::vector<RdpWorker::Execute> executes;
    executes.emplace_back([this](const RdpPacket& packet) { execute_packet(packet, worker_ram_); });
    for (auto& band : bands_) {
        executes.emplace_back([this, raw = band.get()](const RdpPacket& packet) { raw->run(packet, worker_ram_); });
    }
    try {
        worker_ = std::make_unique<RdpWorker>(std::move(executes));
    } catch (const std::system_error&) {
        bands_.clear();  // No thread is left running; synchronous rendering remains available.
        band_count_ = 1;
        return false;
    }
    return true;
}

void Rdp::sync_bands() {
    band_index_ = 0;
    band_count_ = static_cast<unsigned>(bands_.size() + 1);
    for (unsigned i = 0; i < bands_.size(); ++i) bands_[i]->assign(*this, i + 1, band_count_);
    drawn_count_ = 0;
}

bool Rdp::needs_barrier(const RdpPacket& packet, const RdpAccess& access) {
    if (bands_.empty()) return false;
    const unsigned op = static_cast<unsigned>(packet.words[0] >> 56) & 63;
    // A new color or depth image maps addresses to other rows, so to other bands.
    if (op == 0x3E || op == 0x3F) {
        drawn_count_ = 0;
        return true;
    }
    if (op == 0x30 || op == 0x33 || op == 0x34) {
        for (unsigned i = 0; i < access.count; ++i) {
            const auto& load = access.ranges[i];
            for (unsigned d = 0; d < drawn_count_; ++d) {
                if (load.begin < drawn_[d].end && drawn_[d].begin < load.end) {
                    drawn_count_ = 0;
                    return true;
                }
            }
        }
        return false;
    }
    for (unsigned i = 0; i < access.count; ++i) {
        const auto& range = access.ranges[i];
        if (!range.write) continue;
        const auto same = std::find_if(drawn_.begin(), drawn_.begin() + drawn_count_, [&](const RdpAccess::Range& d) {
            return d.begin == range.begin && d.end == range.end;
        });
        if (same != drawn_.begin() + drawn_count_) continue;
        if (drawn_count_ < drawn_.size()) {
            drawn_[drawn_count_++] = range;
        } else {  // too many: widen the last one
            auto& last = drawn_.back();
            last.begin = std::min(last.begin, range.begin);
            last.end = std::max(last.end, range.end);
        }
    }
    return false;
}

void Rdp::rebuild_memory_state() {
    memory_state_ = {color_address_, color_width_, pixel_size_, mask_address_, scissor_, solid_.modes,
                     texture_.image};
}

void Rdp::reset_state() {
    drain();
    static_cast<RdpRasterizer&>(*this) = RdpRasterizer{};
    sync_bands();
    start_ = end_ = current_ = words_to_skip_ = 0;
    status_ = 0x80;
    clock_origin_ = last_update_ = 0;
    command_cycles_ = pipe_cycles_ = draw_words_ = 0;
    draw_command_ = {};
    rebuild_memory_state();
}

void Rdp::dispatch(const RdpPacket& packet, std::span<std::uint8_t> rdram) {
    if (worker_) {
        const auto access = memory_state_.access(packet);
        memory_state_.apply(packet.words[0]);
        worker_->submit(packet, access, needs_barrier(packet, access));
    } else {
        execute_packet(packet, rdram);
    }
}

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
            if (!xbus) prepare_memory(static_cast<std::uint32_t>(address), 8, false);
            for (unsigned i = 0; i < 8; ++i) word = (word << 8) | source[address + i];
        }
        const unsigned op = opcode(word);
        if (draw_words_ != 0 || buffered_draw_words(op) != 0) {
            draw_command_[draw_words_++] = word;
            const unsigned draw_op = opcode(draw_command_[0]);
            if (draw_words_ == buffered_draw_words(draw_op)) {
                dispatch({draw_command_, draw_words_}, rdram);
                draw_words_ = 0;
            }
        } else {
            // Preserve packet framing for unsupported commands across END extensions.
            words_to_skip_ = command_words(op) - 1;
            if (words_to_skip_ == 0) {
                if (op == 0x29) {
                    // No drain: every RDRAM observer (CPU, DMA, VI image, save state) waits
                    // for the packets it overlaps, so the CPU can run ahead of the worker.
                    status_ &= ~(kStartClock | kPipeBusy);
                    mi.raise(MiInterrupt::Dp);
                } else {
                    RdpPacket packet;
                    packet.words[0] = word;
                    packet.count = 1;
                    dispatch(packet, rdram);
                }
            }
        }
    }
    current_ += 8;
    schedule_word(scheduler, when);
}


} // namespace ezy::n64
