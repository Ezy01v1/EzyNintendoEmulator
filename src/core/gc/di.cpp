#include "gc/di.hpp"

#include <algorithm>
#include <cstdio>
#include <string>

#include "gc/disc.hpp"
#include "gc/gekko/gekko.hpp"
#include "gc/memory.hpp"
#include "gc/pi.hpp"
#include "gc/scheduler.hpp"

namespace ezy::gc {

namespace {
constexpr std::uint32_t kBase = Memory::mmio_block_base(MmioBlock::Di);
constexpr std::uint32_t kDicrOffset = 0x1C;

// DISR bits (Dolphin's UDISR).
constexpr std::uint32_t kBreak = 1u << 0, kDeintMask = 1u << 1, kDeint = 1u << 2, kTcintMask = 1u << 3,
                        kTcint = 1u << 4, kBrkintMask = 1u << 5, kBrkint = 1u << 6;
// DICVR bits (UDICVR) and DICR bits (UDICR).
constexpr std::uint32_t kCvrintMask = 1u << 1, kCvrint = 1u << 2;
constexpr std::uint32_t kTstart = 1u << 0, kDma = 1u << 1, kRw = 1u << 2;
constexpr std::uint32_t kDimarMask = 0x03FFFFE0;  // Dolphin, GameCube: ~0xFC00001F
constexpr std::uint32_t kDicfg = 1;
constexpr std::uint32_t kDiscIdBytes = 0x20, kInquiryBytes = 0x20;

std::string hex32(std::uint32_t value) {
    char b[11];
    std::snprintf(b, sizeof b, "0x%08X", value);
    return b;
}

[[noreturn]] void unmodeled(const char* what, std::uint32_t offset) {
    char b[16];
    std::snprintf(b, sizeof b, "%X", offset);
    throw BusError(std::string(what) + " of the unmodeled DVD interface register at offset 0x" + b, kBase + offset);
}

[[noreturn]] void command_error(const std::string& what, std::uint32_t cmd0) {
    throw BusError("DI command " + hex32(cmd0) + ": " + what, kBase + kDicrOffset);
}

void put32(std::vector<std::uint8_t>& bytes, std::size_t at, std::uint32_t value) {
    for (int i = 0; i < 4; ++i) bytes[at + static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(value >> (24 - 8 * i));
}
}  // namespace

Di::Di(Scheduler& scheduler, Pi& pi, std::span<std::uint8_t> mem1, Disc& disc)
    : sched_(scheduler), pi_(pi), mem1_(mem1), disc_(disc) {}

void Di::update_irq() {
    const bool any = ((disr_ & kDeint) && (disr_ & kDeintMask)) || ((disr_ & kTcint) && (disr_ & kTcintMask)) ||
                     ((disr_ & kBrkint) && (disr_ & kBrkintMask)) || ((dicvr_ & kCvrint) && (dicvr_ & kCvrintMask));
    pi_.set_cause(Pi::kDi, any);
}

void Di::start_dma(std::uint32_t dicr, std::uint32_t bytes) {
    const std::uint32_t cmd0 = cmd_[0];
    if (!(dicr & kDma)) command_error("needs a DMA (DICR.DMA clear: not modeled)", cmd0);
    if (dilength_ != bytes) {
        command_error("transfers " + hex32(bytes) + " bytes but DILENGTH is " + hex32(dilength_) + " (not modeled)",
                      cmd0);
    }
    if (std::uint64_t{dimar_} + bytes > mem1_.size()) {
        command_error("DMA to " + hex32(dimar_) + ", " + hex32(bytes) + " bytes, runs past MEM1", cmd0);
    }
    pending_.assign(bytes, 0);
    dma_address_ = dimar_;  // checked now: a DIMAR written during the transfer cannot move it
}

void Di::start_command(std::uint32_t dicr) {
    if (dicr & kRw) command_error("DMA writes to the drive are not modeled", cmd_[0]);
    const std::uint32_t cmd0 = cmd_[0];
    pending_.clear();
    switch (cmd0 >> 24) {
        case 0x12:  // Inquiry (Dolphin: "Taken from my Wii")
            start_dma(dicr, kInquiryBytes);
            put32(pending_, 0, 0x00000002);  // revision level, device code
            put32(pending_, 4, 0x20060526);  // release date
            put32(pending_, 8, 0x41000000);  // version
            break;
        case 0xA8: {  // Read
            std::uint64_t offset = 0;
            std::uint32_t bytes = 0;
            if ((cmd0 & 0xFF) == 0x00) {         // read sectors
                offset = std::uint64_t{cmd_[1]} << 2;
                bytes = cmd_[2];
            } else if ((cmd0 & 0xFF) == 0x40) {  // read the disc ID
                bytes = kDiscIdBytes;
            } else {
                command_error("unmodeled Read subcommand", cmd0);
            }
            start_dma(dicr, bytes);
            if (!disc_.read(offset, pending_)) {
                char at[24];
                std::snprintf(at, sizeof at, "0x%llX", static_cast<unsigned long long>(offset));
                command_error("reads " + hex32(bytes) + " bytes at disc offset " + at +
                                  ", past the end of the disc or unreadable",
                              cmd0);
            }
            ++reads_;
            break;
        }
        case 0xAB:  // Seek: accepted, no effect (Dolphin: "Currently unimplemented")
        case 0xE1:  // Audio Stream: accepted, no effect (streaming is off)
        case 0xE4:  // Audio Buffer Config: accepted, no effect (streaming is off)
            break;
        case 0xE0:  // Request Error: drive state Ready (0) << 24 | DriveError::None (0)
            diimmbuf_ = 0;
            break;
        case 0xE2:  // Request Audio Status: not playing; current/start address and length 0
            if (((cmd0 >> 16) & 0xFF) > 3) command_error("unmodeled audio status subcommand", cmd0);
            diimmbuf_ = 0;
            break;
        default:
            command_error("not modeled", cmd0);
    }
    dicr_ = dicr;  // only once the command is accepted: a rejected one leaves DICR idle
    sched_.schedule_in(EventType::DiTransfer, read_cycles(static_cast<std::uint32_t>(pending_.size())));
}

void Di::on_event() {
    if (!pending_.empty()) {  // a DMA command: its data lands now
        std::copy(pending_.begin(), pending_.end(), mem1_.begin() + dma_address_);
        dimar_ = dma_address_ + static_cast<std::uint32_t>(pending_.size());
        dilength_ -= static_cast<std::uint32_t>(pending_.size());
        pending_.clear();
    }
    dicr_ &= ~kTstart;
    disr_ |= kTcint;
    update_irq();
}

std::uint32_t Di::read32(std::uint32_t offset) {
    switch (offset) {
        case 0x00: return disr_;
        case 0x04: return dicvr_;
        case 0x08: case 0x0C: case 0x10: return cmd_[(offset - 0x08) / 4];
        case 0x14: return dimar_;
        case 0x18: return dilength_;
        case 0x1C: return dicr_;
        case 0x20: return diimmbuf_;
        case 0x24: return kDicfg;
        default: unmodeled("read", offset);
    }
}

void Di::write32(std::uint32_t offset, std::uint32_t value) {
    switch (offset) {
        case 0x00:
            if (value & kBreak) unmodeled("BREAK (abort the command) write", offset);
            disr_ = (disr_ & ~(kDeintMask | kTcintMask | kBrkintMask)) | (value & (kDeintMask | kTcintMask | kBrkintMask));
            disr_ &= ~(value & (kDeint | kTcint | kBrkint));
            update_irq();
            return;
        case 0x04:  // CVR is read-only (the cover stays closed)
            dicvr_ = (dicvr_ & ~kCvrintMask) | (value & kCvrintMask);
            dicvr_ &= ~(value & kCvrint);
            update_irq();
            return;
        case 0x08: case 0x0C: case 0x10: cmd_[(offset - 0x08) / 4] = value; return;
        case 0x14: dimar_ = value & kDimarMask; return;
        case 0x18: dilength_ = value & ~0x1Fu; return;
        case 0x1C:
            if (sched_.pending(EventType::DiTransfer)) unmodeled("write while a command runs", offset);  // DICR
            if (value & kTstart) {
                start_command(value & 7);
            } else {
                dicr_ = value & 7;
            }
            return;
        case 0x20: diimmbuf_ = value; return;
        default: unmodeled("write", offset);  // includes the read-only DICFG
    }
}

std::uint16_t Di::read16(std::uint32_t offset) { unmodeled("16-bit read (the register is 32 bits)", offset); }

void Di::write16(std::uint32_t offset, std::uint16_t) {
    unmodeled("16-bit write (the register is 32 bits)", offset);
}

}  // namespace ezy::gc
