#include "n64/rsp_audio_hle.hpp"

#include <algorithm>
#include <array>

namespace ezy::n64 {

namespace {

constexpr std::uint32_t kDramMask = 0xFFFFFF;

std::uint32_t be32(std::span<const std::uint8_t> bytes, std::size_t offset) {
    return (std::uint32_t{bytes[offset]} << 24) | (bytes[offset + 1] << 16) | (bytes[offset + 2] << 8) |
           bytes[offset + 3];
}

std::uint64_t fnv1a(std::span<const std::uint8_t> bytes) {
    std::uint64_t hash = 0xCBF29CE484222325ULL;
    for (const auto byte : bytes) hash = (hash ^ byte) * 0x100000001B3ULL;
    return hash;
}

std::int16_t clamp16(std::int32_t value) {
    return static_cast<std::int16_t>(std::clamp(value, -32768, 32767));
}

// Mario Kart 64 (USA): the data segment of its audio microcode.
constexpr std::uint32_t kMkDataBytes = 0x300;
constexpr std::uint64_t kMkDataHash = 0xC7E372036390F60CULL;
constexpr std::uint32_t kResampleTable = 0x100;  // in the microcode data: 64 sets of 4 taps

class Nead {
public:
    Nead(std::span<std::uint8_t> rdram, std::span<std::uint8_t, 0x1000> dmem, const AudioHle::Access& access)
        : rdram_(rdram), dmem_(dmem), access_(access) {}

    void load_resample_table(std::uint32_t data) {
        for (unsigned i = 0; i < resample_.size(); ++i) resample_[i] = static_cast<std::int16_t>(dram16(data + i * 2));
    }

    void execute(std::uint32_t w1, std::uint32_t w2) {
        switch ((w1 >> 24) & 0x1F) {
            case 0x01: adpcm(w1, w2); break;
            case 0x02: clear(static_cast<std::uint16_t>(w1), w2 & 0xFFF); break;
            case 0x05: resample(w1, w2); break;
            case 0x08: in_ = static_cast<std::uint16_t>(w1), out_ = static_cast<std::uint16_t>(w2 >> 16),
                       count_ = static_cast<std::uint16_t>(w2); break;  // SETBUFF
            case 0x0A: {  // DMEMMOVE
                const auto count = static_cast<std::uint16_t>(w2);
                if (count != 0) move(static_cast<std::uint16_t>(w2 >> 16), static_cast<std::uint16_t>(w1), (count + 3) & ~3U);
                break;
            }
            case 0x0B: load_adpcm_table(w1 & 0xFFFF, w2 & kDramMask); break;
            case 0x0C: mix(static_cast<std::uint16_t>(w2), static_cast<std::uint16_t>(w2 >> 16), (w1 >> 12) & 0xFF0,
                           static_cast<std::int16_t>(w1)); break;
            case 0x0D:  // INTERLEAVE
                if (count_ != 0) interleave(out_, static_cast<std::uint16_t>(w2 >> 16), static_cast<std::uint16_t>(w2), count_);
                break;
            case 0x0F: loop_ = w2 & kDramMask; break;  // SETLOOP
            case 0x11: copy_every_other(static_cast<std::uint16_t>(w2), static_cast<std::uint16_t>(w2 >> 16), w1 & 0xFFFF); break;
            case 0x12:  // ENVSETUP1 (Mario Kart: no wet volume step)
                env_values_[2] = static_cast<std::uint16_t>((w1 >> 8) & 0xFF00);
                env_steps_[2] = 0;
                env_steps_[0] = static_cast<std::uint16_t>(w2 >> 16);
                env_steps_[1] = static_cast<std::uint16_t>(w2);
                break;
            case 0x13: envmixer(w1, w2); break;
            case 0x14: load((w1 & 0xFFF), w2 & kDramMask, (w1 >> 12) & 0xFFF); break;
            case 0x15: save((w1 & 0xFFF), w2 & kDramMask, (w1 >> 12) & 0xFFF); break;
            case 0x16:  // ENVSETUP2
                env_values_[0] = static_cast<std::uint16_t>(w2 >> 16);
                env_values_[1] = static_cast<std::uint16_t>(w2);
                break;
            default: break;  // SPNOOP and SEGMENT (addresses are physical)
        }
    }

private:
    // DMEM is big-endian and wraps at 4 KiB.
    std::uint8_t& byte(std::uint32_t address) { return dmem_[address & 0xFFF]; }
    std::int16_t sample(std::uint32_t address) {
        return static_cast<std::int16_t>((byte(address) << 8) | byte(address + 1));
    }
    void set_sample(std::uint32_t address, std::int16_t value) {
        byte(address) = static_cast<std::uint8_t>(static_cast<std::uint16_t>(value) >> 8);
        byte(address + 1) = static_cast<std::uint8_t>(value);
    }

    std::uint8_t dram8(std::uint32_t address) {
        address &= kDramMask;
        return address < rdram_.size() ? rdram_[address] : 0;
    }
    std::uint16_t dram16(std::uint32_t address) {
        return static_cast<std::uint16_t>((dram8(address) << 8) | dram8(address + 1));
    }
    void guard(std::uint32_t address, std::uint32_t bytes, bool write) {
        if (bytes != 0) access_(address & kDramMask, bytes, write);
    }
    void dram_write8(std::uint32_t address, std::uint8_t value) {
        address &= kDramMask;
        if (address < rdram_.size()) rdram_[address] = value;
    }
    void dram_write16(std::uint32_t address, std::uint16_t value) {
        dram_write8(address, static_cast<std::uint8_t>(value >> 8));
        dram_write8(address + 1, static_cast<std::uint8_t>(value));
    }

    // RSP DMA: 8-byte aligned addresses, lengths rounded up to 8 bytes.
    void load(std::uint32_t dmem, std::uint32_t address, std::uint32_t count) {
        dmem &= ~7U, address &= ~7U, count = (count + 7) & ~7U;
        guard(address, count, false);
        for (std::uint32_t i = 0; i < count; ++i) byte(dmem + i) = dram8(address + i);
    }
    void save(std::uint32_t dmem, std::uint32_t address, std::uint32_t count) {
        dmem &= ~7U, address &= ~7U, count = (count + 7) & ~7U;
        guard(address, count, true);
        for (std::uint32_t i = 0; i < count; ++i) dram_write8(address + i, byte(dmem + i));
    }
    void clear(std::uint32_t dmem, std::uint32_t count) {
        for (std::uint32_t i = 0; i < count; ++i) byte(dmem + i) = 0;
    }
    void move(std::uint32_t to, std::uint32_t from, std::uint32_t count) {
        for (std::uint32_t i = 0; i < count; ++i) byte(to + i) = byte(from + i);
    }

    void load_adpcm_table(std::uint32_t bytes, std::uint32_t address) {
        const std::uint32_t entries = std::min(((bytes + 7) & ~7U) / 2, static_cast<std::uint32_t>(table_.size()));
        guard(address, entries * 2, false);
        for (std::uint32_t i = 0; i < entries; ++i) table_[i] = static_cast<std::int16_t>(dram16(address + i * 2));
    }

    // The microcode scales the destination by 0x7FFF (not 1.0) and accumulates both
    // products before rounding once (VMULF then VMACF); checked against the RSP.
    void mix(std::uint32_t to, std::uint32_t from, std::uint32_t bytes, std::int16_t gain) {
        for (std::uint32_t i = 0; i < bytes; i += 2) {
            const std::int64_t accu = std::int64_t{sample(to + i)} * 0x7FFF + std::int64_t{sample(from + i)} * gain + 0x4000;
            set_sample(to + i, static_cast<std::int16_t>(std::clamp<std::int64_t>(accu >> 15, -32768, 32767)));
        }
    }

    void interleave(std::uint32_t to, std::uint32_t left, std::uint32_t right, std::uint32_t count) {
        for (std::uint32_t i = 0; i < count / 2; ++i) {
            set_sample(to + i * 4, sample(left + i * 2));
            set_sample(to + i * 4 + 2, sample(right + i * 2));
        }
    }

    void copy_every_other(std::uint32_t to, std::uint32_t from, std::uint32_t count) {
        for (std::uint32_t i = 0; i < count; ++i) set_sample(to + i * 2, sample(from + i * 4));
    }

    void adpcm(std::uint32_t w1, std::uint32_t w2) {
        const unsigned flags = (w1 >> 16) & 0xFF;
        const bool init = flags & 1, loop = flags & 2;  // 2-bit samples (flag 4) stay on the RSP
        const std::uint32_t address = w2 & kDramMask;
        std::uint32_t out = out_, in = in_;
        std::uint32_t count = (count_ + 0x1F) & ~0x1FU;
        std::array<std::int16_t, 16> last{};
        if (!init) {
            const std::uint32_t from = loop ? loop_ : address;
            guard(from, 32, false);
            for (unsigned i = 0; i < 16; ++i) last[i] = static_cast<std::int16_t>(dram16(from + i * 2));
        }
        for (unsigned i = 0; i < 16; ++i, out += 2) set_sample(out, last[i]);
        while (count != 0) {
            const std::uint8_t code = byte(in++);
            const unsigned scale = code >> 4;
            const std::int16_t* book = table_.data() + ((code & 0xF) << 4);
            std::array<std::int16_t, 16> frame{};
            const unsigned shift = scale < 12 ? 12 - scale : 0;
            for (unsigned i = 0; i < 8; ++i) {  // two signed 4-bit residuals per byte
                const std::uint8_t b = byte(in++);
                frame[i * 2] = static_cast<std::int16_t>(static_cast<std::int16_t>((b & 0xF0) << 8) >> shift);
                frame[i * 2 + 1] = static_cast<std::int16_t>(static_cast<std::int16_t>((b & 0x0F) << 12) >> shift);
            }
            residuals(last.data(), frame.data(), book, last[14], last[15]);
            residuals(last.data() + 8, frame.data() + 8, book, last[6], last[7]);
            for (unsigned i = 0; i < 16; ++i, out += 2) set_sample(out, last[i]);
            count -= 32;
        }
        guard(address, 32, true);
        for (unsigned i = 0; i < 16; ++i) dram_write16(address + i * 2, static_cast<std::uint16_t>(last[i]));
    }

    // Order-2 prediction of 8 samples from the previous two and the decoded residuals.
    static void residuals(std::int16_t* out, const std::int16_t* frame, const std::int16_t* book,
                          std::int16_t l1, std::int16_t l2) {
        const std::int16_t* book1 = book;
        const std::int16_t* book2 = book + 8;
        for (unsigned i = 0; i < 8; ++i) {
            std::int32_t accu = static_cast<std::int32_t>(frame[i]) << 11;
            accu += book1[i] * l1 + book2[i] * l2;
            for (unsigned k = 0; k < i; ++k) accu += book2[k] * frame[i - 1 - k];
            out[i] = clamp16(accu >> 11);
        }
    }

    void resample(std::uint32_t w1, std::uint32_t w2) {
        const bool init = ((w1 >> 16) & 1) != 0;
        const std::uint32_t pitch = (w1 & 0xFFFF) << 1;
        const std::uint32_t address = w2 & kDramMask;
        std::uint32_t out = out_;
        std::uint32_t position = (in_ >> 1) - 4;  // in samples; four history samples precede the input
        std::uint32_t count = ((count_ + 0xF) & ~0xFU) >> 1;
        std::uint32_t accu = 0;
        const auto at = [](std::uint32_t pos) { return (pos & 0x7FF) * 2; };
        if (init) {
            for (unsigned k = 0; k < 4; ++k) set_sample(at(position + k), 0);
        } else {
            guard(address, 10, false);
            for (unsigned k = 0; k < 4; ++k) set_sample(at(position + k), static_cast<std::int16_t>(dram16(address + k * 2)));
            accu = dram16(address + 8);
        }
        for (; count != 0; --count, out += 2) {
            const std::int16_t* taps = resample_.data() + ((accu & 0xFC00) >> 8);
            std::int32_t sum = 0;  // each tap is rounded on its own (checked against the RSP)
            for (unsigned k = 0; k < 4; ++k) sum += (sample(at(position + k)) * taps[k] + 0x4000) >> 15;
            set_sample(out, clamp16(sum));
            accu += pitch;
            position += accu >> 16;
            accu &= 0xFFFF;
        }
        guard(address, 10, true);
        for (unsigned k = 0; k < 4; ++k) dram_write16(address + k * 2, static_cast<std::uint16_t>(sample(at(position + k))));
        dram_write16(address + 8, static_cast<std::uint16_t>(accu));
    }

    void envmixer(std::uint32_t w1, std::uint32_t w2) {
        const std::uint32_t in = (w1 >> 12) & 0xFF0;
        std::uint32_t count = (((w1 >> 8) & 0xFF) + 7) & ~7U;
        const std::int16_t xor_left = static_cast<std::int16_t>(-static_cast<std::int16_t>((w1 >> 1) & 1));
        const std::int16_t xor_right = static_cast<std::int16_t>(-static_cast<std::int16_t>(w1 & 1));
        std::uint32_t dry_left = (w2 >> 20) & 0xFF0, dry_right = (w2 >> 12) & 0xFF0;
        std::uint32_t wet_left = (w2 >> 4) & 0xFF0, wet_right = (w2 << 4) & 0xFF0;
        // in * unsigned volume, keeping bits 16..31 of the product.
        const auto scale = [](std::int16_t value, std::uint16_t volume) {
            return static_cast<std::int16_t>((static_cast<std::int64_t>(value) * volume) >> 16);
        };
        for (std::uint32_t offset = 0; count != 0; count -= 8, offset += 16) {
            for (unsigned i = 0; i < 8; ++i) {
                const std::uint32_t at = offset + i * 2;
                const std::int16_t source = sample(in + at);
                const auto left = static_cast<std::int16_t>(scale(source, env_values_[0]) ^ xor_left);
                const auto right = static_cast<std::int16_t>(scale(source, env_values_[1]) ^ xor_right);
                const std::int16_t wet_l = scale(left, env_values_[2]);
                const std::int16_t wet_r = scale(right, env_values_[2]);
                set_sample(dry_left + at, clamp16(sample(dry_left + at) + left));
                set_sample(dry_right + at, clamp16(sample(dry_right + at) + right));
                set_sample(wet_left + at, clamp16(sample(wet_left + at) + wet_l));
                set_sample(wet_right + at, clamp16(sample(wet_right + at) + wet_r));
            }
            for (unsigned k = 0; k < 3; ++k) env_values_[k] = static_cast<std::uint16_t>(env_values_[k] + env_steps_[k]);
        }
    }

    std::span<std::uint8_t> rdram_;
    std::span<std::uint8_t, 0x1000> dmem_;
    const AudioHle::Access& access_;
    std::uint16_t in_ = 0, out_ = 0, count_ = 0;
    std::uint32_t loop_ = 0;
    std::array<std::int16_t, 256> table_{};
    std::array<std::int16_t, 256> resample_{};
    std::array<std::uint16_t, 3> env_values_{}, env_steps_{};
};

}  // namespace

RspTask RspTask::read(std::span<const std::uint8_t, 0x1000> dmem) {
    const auto word = [&](unsigned index) { return be32(dmem, 0xFC0 + index * 4); };
    RspTask task;
    task.type = word(0), task.flags = word(1);
    task.ucode = word(4) & kDramMask, task.ucode_size = word(5);
    task.ucode_data = word(6) & kDramMask, task.ucode_data_size = word(7);
    task.data_ptr = word(12) & kDramMask, task.data_size = word(13);
    return task;
}

bool AudioHle::supports(const RspTask& task, std::span<const std::uint8_t> rdram) {
    if (task.type != RspTask::kAudio || task.ucode_data_size != kMkDataBytes) return false;
    if (task.ucode_data + kMkDataBytes > rdram.size()) return false;
    if (task.data_ptr + task.data_size > rdram.size() || (task.data_size & 7) != 0) return false;
    // Only the data is hashed: the 4 KiB "text" also holds whatever follows the code.
    if (fnv1a(rdram.subspan(task.ucode_data, kMkDataBytes)) != kMkDataHash) return false;
    // Every command must be one checked against the microcode; anything else (POLEF,
    // block copies, 2-bit ADPCM, resampler flag 2) leaves the task to the RSP.
    for (std::uint32_t offset = 0; offset < task.data_size; offset += 8) {
        const std::uint32_t w1 = be32(rdram, task.data_ptr + offset);
        const unsigned op = (w1 >> 24) & 0x1F, flags = (w1 >> 16) & 0xFF;
        constexpr std::uint32_t kChecked = (1U << 0x00) | (1U << 0x01) | (1U << 0x02) | (1U << 0x05) | (1U << 0x07) |
                                           (1U << 0x08) | (1U << 0x0A) | (1U << 0x0B) | (1U << 0x0C) | (1U << 0x0D) |
                                           (1U << 0x0F) | (1U << 0x11) | (1U << 0x12) | (1U << 0x13) | (1U << 0x14) |
                                           (1U << 0x15) | (1U << 0x16);
        if (!((kChecked >> op) & 1) || (op == 0x01 && (flags & 4)) || (op == 0x05 && (flags & 2))) return false;
    }
    return true;
}

void AudioHle::run(const RspTask& task, std::span<std::uint8_t> rdram, std::span<std::uint8_t, 0x1000> dmem,
                   const Access& access) {
    Nead nead(rdram, dmem, access);
    access(task.ucode_data + kResampleTable, 512, false);
    nead.load_resample_table(task.ucode_data + kResampleTable);
    access(task.data_ptr, task.data_size, false);
    for (std::uint32_t offset = 0; offset < task.data_size; offset += 8) {
        // The command list cannot change under the task: commands only write DMEM and the
        // RDRAM ranges they name, which never overlap a list the game is still running.
        nead.execute(be32(rdram, task.data_ptr + offset), be32(rdram, task.data_ptr + offset + 4));
    }
}

}  // namespace ezy::n64
