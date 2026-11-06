#include "gb/hdma.hpp"

namespace ezy::gb {

void Hdma::write_address(std::uint16_t addr, std::uint8_t value) {
    switch (addr) {
        case 0xFF51: source_ = static_cast<std::uint16_t>((source_ & 0x00F0) | (value << 8)); break;
        case 0xFF52: source_ = static_cast<std::uint16_t>((source_ & 0xFF00) | (value & 0xF0)); break;
        case 0xFF53: dest_ = static_cast<std::uint16_t>((dest_ & 0x00F0) | ((value & 0x1F) << 8)); break;
        case 0xFF54: dest_ = static_cast<std::uint16_t>((dest_ & 0x1F00) | (value & 0xF0)); break;
        default: break;
    }
}

int Hdma::write_control(std::uint8_t value) {
    const int blocks = (value & 0x7F) + 1;
    if (value & 0x80) {
        active_ = true;
        remaining_ = static_cast<std::uint8_t>(blocks);
        return 0;
    }
    if (active_) {  // cancel: HDMA5 then reads bit 7 = 1 with the blocks that were left
        active_ = false;
        return 0;
    }
    remaining_ = 0;
    return blocks;
}

std::uint8_t Hdma::read_control() const {
    if (remaining_ == 0) return 0xFF;
    const auto left = static_cast<std::uint8_t>((remaining_ - 1) & 0x7F);
    return active_ ? left : static_cast<std::uint8_t>(0x80 | left);
}

Hdma::Block Hdma::next_block() {
    const Block block{source_, static_cast<std::uint16_t>(0x8000 | dest_)};
    source_ = static_cast<std::uint16_t>(source_ + 16);
    dest_ = static_cast<std::uint16_t>((dest_ + 16) & 0x1FF0);
    if (active_ && --remaining_ == 0) active_ = false;
    return block;
}

}  // namespace ezy::gb
