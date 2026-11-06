#include "gameboy_system.hpp"

#include <imgui.h>

#include <array>
#include <chrono>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <utility>

#include "debug_ui.hpp"

namespace ezy::frontend {

namespace {

using gb::Button;

// How often battery RAM is flushed to disk while running, if it changed (~1 s).
constexpr int kFlushIntervalFrames = 60;

std::int64_t unix_now() {
    using namespace std::chrono;
    return duration_cast<seconds>(system_clock::now().time_since_epoch()).count();
}

// The 4 shades of the original DMG screen, lightest to darkest.
constexpr std::array<std::uint32_t, 4> kShades = {0xFFE0F8D0, 0xFF88C070, 0xFF346856, 0xFF081820};

// The DMG model's greys are shown with the DMG's green tint; anything else is a CGB colour.
std::uint32_t to_argb(std::uint16_t color) {
    for (std::size_t shade = 0; shade < 4; ++shade) {
        if (gb::kDmgGreys[shade] == color) return kShades[shade];
    }
    const auto expand = [](std::uint32_t c) { return (c << 3) | (c >> 2); };
    return 0xFF000000 | (expand(color & 0x1F) << 16) | (expand((color >> 5) & 0x1F) << 8) |
           expand((color >> 10) & 0x1F);
}

constexpr std::array<std::pair<SDL_Scancode, Button>, 8> kKeymap = {{
    {SDL_SCANCODE_RIGHT, Button::Right},
    {SDL_SCANCODE_LEFT, Button::Left},
    {SDL_SCANCODE_UP, Button::Up},
    {SDL_SCANCODE_DOWN, Button::Down},
    {SDL_SCANCODE_X, Button::A},
    {SDL_SCANCODE_Z, Button::B},
    {SDL_SCANCODE_BACKSPACE, Button::Select},
    {SDL_SCANCODE_RETURN, Button::Start},
}};

void draw_cpu(const gb::GameBoy& gameboy) {
    debug_ui::place_window(800, 130, 470, 230);
    ImGui::Begin("CPU");
    const gb::Cpu& cpu = gameboy.cpu();
    const gb::Registers& r = cpu.regs();

    ImGui::Text("AF %04X   BC %04X", r.af(), r.bc());
    ImGui::Text("DE %04X   HL %04X", r.de(), r.hl());
    ImGui::Text("SP %04X", r.sp);
    ImGui::SameLine();
    ImGui::TextColored(debug_ui::kHighlightPc, "  PC %04X", r.pc);

    ImGui::Text("Flags  %c %c %c %c", r.flag(gb::flag::kZ) ? 'Z' : '-', r.flag(gb::flag::kN) ? 'N' : '-',
                r.flag(gb::flag::kH) ? 'H' : '-', r.flag(gb::flag::kC) ? 'C' : '-');
    ImGui::Text("IME %d   IE %02X   IF %02X", cpu.ime() ? 1 : 0, gameboy.bus().interrupt_enable(),
                gameboy.bus().interrupt_flags());
    if (cpu.halted()) ImGui::TextColored(debug_ui::kDim, "HALTED");
    if (cpu.locked()) ImGui::TextColored({1, 0.3F, 0.3F, 1}, "LOCKED (illegal opcode)");

    ImGui::SeparatorText("Next bytes at PC");
    for (int i = 0; i < 4; ++i) {
        if (i > 0) ImGui::SameLine();
        ImGui::Text("%02X", gameboy.bus().peek(static_cast<std::uint16_t>(r.pc + i)));
    }
    ImGui::End();
}

void draw_video(const gb::GameBoy& gameboy) {
    debug_ui::place_window(800, 370, 470, 340);
    ImGui::Begin("PPU / Cartridge");
    const gb::Ppu& ppu = gameboy.bus().ppu();
    const gb::Bus& bus = gameboy.bus();

    ImGui::Text("LCD %s   LY %3d   mode %d", ppu.lcd_on() ? "on " : "off", ppu.ly(), ppu.mode());
    ImGui::Text("LCDC %02X   STAT %02X", ppu.lcdc(), bus.peek(0xFF41));
    ImGui::Text("SCX %02X  SCY %02X  WX %02X  WY %02X", bus.peek(0xFF43), bus.peek(0xFF42), bus.peek(0xFF4B),
                bus.peek(0xFF4A));
    ImGui::Text("BGP %02X  OBP0 %02X  OBP1 %02X", bus.peek(0xFF47), bus.peek(0xFF48), bus.peek(0xFF49));

    ImGui::SeparatorText("Cartridge");
    const auto& header = bus.cartridge().header();
    ImGui::Text("%s", header.title.c_str());
    ImGui::TextColored(debug_ui::kDim, "%s, %zu KiB ROM, %zu KiB RAM",
                       std::string(gb::cart_type_name(header.cart_type)).c_str(), header.rom_size / 1024,
                       header.ram_size / 1024);

    ImGui::SeparatorText("Serial output");
    ImGui::BeginChild("serial", {0, 0}, ImGuiChildFlags_Borders);
    ImGui::TextWrapped("%s", bus.serial_output().c_str());
    ImGui::EndChild();
    ImGui::End();
}

}  // namespace

std::unique_ptr<GameBoySystem> GameBoySystem::create(std::span<const std::uint8_t> rom,
                                                     std::filesystem::path save_path) {
    auto gameboy = gb::GameBoy::create(rom);
    if (!gameboy) return nullptr;
    std::unique_ptr<GameBoySystem> system(
        new GameBoySystem({rom.begin(), rom.end()}, std::move(gameboy), std::move(save_path)));
    system->load_save();
    return system;
}

bool GameBoySystem::persists_save() const {
    const gb::Cartridge& cart = gameboy_->bus().cartridge();
    return !save_path_.empty() && cart.has_battery() && (!cart.ram().empty() || cart.has_rtc());
}

void GameBoySystem::load_save() {
    if (!persists_save()) return;
    std::ifstream file(save_path_, std::ios::binary);
    if (!file) return;  // first run: no save yet
    const std::vector<std::uint8_t> data(std::istreambuf_iterator<char>(file), {});
    if (!gameboy_->bus().cartridge().load_save_data(data)) {
        std::cerr << "Warning: " << save_path_.string() << " has " << data.size()
                  << " bytes, expected " << gameboy_->bus().cartridge().save_data().size() << "\n";
    }
}

void GameBoySystem::flush_save(bool force) {
    frames_since_flush_ = 0;
    if (!persists_save()) return;
    gb::Cartridge& cart = gameboy_->bus().cartridge();
    if (!cart.save_dirty() && !force) return;
    // Write to a temp file and rename, so a crash mid-write never corrupts the old save.
    std::filesystem::path tmp = save_path_;
    tmp += ".tmp";
    {
        std::ofstream file(tmp, std::ios::binary | std::ios::trunc);
        const auto data = cart.save_data(unix_now());
        file.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
        if (!file) {
            std::cerr << "Could not write " << tmp.string() << "\n";
            return;
        }
    }
    std::error_code ec;
    std::filesystem::rename(tmp, save_path_, ec);
    if (ec) {
        std::cerr << "Could not replace " << save_path_.string() << ": " << ec.message() << "\n";
        return;
    }
    cart.clear_save_dirty();
}

void GameBoySystem::run_frame() {
    gameboy_->run_frame();
    if (++frames_since_flush_ >= kFlushIntervalFrames) flush_save();
}

void GameBoySystem::reset() {
    flush_save(/*force=*/gameboy_->bus().cartridge().has_rtc());
    gameboy_ = gb::GameBoy::create(rom_);
    load_save();
}

void GameBoySystem::handle_key(SDL_Scancode scancode, bool pressed) {
    for (const auto& [key, button] : kKeymap) {
        if (key == scancode) gameboy_->set_button(button, pressed);
    }
}

void GameBoySystem::render(std::span<std::uint32_t> argb_pixels) const {
    const auto fb = gameboy_->framebuffer();
    for (std::size_t i = 0; i < argb_pixels.size(); ++i) {
        argb_pixels[i] = to_argb(fb[i]);
    }
}

void GameBoySystem::draw_debug_windows() {
    draw_cpu(*gameboy_);
    draw_video(*gameboy_);
}

}  // namespace ezy::frontend
