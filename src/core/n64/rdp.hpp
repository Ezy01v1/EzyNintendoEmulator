#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <memory>
#include <utility>
#include <vector>

#include "n64/mi.hpp"
#include "n64/rdp_rasterizer.hpp"
#include "n64/rdp_memory.hpp"
#include "n64/scheduler.hpp"

namespace ezy::n64 {

enum class RdpMode { Synchronous, Threaded };
RdpMode default_rdp_mode();
void set_default_rdp_mode(RdpMode mode);

// A copy of the raster state that draws one band of rows for the threaded RDP.
class RdpBand : public RdpRasterizer {
public:
    void assign(const RdpRasterizer& source, unsigned index, unsigned count) {
        RdpRasterizer::operator=(source);
        band_index_ = index, band_count_ = count;
    }
    void run(const RdpPacket& packet, std::span<std::uint8_t> rdram) { execute_packet(packet, rdram); }
};

// Display Processor command interface and software rasterizer.
class Rdp : private RdpRasterizer {
public:
    Rdp() { rebuild_memory_state(); }
    ~Rdp();
    // Threaded mode draws on `bands` worker threads (0 picks one per four hardware threads,
    // at most 4), each owning every band-th group of 4 rows. Results match synchronous mode.
    bool enable_threaded(std::span<std::uint8_t> rdram, unsigned bands = 0);
    [[nodiscard]] unsigned bands() const { return worker_ ? static_cast<unsigned>(worker_->consumers()) : 0; }
    void bind_memory(std::span<std::uint8_t> rdram) { drain(); worker_ram_ = rdram; }
    [[nodiscard]] bool threaded() const { return worker_ != nullptr; }
    [[nodiscard]] RdpWorker::Stats stats() const { return worker_ ? worker_->stats() : RdpWorker::Stats{}; }
    void drain() const { if (worker_) worker_->drain(); }
    // RDRAM's hidden bits (two per halfword): depth DZ low bits and 16-bit color coverage.
    // Drains the worker first, so they match the pixels drawn so far.
    [[nodiscard]] const RdpHiddenBits& hidden_bits() const { drain(); return *hidden_; }
    // The worker's barrier view (RdpWorker::barrier_view); none without a worker.
    [[nodiscard]] std::optional<jit::PageBarrierView> barrier_view() const {
        if (!worker_) return std::nullopt;
        return worker_->barrier_view();
    }
    void prepare_memory(std::uint32_t address, std::size_t bytes, bool write) const {
        if (worker_) worker_->wait_memory(address, bytes, write);  // cheap when nothing touches the range
    }
    [[nodiscard]] std::uint32_t read(std::uint32_t offset, std::uint64_t now) const;
    void write(std::uint32_t offset, std::uint32_t value, Scheduler& scheduler, std::uint64_t when);
    void step(Mi& mi, Scheduler& scheduler, std::span<std::uint8_t> rdram,
              std::span<const std::uint8_t, 0x1000> dmem, std::uint64_t when);
    // Command words that skip DPC (the graphics HLE): same framing as words fetched from
    // RDRAM or DMEM, without touching START/END/CURRENT. With defer_sync_full, a Sync Full
    // among them raises DP only at finish_deferred_sync(), when the task's drawing would end.
    void submit_words(std::span<const std::uint64_t> words, Mi& mi, std::span<std::uint8_t> rdram,
                      bool defer_sync_full = false);
    void finish_deferred_sync(Mi& mi);
    // Called with every RDRAM range [begin, end) a draw may write (color and depth image),
    // on the calling thread before the draw runs: synchronous mode executes it right after,
    // threaded mode hands it to the worker. The bounds may overestimate the pixels drawn.
    void set_ram_write_listener(std::function<void(std::uint32_t begin, std::uint32_t end)> listener) {
        ram_write_listener_ = std::move(listener);
    }
    // When set, every command word the RDP consumes is appended here (for comparing runs).
    void set_trace(std::vector<std::uint64_t>* trace) { trace_ = trace; }
    // How many 64-bit words the command with this opcode (bits 61:56) takes.
    [[nodiscard]] static unsigned words_in_command(unsigned op);

    template <class Ar>
    void serialize(Ar& ar, std::uint32_t version) {
        drain();
        ar(start_), ar(end_), ar(current_), ar(status_), ar(words_to_skip_);
        ar(clock_origin_), ar(last_update_), ar(command_cycles_), ar(pipe_cycles_);
        ar(color_address_), ar(color_width_), ar(pixel_size_), ar(fill_color_), ar(cycle_type_), ar(scissor_);
        if (version >= 5) {
            ar(draw_words_);
            const std::size_t saved_words = version >= 9 ? draw_command_.size() : version >= 8 ? 12 : 4;
            if constexpr (Ar::kLoading) draw_command_ = {};
            for (std::size_t i = 0; i < saved_words; ++i) ar(draw_command_[i]);
            if constexpr (Ar::kLoading) {
                if (draw_words_ != 0) {
                    const auto op = static_cast<unsigned>(draw_command_[0] >> 56) & 63;
                    if (draw_words_ >= buffered_draw_words(op) ||
                        (version < 8 && op == 0x0A) || (version < 7 && op == 0x24)) ar.fail();
                }
            }
        } else {
            draw_words_ = 0;
            draw_command_ = {};
        }
        if (version >= 6) solid_.serialize(ar);
        else solid_ = {};
        if (version >= 7) texture_.serialize(ar);
        else texture_ = {};
        if (version >= 9) ar(mask_address_), ar(prim_depth_);
        else mask_address_ = prim_depth_ = 0;
        // The old noise state slot now holds the primitive serial, the seed noise restarts from.
        if (version >= 10) ar(key_gb_), ar(key_r_), ar(primitive_serial_), ar(convert_), ar(prim_lod_);
        else key_gb_ = 0, key_r_ = 0, primitive_serial_ = 1, convert_ = 0, prim_lod_ = 0;
        if (version >= 11) {
            hidden_->serialize(ar);
        } else {
            hidden_->clear();
        }
        if (version >= 13) ar(pending_sync_full_);
        else pending_sync_full_ = false;
        if constexpr (Ar::kLoading) {
            rebuild_memory_state();
            sync_bands();
        }
    }

    template <class Ar>
    void load_legacy(Ar& ar) {
        std::array<std::uint32_t, 8> registers{};
        ar(registers);
        reset_state();
        start_ = registers[0] & 0xFFFFF8;
        end_ = registers[1] & 0xFFFFF8;
        current_ = registers[2] & 0xFFFFF8;
    }

private:
    void dispatch(const RdpPacket& packet, std::span<std::uint8_t> rdram);
    void reset_state();
    void rebuild_memory_state();
    // After the main thread changed the raster state (load, reset): copy it to every band.
    void sync_bands();
    [[nodiscard]] bool needs_barrier(const RdpPacket& packet, const RdpAccess& access);
    static unsigned buffered_draw_words(unsigned op);
    void consume_word(std::uint64_t word, Mi& mi, std::span<std::uint8_t> rdram);
    void update_counters(std::uint64_t when);
    void schedule_word(Scheduler& scheduler, std::uint64_t when);
    std::uint32_t start_ = 0, end_ = 0, current_ = 0, status_ = 0x80;
    std::uint32_t words_to_skip_ = 0;
    std::uint64_t clock_origin_ = 0, last_update_ = 0;
    std::uint32_t command_cycles_ = 0, pipe_cycles_ = 0;
    std::uint32_t draw_words_ = 0;
    std::array<std::uint64_t, 22> draw_command_{};
    RdpMemoryState memory_state_;
    std::span<std::uint8_t> worker_ram_;
    std::vector<std::unique_ptr<RdpBand>> bands_;  // bands 1.. (band 0 is this object)
    // RDRAM the bands drew since the last barrier: a texture load from it must wait for
    // every band (render to texture), as must a new color or depth image.
    std::array<RdpAccess::Range, 8> drawn_{};
    unsigned drawn_count_ = 0;
    std::unique_ptr<RdpWorker> worker_;
    std::vector<std::uint64_t>* trace_ = nullptr;  // not saved: a debugging aid
    std::function<void(std::uint32_t, std::uint32_t)> ram_write_listener_;  // not saved: wiring
    bool deferring_sync_full_ = false;  // inside a deferred submit_words
    bool pending_sync_full_ = false;    // a deferred Sync Full not delivered yet
};

} // namespace ezy::n64
