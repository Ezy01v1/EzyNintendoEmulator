// Headless Game Boy runner: executes a ROM and prints everything it sends over the
// serial port. Blargg's test ROMs report "Passed" / "Failed" there.
//
// Usage: ezy_gb_runner <rom.gb> [max-seconds]

#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <vector>

#include "gb/bus.hpp"
#include "gb/cartridge.hpp"
#include "gb/cpu.hpp"

int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::cerr << "Usage: ezy_gb_runner <rom.gb> [max-seconds]\n";
        return 2;
    }
    std::ifstream file(argv[1], std::ios::binary);
    const std::vector<std::uint8_t> rom(std::istreambuf_iterator<char>(file), {});
    auto cartridge = ezy::gb::Cartridge::load(rom);
    if (!cartridge) {
        std::cerr << "Not a valid Game Boy ROM: " << argv[1] << '\n';
        return 2;
    }
    std::cout << "Title: " << cartridge->header().title << " ("
              << ezy::gb::cart_type_name(cartridge->header().cart_type) << ")\n";

    constexpr std::uint64_t kClockHz = 4'194'304;  // T-cycles per emulated second
    const double max_seconds = argc > 2 ? std::atof(argv[2]) : 120.0;
    const auto max_cycles = static_cast<std::uint64_t>(max_seconds * kClockHz);

    ezy::gb::Bus bus(std::move(*cartridge));
    ezy::gb::Cpu cpu(bus);
    cpu.reset_post_boot();

    std::size_t printed = 0;
    while (bus.t_cycles() < max_cycles && !cpu.locked()) {
        cpu.step();
        const std::string& out = bus.serial_output();
        if (out.size() > printed) {
            std::cout << out.substr(printed) << std::flush;
            printed = out.size();
            if (out.find("Passed") != std::string::npos || out.find("Failed") != std::string::npos) break;
        }
    }

    const std::string& out = bus.serial_output();
    std::cout << "\n[" << static_cast<double>(bus.t_cycles()) / kClockHz << " emulated s]\n";
    if (cpu.locked()) std::cout << "CPU locked at illegal opcode, PC=" << std::hex << cpu.regs().pc << '\n';
    return out.find("Passed") != std::string::npos ? 0 : 1;
}
