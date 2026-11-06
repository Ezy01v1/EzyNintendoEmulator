#include "gb/gameboy.hpp"

#include "gb/compat_palettes.hpp"

namespace ezy::gb {

namespace {

// What the CGB boot ROM leaves in B for a DMG game: the sum of the 16 title bytes, but
// only for Nintendo-published games (the only ones it has palettes for).
std::uint8_t compat_title_checksum(const CartridgeHeader& header) {
    if (!header.nintendo_licensee()) return 0;
    std::uint8_t sum = 0;
    for (const auto byte : header.title_bytes) sum = static_cast<std::uint8_t>(sum + byte);
    return sum;
}

}  // namespace

std::unique_ptr<GameBoy> GameBoy::create(std::span<const std::uint8_t> rom, Options options) {
    auto cartridge = Cartridge::load(rom);
    if (!cartridge) return nullptr;
    const Model model = resolve_model(options.model);
    constexpr std::size_t kCgbBootRomSize = 0x900;
    const bool real_boot = options.boot_rom && model == Model::Cgb;
    if (real_boot && options.boot_rom->size() != kCgbBootRomSize) return nullptr;

    auto gameboy = std::unique_ptr<GameBoy>(new GameBoy(std::move(*cartridge), model));
    if (real_boot) {
        // Start from scratch at 0x0000 and let Nintendo's code do the rest.
        gameboy->bus().map_boot_rom(std::move(*options.boot_rom));
        gameboy->cpu().regs() = Registers{};
        return gameboy;
    }
    if (gameboy->model() == Model::Cgb && !gameboy->bus().cgb_mode()) {
        // What the CGB boot ROM does for a DMG game: pick its colours.
        const CompatPalette p = select_compat_palette(gameboy->bus().cartridge().header(), options.held_buttons);
        gameboy->bus().ppu().set_compat_palettes(p.bg, p.obj0, p.obj1);
    }
    return gameboy;
}

GameBoy::GameBoy(Cartridge cartridge, Model model) : bus_(std::move(cartridge), model), cpu_(bus_) {
    cpu_.reset_post_boot(model, bus_.cgb_mode(), compat_title_checksum(bus_.cartridge().header()));
}

void GameBoy::run_frame() {
    const std::uint64_t start = bus_.dots();
    while (bus_.dots() - start < kDotsPerFrame) {
        cpu_.step();
        if (bus_.ppu().take_frame_ready()) return;
    }
}

}  // namespace ezy::gb
