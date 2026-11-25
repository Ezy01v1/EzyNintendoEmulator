#include "gc/dsp_zelda.hpp"

#include <cstdio>
#include <string>

#include "gc/gekko/gekko.hpp"
#include "gc/memory.hpp"

namespace ezy::gc {

namespace {
// The CPU->DSP mailbox's low half: where a mail lands (an error's address).
constexpr std::uint32_t kMailbox = Memory::mmio_block_base(MmioBlock::Dsp) + 0x02;

// The task mails every SDK DSP task sends (Dolphin's UCodes.h), and the Zelda microcode's.
constexpr std::uint32_t kDspInit = 0xDCD10000, kDspSync = 0xDCD10004, kDspFrameEnd = 0xDCD10005;
constexpr std::uint32_t kHandshake = 0xF3551111, kAck = 0xF3550000;
constexpr std::uint32_t kFrameEndPrefix = 0xCDD10000;

std::string hex32(std::uint32_t value) {
    char b[11];
    std::snprintf(b, sizeof b, "0x%08X", value);
    return b;
}

[[noreturn]] void unmodeled(const std::string& what) {
    throw BusError("DSP microcode (Zelda): " + what + " is not modeled", kMailbox);
}
}  // namespace

void ZeldaMicrocode::start() {
    state_ = MailState::Waiting;
    command_.clear();
    command_words_left_ = 0;
    pending_.clear();
    can_execute_ = true;
    voices_per_frame_ = frames_ = frame_ = synced_voices_ = 0;
    send_(kDspInit, true);
    send_(kHandshake, false);
}

void ZeldaMicrocode::receive(std::uint32_t mail) {
    switch (state_) {
        case MailState::Waiting:
            if (mail & 0x80000000) {
                if ((mail & 0xFFFF0000) != kFrameEndPrefix || (mail & 0xFFFF) != 3) unmodeled("mail " + hex32(mail));
                can_execute_ = true;  // the CPU took the frame end: go on
                run_pending_commands();
            } else if ((mail & 0xFFFF) == 0) {
                if (!rendering()) unmodeled("sync mail " + hex32(mail) + " with no rendering in progress");
                state_ = MailState::Sync;
            } else if ((mail >> 16) == 0) {
                command_.clear();
                command_words_left_ = mail;
                state_ = MailState::WritingCommand;
            } else {
                unmodeled("mail " + hex32(mail));
            }
            return;
        case MailState::Sync:
            // Voices up to the end of this group may be rendered; the skip flags (low half)
            // pick which ones are mixed, and nothing is mixed here.
            synced_voices_ = (((mail >> 16) & 0xF) + 1) << 4;
            state_ = MailState::Waiting;
            render();
            return;
        case MailState::WritingCommand:
            command_.push_back(mail);
            if (--command_words_left_ == 0) {
                pending_.push_back(std::move(command_));
                command_.clear();
                state_ = MailState::Waiting;
                run_pending_commands();
            }
            return;
    }
}

void ZeldaMicrocode::ack(std::uint16_t sync) {
    send_(kDspSync, true);
    send_(kAck | sync, false);
}

void ZeldaMicrocode::run_pending_commands() {
    while (!pending_.empty() && !rendering() && can_execute_) {
        const std::vector<std::uint32_t> words = std::move(pending_.front());
        pending_.pop_front();
        const std::uint32_t cmd = words[0];
        const auto sync = static_cast<std::uint16_t>(cmd >> 16);
        switch ((cmd >> 24) & 0x7F) {
            case 0x00:
            case 0x03:
            case 0x0A:
            case 0x0B:
            case 0x0C:
            case 0x0D:
            case 0x0E:
            case 0x0F: ack(sync); break;  // no effect here (no mixing, no GBA crypto, no Wii ARAM)
            case 0x01:
                if (words.size() != 5) unmodeled("setup command " + hex32(cmd) + " of " + std::to_string(words.size()) + " words");
                voices_per_frame_ = cmd & 0xFFFF;
                ack(sync);
                break;
            case 0x02:
                if (words.size() != 3) unmodeled("render command " + hex32(cmd) + " of " + std::to_string(words.size()) + " words");
                frames_ = (cmd >> 16) & 0xFF;
                frame_ = 0;
                render();
                break;
            default: unmodeled("command " + hex32(cmd));
        }
    }
}

// Dolphin's RenderAudio without the mixing: each frame goes as far as the synced voices, and
// waits for the next sync mail when it is not whole yet.
void ZeldaMicrocode::render() {
    if (!rendering()) return;
    while (frame_ < frames_) {
        if (synced_voices_ < voices_per_frame_) return;  // the next sync lets it go on
        ack(static_cast<std::uint16_t>(0xFF00 | frame_));
        synced_voices_ = 0;
        ++frame_;
    }
    send_(kDspFrameEnd, true);  // DONE_RENDERING
    can_execute_ = false;        // until the CPU's 0xCDD10003
}

}  // namespace ezy::gc
