#include "gba_system.hpp"

#include <imgui.h>

#include <array>

#include "debug_ui.hpp"

namespace ezy::frontend {

namespace {

// KEYINPUT bits: A, B, Select, Start, Right, Left, Up, Down, R, L.
struct KeyBinding {
    SDL_Scancode key;
    std::uint16_t bit;
};

constexpr std::array<KeyBinding, 10> kKeymap = {{
    {SDL_SCANCODE_X, 1},
    {SDL_SCANCODE_Z, 2},
    {SDL_SCANCODE_BACKSPACE, 4},
    {SDL_SCANCODE_RETURN, 8},
    {SDL_SCANCODE_RIGHT, 16},
    {SDL_SCANCODE_LEFT, 32},
    {SDL_SCANCODE_UP, 64},
    {SDL_SCANCODE_DOWN, 128},
    {SDL_SCANCODE_S, 256},
    {SDL_SCANCODE_A, 512},
}};

constexpr std::uint32_t expand5(std::uint32_t c5) { return (c5 << 3) | (c5 >> 2); }

// BGR555 -> ARGB8888.
constexpr std::uint32_t to_argb(std::uint16_t c) {
    return 0xFF000000U | (expand5(c & 0x1F) << 16) | (expand5((c >> 5) & 0x1F) << 8) | expand5((c >> 10) & 0x1F);
}

void draw_cpu(const gba::Gba& gba) {
    debug_ui::place_window(800, 130, 470, 300);
    ImGui::Begin("ARM7TDMI");
    const gba::Arm7tdmi& cpu = gba.cpu();
    for (std::size_t i = 0; i < 16; i += 2) {
        ImGui::Text("r%-2zu %08X   r%-2zu %08X", i, cpu.reg(i), i + 1, cpu.reg(i + 1));
    }
    const std::uint32_t cpsr = cpu.cpsr();
    ImGui::Text("CPSR %08X  %c%c%c%c  %s  mode %02X", cpsr, (cpsr >> 31) & 1 ? 'N' : '-', (cpsr >> 30) & 1 ? 'Z' : '-',
                (cpsr >> 29) & 1 ? 'C' : '-', (cpsr >> 28) & 1 ? 'V' : '-', cpu.thumb() ? "Thumb" : "ARM",
                cpsr & 0x1F);
    ImGui::TextColored(debug_ui::kHighlightPc, "executing %08X", cpu.reg(15) - (cpu.thumb() ? 4U : 8U));
    ImGui::Text("cycles %llu   frames %llu", static_cast<unsigned long long>(gba.scheduler().now()),
                static_cast<unsigned long long>(gba.frame_count()));
    ImGui::End();
}

void draw_video(const gba::Gba& gba) {
    debug_ui::place_window(800, 440, 470, 200);
    ImGui::Begin("PPU / Cartridge");
    const gba::Bus& bus = gba.bus();
    ImGui::Text("DISPCNT %04X  mode %d", bus.read16(0x04000000), bus.read16(0x04000000) & 7);
    ImGui::Text("DISPSTAT %04X  VCOUNT %3d", bus.read16(0x04000004), bus.ppu().vcount());
    ImGui::Text("IE %04X  IF %04X  IME %d", bus.interrupt_enable(), bus.interrupt_flags(),
                bus.interrupt_master_enable() ? 1 : 0);
    ImGui::SeparatorText("Cartridge");
    ImGui::Text("%s  (%s)", gba.header().title.c_str(), gba.header().game_code.c_str());
    ImGui::End();
}

}  // namespace

std::unique_ptr<GbaSystem> GbaSystem::create(std::span<const std::uint8_t> rom, std::span<const std::uint8_t> bios) {
    auto gba = gba::Gba::create(rom, bios);
    if (!gba) return nullptr;
    return std::unique_ptr<GbaSystem>(
        new GbaSystem({rom.begin(), rom.end()}, {bios.begin(), bios.end()}, std::move(gba)));
}

void GbaSystem::reset() {
    gba_ = gba::Gba::create(rom_, bios_);
    gba_->bus().set_keys(keys_);
}

void GbaSystem::handle_key(SDL_Scancode scancode, bool pressed) {
    for (const auto& [key, bit] : kKeymap) {
        if (key != scancode) continue;
        keys_ = static_cast<std::uint16_t>(pressed ? keys_ | bit : keys_ & ~bit);
        gba_->bus().set_keys(keys_);
    }
}

void GbaSystem::render(std::span<std::uint32_t> argb_pixels) const {
    const auto fb = gba_->bus().ppu().framebuffer();
    for (std::size_t i = 0; i < argb_pixels.size(); ++i) argb_pixels[i] = to_argb(fb[i]);
}

void GbaSystem::draw_debug_windows() {
    draw_cpu(*gba_);
    draw_video(*gba_);
}

}  // namespace ezy::frontend
