#include "gb/gameboy.hpp"

namespace ezy::gb {

std::unique_ptr<GameBoy> GameBoy::create(std::span<const std::uint8_t> rom) {
    auto cartridge = Cartridge::load(rom);
    if (!cartridge) return nullptr;
    return std::unique_ptr<GameBoy>(new GameBoy(std::move(*cartridge)));
}

GameBoy::GameBoy(Cartridge cartridge) : bus_(std::move(cartridge)), cpu_(bus_) { cpu_.reset_post_boot(); }

void GameBoy::run_frame() {
    const std::uint64_t start = bus_.t_cycles();
    while (bus_.t_cycles() - start < kDotsPerFrame) {
        cpu_.step();
        if (bus_.ppu().take_frame_ready()) return;
    }
}

}  // namespace ezy::gb
