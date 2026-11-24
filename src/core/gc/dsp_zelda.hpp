#pragma once

#include <cstdint>
#include <deque>
#include <functional>
#include <vector>

namespace ezy::gc {

// The mail protocol of MKDD's DSP microcode: Nintendo EAD's JAudio "Zelda" microcode, CRC
// 0x2FCDF1EC (after Dolphin's Core/HW/DSPHLE/UCodes/Zelda.cpp and UCodes.cpp, from memory of
// that code, not re-read in this session: flags MAKE_DOLBY_LOUD, so the default protocol,
// neither LIGHT_PROTOCOL nor SYNC_PER_FRAME; checked against what MKDD's own SDK and JAudio
// code send and wait for). The protocol is followed mail for mail, so the CPU gets every
// answer when Dolphin's HLE would give it; no audio is mixed (design: audio is sub-project
// 3), so the output buffers are left as they are. Every mail or command this does not model
// throws BusError at the mailbox.
//
// Start (ZeldaUCode::Initialize): DSP_INIT 0xDCD10000 with the DSP interrupt, then the
// handshake 0xF3551111.
//
// Mails from the CPU (HandleMailDefault), while waiting:
//   0x0000nnnn, n != 0  a command of n words follows (the next n mails).
//   0x00000000          a sync follows: the next mail is group << 16 | skip flags, which lets
//                       the rendering go on up to voice (group + 1) * 16 (RENDERING state; with
//                       no rendering in progress the real microcode halts: a stop here).
//   0xCDD1000x          after a frame end: 3 = go on (commands run again); 0 = halt, 1 = a new
//                       microcode follows, 2 = back to the DSP ROM (not modeled: stops).
// Commands run once complete, unless a rendering is in progress or a frame end is waiting for
// its 0xCDD10003; then they queue. Each is acknowledged with DSP_SYNC 0xDCD10004 (with the DSP
// interrupt) and 0xF3550000 | the command word's high half (SendCommandAck STANDARD), except
// command 02. By the command word's bits 24-30:
//   00, 03, 0A, 0B, 0F  no effect; 0C, 0D, 0E: their parameters are not used here.
//   01  setup: voices per frame = the low half; then the VPB base, the coefficient tables, the
//       AFC coefficients and the reverb PB base (stored only).
//   02  render (bits 16-23) frames: volume, then the left and right output buffers. For each
//       frame, once the syncs reach the voices per frame: DSP_SYNC and 0xF355FF00 | the frame;
//       after the last, DSP_FRAME_END 0xDCD10005 with the DSP interrupt.
//   04-09 crash the real microcode, and the rest do not exist: they stop emulation.
class ZeldaMicrocode {
public:
    // Sends a mail to the CPU, with the DSP interrupt when `interrupt` (Dsp::push_mail).
    using Send = std::function<void(std::uint32_t mail, bool interrupt)>;
    static constexpr std::uint32_t kCrc = 0x2FCDF1EC;

    explicit ZeldaMicrocode(Send send) : send_(std::move(send)) {}

    // The microcode starts running (the DSP ROM booted it): the state is reset and it greets.
    void start();
    // A mail from the CPU.
    void receive(std::uint32_t mail);

private:
    enum class MailState : std::uint8_t { Waiting, Sync, WritingCommand };

    void run_pending_commands();
    void render();
    void ack(std::uint16_t sync);
    [[nodiscard]] bool rendering() const { return frame_ != frames_; }

    Send send_;
    MailState state_ = MailState::Waiting;
    std::vector<std::uint32_t> command_;          // the command being received
    std::uint32_t command_words_left_ = 0;
    std::deque<std::vector<std::uint32_t>> pending_;  // complete commands not run yet
    bool can_execute_ = true;                     // false from a frame end to its 0xCDD10003
    std::uint32_t voices_per_frame_ = 0;          // command 01
    std::uint32_t frames_ = 0, frame_ = 0;        // command 02: frames asked for, the current one
    std::uint32_t synced_voices_ = 0;             // voices of the current frame the CPU synced
};

}  // namespace ezy::gc
