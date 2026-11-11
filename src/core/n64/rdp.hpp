#pragma once

#include <array>
#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <memory>

#include "n64/mi.hpp"
#include "n64/rdp_rasterizer.hpp"
#include "n64/rdp_memory.hpp"
#include "n64/scheduler.hpp"

namespace ezy::n64 {

// Display Processor command interface and software rasterizer.
class Rdp : private RdpRasterizer {
public:
    Rdp() = default;
    ~Rdp();
    bool enable_threaded(std::span<std::uint8_t> rdram);
    [[nodiscard]] bool threaded() const { return worker_ != nullptr; }
    void drain() const { if (worker_) worker_->drain(); }
    void prepare_memory(std::uint32_t address, std::size_t bytes, bool write) const {
        if (worker_ && worker_->pending()) worker_->wait_memory(address, bytes, write);
    }
    [[nodiscard]] std::uint32_t read(std::uint32_t offset, std::uint64_t now) const;
    void write(std::uint32_t offset, std::uint32_t value, Scheduler& scheduler, std::uint64_t when);
    void step(Mi& mi, Scheduler& scheduler, std::span<std::uint8_t> rdram,
              std::span<const std::uint8_t, 0x1000> dmem, std::uint64_t when);

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
        if (version >= 10) ar(key_gb_), ar(key_r_), ar(noise_state_), ar(convert_), ar(prim_lod_);
        else key_gb_ = 0, key_r_ = 0, noise_state_ = 1, convert_ = 0, prim_lod_ = 0;
        if (version >= 11) {
            auto count = static_cast<std::uint32_t>(hidden_.size());
            ar(count);
            if constexpr (Ar::kLoading) {
                hidden_.clear();
                for (std::uint32_t i = 0; i < count; ++i) {
                    std::uint32_t halfword = 0;
                    std::uint8_t bits = 0;
                    ar(halfword), ar(bits);
                    if (bits > 3) ar.fail();
                    else if (bits != 0) hidden_[halfword] = bits;
                }
            } else {
                for (auto [halfword, bits] : hidden_) ar(halfword), ar(bits);
            }
        } else {
            hidden_.clear();
        }
        if constexpr (Ar::kLoading) rebuild_memory_state();
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
    static unsigned buffered_draw_words(unsigned op);
    void update_counters(std::uint64_t when);
    void schedule_word(Scheduler& scheduler, std::uint64_t when);
    std::uint32_t start_ = 0, end_ = 0, current_ = 0, status_ = 0x80;
    std::uint32_t words_to_skip_ = 0;
    std::uint64_t clock_origin_ = 0, last_update_ = 0;
    std::uint32_t command_cycles_ = 0, pipe_cycles_ = 0;
    std::uint32_t draw_words_ = 0;
    std::array<std::uint64_t, 22> draw_command_{};
    RdpMemoryState memory_state_;
    std::unique_ptr<RdpWorker> worker_;
};

} // namespace ezy::n64
