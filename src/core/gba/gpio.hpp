#pragma once

#include <cstdint>
#include <ctime>
#include <functional>
#include <span>

namespace ezy::gba {

// The cartridge's GPIO port (GBATEK "GBA Cart I/O Port (GPIO)") with the Seiko S-3511
// real-time clock behind it, as in Pokemon Ruby/Sapphire/Emerald and others.
//
// The port lives in the ROM area: 0x080000C4 data (4 pins), 0x080000C6 direction
// (1 = the game drives the pin), 0x080000C8 control (bit 0: the registers read back;
// otherwise those addresses read ROM). RTC pins: 0 = SCK, 1 = SIO, 2 = CS.
//
// Serial protocol: with CS high, each bit is clocked on SCK's rising edge, LSB first.
// The first byte is the command: bits 0-3 = 0110, bits 4-6 the register, bit 7 = read.
// Registers: 0 reset, 2 date+time (7 BCD bytes: year, month, day, weekday, hour,
// minute, second), 3 force IRQ, 4 control (1 byte), 6 time (3 bytes). When reading,
// the chip puts each bit on SIO at SCK's falling edge, so it's there after the rise.
//
// The time comes from the host clock (or a test-supplied one); setting the time is
// accepted and ignored, as most emulators do.
class Gpio {
public:
    using Clock = std::function<std::tm()>;

    // Defaults to the host's local time.
    explicit Gpio(Clock clock = {});

    // `offset` is the ROM offset (0xC4-0xC9). readable() says whether reads see the port.
    [[nodiscard]] bool readable() const { return (control_ & 1) != 0; }
    [[nodiscard]] std::uint16_t read(std::uint32_t offset) const;
    void write(std::uint32_t offset, std::uint16_t value);

    static constexpr std::uint32_t kFirst = 0xC4;
    static constexpr std::uint32_t kLast = 0xC9;

private:
    enum class State { Idle, Command, Write, Read };

    void write_pins(std::uint8_t pins);
    void on_rising_edge(bool sio);
    void finish_command();
    void latch_time();

    Clock clock_;
    std::uint8_t data_ = 0;       // last value the game wrote
    std::uint8_t direction_ = 0;
    std::uint8_t control_ = 0;
    std::uint8_t pins_ = 0;       // the pins as the RTC sees them
    bool sio_out_ = false;        // the RTC's output on SIO

    State state_ = State::Idle;
    std::uint8_t shift_ = 0;
    int bit_ = 0;
    int byte_ = 0;
    int length_ = 0;
    std::uint8_t reg_ = 0;
    std::uint8_t buffer_[7] = {};
    std::uint8_t rtc_control_ = 0x40;  // 24-hour mode
};

// The S-3511 ID string games link in with the RTC driver.
[[nodiscard]] bool has_rtc(std::span<const std::uint8_t> rom);

}  // namespace ezy::gba
