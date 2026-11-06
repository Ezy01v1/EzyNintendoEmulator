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
#include "file_util.hpp"
#include "gb/color.hpp"

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

// RGB555 -> ARGB for every colour, with or without the GBC screen correction.
struct ColorTable {
    std::array<std::uint32_t, 0x8000> plain{};
    std::array<std::uint32_t, 0x8000> corrected{};
    ColorTable() {
        for (std::uint32_t c = 0; c < 0x8000; ++c) {
            plain[c] = gb::rgb555_to_argb(static_cast<std::uint16_t>(c));
            corrected[c] = gb::correct_cgb_color(static_cast<std::uint16_t>(c));
        }
    }
};

const ColorTable& color_table() {
    static const ColorTable table;
    return table;
}

// The DMG model shows its shades with the DMG's green tint.
std::uint32_t dmg_to_argb(std::uint16_t color) {
    for (std::size_t shade = 0; shade < 4; ++shade) {
        if (gb::kDmgGreys[shade] == color) return kShades[shade];
    }
    return color_table().plain[color & 0x7FFF];
}

ImU32 to_imgui(std::uint16_t color) {
    const std::uint32_t argb = color_table().plain[color & 0x7FFF];
    return IM_COL32((argb >> 16) & 0xFF, (argb >> 8) & 0xFF, argb & 0xFF, 0xFF);
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

// 8 palettes of 4 colours as swatches.
void draw_palettes(const char* label, std::span<const std::uint8_t, 64> ram) {
    ImGui::TextUnformatted(label);
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    constexpr float kSize = 12.0F;
    for (int palette = 0; palette < 8; ++palette) {
        for (int i = 0; i < 4; ++i) {
            const auto at = static_cast<std::size_t>(palette * 8 + i * 2);
            const auto color = static_cast<std::uint16_t>(ram[at] | (ram[at + 1] << 8));
            const ImVec2 p0{origin.x + static_cast<float>(palette * 5 + i) * kSize, origin.y};
            draw->AddRectFilled(p0, {p0.x + kSize - 1, p0.y + kSize - 1}, to_imgui(color));
        }
    }
    ImGui::Dummy({8 * 5 * kSize, kSize});
}

// The 384 tiles of one VRAM bank, drawn with BG palette 0 (or greys on a DMG).
void draw_tiles(const gb::GameBoy& gameboy, int bank) {
    const gb::Ppu& ppu = gameboy.bus().ppu();
    const auto ram = ppu.bg_palette_ram();
    std::array<ImU32, 4> colors{};
    for (std::size_t i = 0; i < 4; ++i) {
        const auto color = gameboy.model() == gb::Model::Cgb
                               ? static_cast<std::uint16_t>(ram[i * 2] | (ram[i * 2 + 1] << 8))
                               : gb::kDmgGreys[i];
        colors[i] = to_imgui(color);
    }
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    constexpr int kPerRow = 16;
    constexpr float kScale = 2.0F;
    for (int tile = 0; tile < 384; ++tile) {
        const float tx = origin.x + static_cast<float>((tile % kPerRow) * 8) * kScale;
        const float ty = origin.y + static_cast<float>((tile / kPerRow) * 8) * kScale;
        for (int row = 0; row < 8; ++row) {
            const auto addr = static_cast<std::uint16_t>(0x8000 + tile * 16 + row * 2);
            const std::uint8_t lo = ppu.read_vram_bank(bank, addr);
            const std::uint8_t hi = ppu.read_vram_bank(bank, static_cast<std::uint16_t>(addr + 1));
            for (int col = 0; col < 8; ++col) {
                const int bit = 7 - col;
                const int index = (((hi >> bit) & 1) << 1) | ((lo >> bit) & 1);
                const ImVec2 p0{tx + static_cast<float>(col) * kScale, ty + static_cast<float>(row) * kScale};
                draw->AddRectFilled(p0, {p0.x + kScale, p0.y + kScale}, colors[static_cast<std::size_t>(index)]);
            }
        }
    }
    ImGui::Dummy({kPerRow * 8 * kScale, (384 / kPerRow) * 8 * kScale});
}

void draw_vram(const gb::GameBoy& gameboy, int& bank) {
    debug_ui::place_window(10, 370, 300, 340);
    ImGui::Begin("VRAM");
    if (gameboy.model() == gb::Model::Cgb) {
        draw_palettes("BG palettes", gameboy.bus().ppu().bg_palette_ram());
        draw_palettes("OBJ palettes", gameboy.bus().ppu().obj_palette_ram());
        ImGui::RadioButton("Bank 0", &bank, 0);
        ImGui::SameLine();
        ImGui::RadioButton("Bank 1", &bank, 1);
    } else {
        bank = 0;
    }
    draw_tiles(gameboy, bank);
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

    if (gameboy.model() == gb::Model::Cgb) {
        ImGui::SeparatorText("Game Boy Color");
        ImGui::Text("%s   %s speed", bus.cgb_mode() ? "CGB mode" : "DMG compatibility mode",
                    bus.double_speed() ? "double" : "normal");
        ImGui::Text("VBK %d   SVBK %d   KEY1 %02X   OPRI %d", ppu.vram_bank(), bus.wram_bank(), bus.peek(0xFF4D),
                    bus.opri());
        const std::uint8_t hdma5 = bus.peek(0xFF55);
        if (hdma5 == 0xFF) {
            ImGui::Text("VRAM DMA idle");
        } else {
            ImGui::Text("VRAM DMA %s, %d blocks left", (hdma5 & 0x80) ? "stopped" : "HBlank", (hdma5 & 0x7F) + 1);
        }
    }

    ImGui::SeparatorText("Serial output");
    ImGui::BeginChild("serial", {0, 0}, ImGuiChildFlags_Borders);
    ImGui::TextWrapped("%s", bus.serial_output().c_str());
    ImGui::EndChild();
    ImGui::End();
}

}  // namespace

std::unique_ptr<GameBoySystem> GameBoySystem::create(std::span<const std::uint8_t> rom,
                                                     std::filesystem::path save_path, GameBoySettings settings) {
    auto gameboy = gb::GameBoy::create(rom, gb::Options{.model = settings.model, .boot_rom = settings.boot_rom});
    if (!gameboy) return nullptr;
    std::unique_ptr<GameBoySystem> system(
        new GameBoySystem({rom.begin(), rom.end()}, std::move(gameboy), std::move(save_path), std::move(settings)));
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
    // Like the real boot ROM, a direction (+ A or B) held at power-on picks the palette of
    // a DMG game.
    const bool* keys = SDL_GetKeyboardState(nullptr);
    const gb::HeldButtons held{.right = keys[SDL_SCANCODE_RIGHT],
                               .left = keys[SDL_SCANCODE_LEFT],
                               .up = keys[SDL_SCANCODE_UP],
                               .down = keys[SDL_SCANCODE_DOWN],
                               .a = keys[SDL_SCANCODE_X],
                               .b = keys[SDL_SCANCODE_Z]};
    gameboy_ = gb::GameBoy::create(rom_, gb::Options{.model = settings_.model,
                                                     .boot_rom = settings_.boot_rom,
                                                     .held_buttons = held});
    load_save();
    sync_held_keys();
}

void GameBoySystem::sync_held_keys() {
    const bool* keys = SDL_GetKeyboardState(nullptr);
    for (const auto& [key, button] : kKeymap) gameboy_->set_button(button, keys[key]);
}

void GameBoySystem::save_state() {
    if (save_path_.empty()) return;
    const std::filesystem::path path = state_path(save_path_);
    if (write_file_atomically(path, gameboy_->save_state())) std::cerr << "Saved state to " << path.string() << "\n";
}

void GameBoySystem::load_state() {
    if (save_path_.empty()) return;
    const std::filesystem::path path = state_path(save_path_);
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        std::cerr << "No save state at " << path.string() << "\n";
        return;
    }
    const std::vector<std::uint8_t> data(std::istreambuf_iterator<char>(file), {});
    if (!gameboy_->load_state(data)) {
        std::cerr << path.string() << " is not a save state of this game, model and version; ignoring it\n";
        return;
    }
    sync_held_keys();  // the keys held now, not the ones in the state
    std::cerr << "Loaded state from " << path.string() << "\n";
}

void GameBoySystem::handle_key(SDL_Scancode scancode, bool pressed) {
    if (pressed && scancode == SDL_SCANCODE_F2) save_state();
    if (pressed && scancode == SDL_SCANCODE_F4) load_state();
    for (const auto& [key, button] : kKeymap) {
        if (key == scancode) gameboy_->set_button(button, pressed);
    }
}

void GameBoySystem::render(std::span<std::uint32_t> argb_pixels) const {
    const auto fb = gameboy_->framebuffer();
    if (gameboy_->model() == gb::Model::Dmg) {
        for (std::size_t i = 0; i < argb_pixels.size(); ++i) argb_pixels[i] = dmg_to_argb(fb[i]);
        return;
    }
    const auto& table = settings_.color_correction ? color_table().corrected : color_table().plain;
    for (std::size_t i = 0; i < argb_pixels.size(); ++i) argb_pixels[i] = table[fb[i] & 0x7FFF];
}

void GameBoySystem::draw_debug_windows() {
    draw_cpu(*gameboy_);
    draw_video(*gameboy_);
    draw_vram(*gameboy_, debug_vram_bank_);
}

}  // namespace ezy::frontend
