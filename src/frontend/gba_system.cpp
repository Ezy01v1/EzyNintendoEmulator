#include "gba_system.hpp"

#include "file_util.hpp"

#include <imgui.h>

#include <array>
#include <fstream>
#include <iostream>
#include <iterator>

#include "debug_ui.hpp"

namespace ezy::frontend {

namespace {

// How often the save chip is flushed to disk while running, if it changed (~1 s).
constexpr int kFlushIntervalFrames = 60;

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
    debug_ui::place_window(800, 440, 470, 250);
    ImGui::Begin("PPU / Cartridge");
    const gba::Bus& bus = gba.bus();
    ImGui::Text("DISPCNT %04X  mode %d", bus.read16(0x04000000), bus.read16(0x04000000) & 7);
    ImGui::Text("DISPSTAT %04X  VCOUNT %3d", bus.read16(0x04000004), bus.ppu().vcount());
    ImGui::Text("IE %04X  IF %04X  IME %d", bus.interrupt_enable(), bus.interrupt_flags(),
                bus.interrupt_master_enable() ? 1 : 0);
    const gba::Apu& apu = bus.apu();
    ImGui::SeparatorText("Sound");
    ImGui::Text("%s  PSG %c%c%c%c  FIFO A %2d  B %2d", apu.enabled() ? "on " : "off", apu.channel_on(0) ? '1' : '-',
                apu.channel_on(1) ? '2' : '-', apu.channel_on(2) ? '3' : '-', apu.channel_on(3) ? '4' : '-',
                apu.fifo_size(0), apu.fifo_size(1));
    ImGui::SeparatorText("Cartridge");
    ImGui::Text("%s  (%s)", gba.header().title.c_str(), gba.header().game_code.c_str());
    const gba::Backup& backup = bus.backup();
    const std::string_view kind = gba::backup_type_name(backup.type());
    ImGui::Text("Save: %.*s, %zu bytes%s", static_cast<int>(kind.size()), kind.data(), backup.data().size(),
                backup.dirty() ? " (unsaved changes)" : "");
    ImGui::End();
}

}  // namespace

std::unique_ptr<GbaSystem> GbaSystem::create(std::span<const std::uint8_t> rom, std::span<const std::uint8_t> bios,
                                             std::filesystem::path save_path) {
    auto gba = gba::Gba::create(rom, bios);
    if (!gba) return nullptr;
    std::unique_ptr<GbaSystem> system(new GbaSystem({rom.begin(), rom.end()}, {bios.begin(), bios.end()},
                                                    std::move(gba), std::move(save_path)));
    system->load_save();
    return system;
}

void GbaSystem::load_save() {
    if (save_path_.empty()) return;
    std::ifstream file(save_path_, std::ios::binary);
    if (!file) return;  // first run: no save yet
    const std::vector<std::uint8_t> data(std::istreambuf_iterator<char>(file), {});
    gba::Backup& backup = gba_->bus().backup();
    if (!backup.load(data)) {
        std::cerr << "Warning: " << save_path_.string() << " has " << data.size() << " bytes, which doesn't fit "
                  << gba::backup_type_name(backup.type()) << "; ignoring it\n";
    }
}


void GbaSystem::flush_save() {
    frames_since_flush_ = 0;
    gba::Backup& backup = gba_->bus().backup();
    if (save_path_.empty() || !backup.persists() || !backup.dirty()) return;
    if (write_file_atomically(save_path_, backup.data())) backup.clear_dirty();
}

void GbaSystem::save_state() {
    if (save_path_.empty()) return;
    const std::filesystem::path path = state_path(save_path_);
    if (write_file_atomically(path, gba_->save_state())) std::cerr << "Saved state to " << path.string() << "\n";
}

void GbaSystem::load_state() {
    if (save_path_.empty()) return;
    const std::filesystem::path path = state_path(save_path_);
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        std::cerr << "No save state at " << path.string() << "\n";
        return;
    }
    const std::vector<std::uint8_t> data(std::istreambuf_iterator<char>(file), {});
    if (!gba_->load_state(data)) {
        std::cerr << path.string() << " is not a save state of this game and version; ignoring it\n";
        return;
    }
    gba_->bus().set_keys(keys_);  // the keys held now, not the ones in the state
    std::cerr << "Loaded state from " << path.string() << "\n";
}

void GbaSystem::run_frame() {
    gba_->run_frame();
    if (++frames_since_flush_ >= kFlushIntervalFrames) flush_save();
}

void GbaSystem::reset() {
    flush_save();
    gba_ = gba::Gba::create(rom_, bios_);
    load_save();
    gba_->bus().set_keys(keys_);
}

void GbaSystem::handle_key(SDL_Scancode scancode, bool pressed) {
    if (pressed && scancode == SDL_SCANCODE_F2) save_state();
    if (pressed && scancode == SDL_SCANCODE_F4) load_state();
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
