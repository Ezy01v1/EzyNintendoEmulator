#include <doctest/doctest.h>

#include <algorithm>
#include <cstdint>
#include <initializer_list>
#include <vector>

#include "gc/exi.hpp"
#include "gc/gekko/gekko.hpp"
#include "gc/memory.hpp"
#include "gc/pi.hpp"
#include "gc/scheduler.hpp"
#include "gc/si.hpp"
#include "gc/vi.hpp"
#include "run_events.hpp"

using namespace ezy::gc;

namespace {
// SI registers (Dolphin's SI.cpp/SI.h): per channel n, OUT at 0x0C*n, IN_HI at +4, IN_LO at
// +8; POLL 0x30, COMCSR 0x34, SISR 0x38, the EXI clock lock 0x3C, the 128-byte buffer at 0x80.
constexpr std::uint32_t kPoll = 0x30, kComCsr = 0x34, kSisr = 0x38, kBuffer = 0x80;
constexpr std::uint32_t kTcint = 1u << 31, kTcintMask = 1u << 30, kComErr = 1u << 29, kRdstInt = 1u << 28,
                        kRdstIntMask = 1u << 27;
constexpr std::uint64_t kLine = 2 * Vi::kCyclesPerHalfLine;

// Writes `bytes` to the SI buffer and runs a transfer of them on `channel`, `in` bytes back.
void si_transfer(Si& si, unsigned channel, std::initializer_list<std::uint8_t> bytes, std::uint32_t in) {
    std::vector<std::uint8_t> padded(bytes);
    padded.resize((padded.size() + 3) & ~std::size_t{3}, 0);
    for (std::size_t i = 0; i < padded.size(); i += 4) {
        si.write32(kBuffer + static_cast<std::uint32_t>(i),
                   (std::uint32_t{padded[i]} << 24) | (std::uint32_t{padded[i + 1]} << 16) |
                       (std::uint32_t{padded[i + 2]} << 8) | padded[i + 3]);
    }
    si.write32(kComCsr, kTcint | kTcintMask | (static_cast<std::uint32_t>(bytes.size()) << 16) | (in << 8) |
                            (channel << 1) | 1);
}

std::uint8_t si_byte(Si& si, std::uint32_t at) {
    return static_cast<std::uint8_t>(si.read32(kBuffer + (at & ~3u)) >> (24 - 8 * (at & 3)));
}
}  // namespace

TEST_CASE("SI reports a standard pad and its buttons") {
    Scheduler sched;
    Pi pi;
    Si si(sched, pi);
    pi.write32(0x04, 1u << Pi::kSi);
    PadState pad;
    pad.buttons = 0x1000 | 0x0100 | 0x0008;  // Start, A, D-pad up
    pad.stick_x = 0x20;
    pad.stick_y = 0xE0;
    pad.c_x = 0x40;
    pad.c_y = 0xC0;
    pad.trigger_l = 0x11;
    pad.trigger_r = 0x22;
    si.set_pad(pad);

    // The ID command (0x00) answers the standard controller, 0x09 0x00 0x00; the transfer
    // completes at once with TCINT.
    si_transfer(si, 0, {0x00}, 3);
    CHECK((si.read32(kComCsr) & 1) == 0);
    CHECK((si.read32(kComCsr) & kTcint) != 0);
    CHECK((si.read32(kComCsr) & kComErr) == 0);
    CHECK(pi.line());
    CHECK(si_byte(si, 0) == 0x09);
    CHECK(si_byte(si, 1) == 0x00);
    CHECK(si_byte(si, 2) == 0x00);
    si.write32(kComCsr, kTcint);  // acknowledge, mask off
    CHECK_FALSE(pi.line());

    // Channels 1-3 have nothing plugged in: no response, COMERR and that channel's NOREP.
    si_transfer(si, 1, {0x00}, 3);
    CHECK((si.read32(kComCsr) & kComErr) != 0);
    CHECK((si.read32(kSisr) & (1u << 19)) != 0);  // NOREP1
    si.write32(kSisr, 1u << 19);                    // write 1 clears it
    CHECK((si.read32(kSisr) & (1u << 19)) == 0);

    // Origin (0x41): buttons 0, the sticks centred, the triggers released (Dolphin's SOrigin).
    si_transfer(si, 0, {0x41}, 10);
    CHECK((si.read32(kComCsr) & kComErr) == 0);
    const std::uint8_t origin[10] = {0x00, 0x00, 0x80, 0x80, 0x80, 0x80, 0x00, 0x00, 0x00, 0x00};
    for (std::uint32_t i = 0; i < 10; ++i) CHECK(si_byte(si, i) == origin[i]);

    // A direct status read (0x40, analog mode 3, motor off): 8 bytes in the poll layout.
    si_transfer(si, 0, {0x40, 0x03, 0x00}, 8);
    const std::uint32_t hi = (std::uint32_t{0x1108 | 0x0080} << 16) | (0x20u << 8) | 0xE0u;  // buttons | USE_ORIGIN
    const std::uint32_t lo = (0x40u << 24) | (0xC0u << 16) | (0x11u << 8) | 0x22u;
    CHECK(si.read32(kBuffer) == hi);
    CHECK(si.read32(kBuffer + 4) == lo);

    // Polling: channel 0 every X = 10 lines with RDSTINT enabled. Nothing before the 10th line.
    si.write32(0x00, 0x00400300);  // OUT0: poll command, mode 3, motor off (the SDK's PADInit)
    si.write32(kComCsr, kTcint | kRdstIntMask);
    si.write32(kPoll, (10u << 16) | (2u << 8) | 0x80);  // X = 10, Y = 2, EN0
    const std::uint64_t start = sched.now();
    auto deliver = [&](const Scheduler::Due& due) {
        if (due.type == EventType::SiPoll) si.on_event(due.when);
    };
    run_events_to(sched, start + 10 * kLine - 1, deliver);
    CHECK((si.read32(kSisr) & (1u << 29)) == 0);  // RDST0
    CHECK_FALSE(pi.line());
    run_events_to(sched, start + 10 * kLine, deliver);
    CHECK((si.read32(kSisr) & (1u << 29)) != 0);
    CHECK((si.read32(kComCsr) & kRdstInt) != 0);
    CHECK(pi.line());
    CHECK(si.read32(0x04) == hi);  // reading IN_HI clears RDST0 and with it RDSTINT
    CHECK(si.read32(0x08) == lo);
    CHECK((si.read32(kSisr) & (1u << 29)) == 0);
    CHECK((si.read32(kComCsr) & kRdstInt) == 0);
    CHECK_FALSE(pi.line());

    // The next poll follows the pad's new state, 10 lines after the last (never drifting).
    pad.buttons = 0x0200;  // B
    si.set_pad(pad);
    run_events_to(sched, start + 20 * kLine, deliver);
    CHECK((si.read32(0x04) >> 16) == (0x0200u | 0x0080u));

    // An analog mode other than 3 is not modeled, nor is an unknown pad command.
    CHECK_THROWS_AS(si.write32(0x00, 0x00400000), BusError);
    CHECK_THROWS_AS(si_transfer(si, 0, {0x77}, 3), BusError);
    CHECK_THROWS_AS(si.read16(kComCsr), BusError);
    CHECK_THROWS_AS(si.read32(0x40), BusError);
    CHECK(si.read16(kBuffer + 2) == static_cast<std::uint16_t>(si.read32(kBuffer)));  // the buffer takes halves
}

namespace {
// EXI registers (Dolphin's EXI_Channel.h): per channel (stride 0x14) the status/CSR, DMA
// address, DMA length, control and immediate data.
constexpr std::uint32_t kCsr = 0x00, kMar = 0x04, kLen = 0x08, kCr = 0x0C, kData = 0x10;
constexpr std::uint32_t kExiTcintMask = 1u << 2, kExiTcint = 1u << 3, kExt = 1u << 12;
constexpr std::uint32_t kSelectIpl = (2u << 7) | (3u << 4) | kExiTcintMask;  // channel 0, device 1, 8 MHz

void exi_imm_write(Exi& exi, std::uint32_t value, unsigned bytes) {
    exi.write32(kData, value);
    exi.write32(kCr, 1 | (1u << 2) | ((bytes - 1) << 4));
}
std::uint32_t exi_imm_read(Exi& exi, unsigned bytes) {
    exi.write32(kCr, 1 | ((bytes - 1) << 4));
    return exi.read32(kData);
}
std::uint16_t be16(const std::vector<std::uint8_t>& ram, std::uint32_t at) {
    return static_cast<std::uint16_t>((ram[at] << 8) | ram[at + 1]);
}
}  // namespace

TEST_CASE("EXI RTC and SRAM reads return the clock and a valid SRAM") {
    Pi pi;
    std::vector<std::uint8_t> ram(Memory::kMem1Size, 0);
    Exi exi(pi, ram);
    exi.set_rtc_source([] { return 0x30123456u; });
    pi.write32(0x04, 1u << Pi::kExi);

    // The memory card slots are empty (EXT clear); channel 2 never reports a device.
    CHECK((exi.read32(0x00) & kExt) == 0);
    CHECK((exi.read32(0x14) & kExt) == 0);
    CHECK((exi.read32(0x28) & kExt) == 0);

    // The RTC, as the SDK's __OSGetRTC reads it: select, write the command 0x20000000, read 4.
    exi.write32(kCsr, kSelectIpl);
    exi_imm_write(exi, 0x20000000, 4);
    CHECK((exi.read32(kCr) & 1) == 0);           // immediate: done at once
    CHECK((exi.read32(kCsr) & kExiTcint) != 0);  // with TCINT
    CHECK(pi.line());
    exi.write32(kCsr, kSelectIpl | kExiTcint);  // acknowledge
    CHECK_FALSE(pi.line());
    CHECK(exi_imm_read(exi, 4) == 0x30123456u);
    exi.write32(kCsr, kExiTcint);  // deselect

    // The SRAM, as __OSInitSram reads it: the command 0x20000100, then a 64-byte DMA.
    exi.write32(kCsr, kSelectIpl);
    exi_imm_write(exi, 0x20000100, 4);
    exi.write32(kMar, 0x00001000);
    exi.write32(kLen, 64);
    exi.write32(kCr, 1 | 2);  // TSTART | DMA, read
    CHECK((exi.read32(kCr) & 1) == 0);
    exi.write32(kCsr, kExiTcint);
    // Dolphin's defaults: rtc_bias 0, English, flags 0x2C (stereo, OOBE done); the checksum
    // pair is the 16-bit sum of the words 0x0C-0x13 and the sum of their complements.
    std::uint16_t sum = 0, inv = 0;
    for (std::uint32_t at = 0x0C; at < 0x14; at += 2) {
        sum = static_cast<std::uint16_t>(sum + be16(ram, 0x1000 + at));
        inv = static_cast<std::uint16_t>(inv + static_cast<std::uint16_t>(~be16(ram, 0x1000 + at)));
    }
    CHECK(be16(ram, 0x1000) == sum);
    CHECK(be16(ram, 0x1002) == inv);
    CHECK(sum == 0x002C);
    CHECK(inv == 0xFFD0);
    CHECK(ram[0x1000 + 0x12] == 0);     // language
    CHECK(ram[0x1000 + 0x13] == 0x2C);  // flags
    CHECK(exi.sram()[0x13] == 0x2C);

    // A write lands in the SRAM: byte 0x12 (the language) through 0xA0000100 + 0x12 << 6.
    exi.write32(kCsr, kSelectIpl);
    exi_imm_write(exi, 0xA0000100 + (0x12u << 6), 4);
    exi_imm_write(exi, 0x01000000, 1);
    exi.write32(kCsr, kExiTcint);
    CHECK(exi.sram()[0x12] == 1);

    // A DMA write under a read command sends MEM1's bytes to the chip and never changes MEM1.
    exi.write32(kCsr, kSelectIpl);
    exi_imm_write(exi, 0x20000100, 4);
    exi.write32(kMar, 0x00002000);
    exi.write32(kLen, 64);
    exi.write32(kCr, 1 | 2 | (1u << 2));
    exi.write32(kCsr, kExiTcint);
    CHECK(std::all_of(ram.begin() + 0x2000, ram.begin() + 0x2040, [](std::uint8_t b) { return b == 0; }));
    CHECK(exi.sram()[0x13] == 0x2C);

    // The host clock: seconds since 2000-01-01, so well past 26 years by now.
    CHECK(Exi::host_rtc() > 26u * 365u * 86400u);

    // Errors: an empty slot, no device selected, an unaligned DMA, a DMA past MEM1, the IPL ROM.
    exi.write32(kCsr, (1u << 7));  // channel 0, device 0: memory card A, empty
    CHECK_THROWS_AS(exi_imm_read(exi, 4), BusError);
    exi.write32(kCsr, 0);
    CHECK_THROWS_AS(exi_imm_read(exi, 4), BusError);
    exi.write32(kCsr, kSelectIpl);
    exi_imm_write(exi, 0x20000100, 4);
    exi.write32(kMar, 0x00001004);
    CHECK_THROWS_AS(exi.write32(kCr, 1 | 2), BusError);
    exi.write32(kMar, Memory::kMem1Size - 0x20);
    CHECK_THROWS_AS(exi.write32(kCr, 1 | 2), BusError);
    exi.write32(kCsr, 0);
    exi.write32(kCsr, kSelectIpl);
    exi_imm_write(exi, 0x00000000, 4);  // IPL ROM offset 0: the BIOS is not offered
    CHECK_THROWS_AS(exi_imm_read(exi, 4), BusError);
    CHECK_THROWS_AS(exi.read16(kCsr), BusError);
    CHECK_THROWS_AS(exi.read32(0x3C), BusError);
}
