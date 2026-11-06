#include "gba/gpio.hpp"

#include <algorithm>
#include <iterator>
#include <string_view>
#include <utility>

namespace ezy::gba {

namespace {

constexpr std::uint8_t kSck = 1U << 0;
constexpr std::uint8_t kSio = 1U << 1;
constexpr std::uint8_t kCs = 1U << 2;

constexpr std::uint8_t bcd(int value) { return static_cast<std::uint8_t>(((value / 10) << 4) | (value % 10)); }

// Bytes each register transfers after the command byte.
constexpr int register_length(std::uint8_t reg) {
    switch (reg) {
        case 2: return 7;
        case 4: return 1;
        case 6: return 3;
        default: return 0;
    }
}

std::tm host_time() {
    const std::time_t now = std::time(nullptr);
    std::tm local{};
#ifdef _WIN32
    localtime_s(&local, &now);
#else
    localtime_r(&now, &local);
#endif
    return local;
}

}  // namespace

Gpio::Gpio(Clock clock) : clock_(clock ? std::move(clock) : Clock(host_time)) {}

std::uint16_t Gpio::read(std::uint32_t offset) const {
    switch (offset & ~1U) {
        case 0xC4: {
            // Pins the game drives read back what it wrote; the others come from the RTC.
            const std::uint8_t from_rtc = sio_out_ ? kSio : 0;
            return static_cast<std::uint16_t>(((data_ & direction_) | (from_rtc & ~direction_)) & 0x0F);
        }
        case 0xC6: return direction_;
        case 0xC8: return control_;
        default: return 0;
    }
}

void Gpio::write(std::uint32_t offset, std::uint16_t value) {
    switch (offset & ~1U) {
        case 0xC4:
            data_ = static_cast<std::uint8_t>(value & 0x0F);
            write_pins(static_cast<std::uint8_t>((data_ & direction_) | (pins_ & ~direction_)));
            break;
        case 0xC6: direction_ = static_cast<std::uint8_t>(value & 0x0F); break;
        case 0xC8: control_ = static_cast<std::uint8_t>(value & 1); break;
        default: break;
    }
}

void Gpio::write_pins(std::uint8_t pins) {
    const std::uint8_t old = pins_;
    pins_ = pins;
    if ((pins & kCs) == 0) {
        state_ = State::Idle;
        return;
    }
    if ((old & kCs) == 0) {
        // CS rising: a new transfer starts with the command byte.
        state_ = State::Command;
        shift_ = 0;
        bit_ = 0;
        return;
    }
    if ((old & kSck) != 0 && (pins & kSck) == 0 && state_ == State::Read) {
        sio_out_ = ((buffer_[byte_] >> bit_) & 1) != 0;  // falling edge: present the next bit
    }
    if ((old & kSck) == 0 && (pins & kSck) != 0) on_rising_edge((pins & kSio) != 0);
}

void Gpio::on_rising_edge(bool sio) {
    switch (state_) {
        case State::Idle: return;
        case State::Command:
        case State::Write:
            shift_ = static_cast<std::uint8_t>(shift_ | (sio ? 1U << bit_ : 0U));
            if (++bit_ < 8) return;
            bit_ = 0;
            if (state_ == State::Command) {
                const std::uint8_t command = shift_;
                shift_ = 0;
                if ((command & 0x0F) != 0x06) {
                    state_ = State::Idle;  // not a command: ignore the rest of the transfer
                    return;
                }
                reg_ = (command >> 4) & 7;
                length_ = register_length(reg_);
                byte_ = 0;
                if ((command & 0x80) != 0) {
                    latch_time();
                    state_ = length_ > 0 ? State::Read : State::Idle;
                } else {
                    state_ = State::Write;
                    if (length_ == 0) finish_command();
                }
                return;
            }
            buffer_[byte_] = shift_;
            shift_ = 0;
            if (++byte_ >= length_) finish_command();
            return;
        case State::Read:
            if (++bit_ < 8) return;
            bit_ = 0;
            if (++byte_ >= length_) state_ = State::Idle;
            return;
    }
}

void Gpio::finish_command() {
    if (reg_ == 0) rtc_control_ = 0;   // reset
    if (reg_ == 4) rtc_control_ = buffer_[0];
    state_ = State::Idle;               // date/time writes are ignored: we follow the host
}

void Gpio::latch_time() {
    std::fill(std::begin(buffer_), std::end(buffer_), std::uint8_t{0});
    if (reg_ == 4) {
        buffer_[0] = rtc_control_;
        return;
    }
    const std::tm t = clock_();
    int hour = t.tm_hour;
    std::uint8_t pm = 0;
    if ((rtc_control_ & 0x40) == 0) {  // 12-hour mode: bit 7 of the hour is the PM flag
        pm = hour >= 12 ? 0x80 : 0;
        hour %= 12;
    }
    const std::uint8_t time[3] = {static_cast<std::uint8_t>(bcd(hour) | pm), bcd(t.tm_min), bcd(std::min(t.tm_sec, 59))};
    if (reg_ == 2) {
        buffer_[0] = bcd((t.tm_year + 1900) % 100);
        buffer_[1] = bcd(t.tm_mon + 1);
        buffer_[2] = bcd(t.tm_mday);
        buffer_[3] = bcd(t.tm_wday);
        std::copy(std::begin(time), std::end(time), buffer_ + 4);
    } else if (reg_ == 6) {
        std::copy(std::begin(time), std::end(time), buffer_);
    }
}

bool has_rtc(std::span<const std::uint8_t> rom) {
    constexpr std::string_view kId = "SIIRTC_V";
    const auto* begin = reinterpret_cast<const char*>(rom.data());
    return std::string_view(begin, rom.size()).find(kId) != std::string_view::npos;
}

}  // namespace ezy::gba
