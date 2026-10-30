// Runs the SingleStepTests/sm83 suite (https://github.com/SingleStepTests/sm83, MIT):
// one JSON file per opcode, each with ~1000 cases of initial state -> final state plus
// the exact bus activity of every M-cycle.
//
// Usage: ezy_gb_sst <path/to/sm83/v1> [file-filter]

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <memory>
#include <filesystem>
#include <format>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

#include "gb/cpu.hpp"

namespace {

using ezy::gb::Cpu;
using ezy::gb::CpuBus;
using ezy::gb::Registers;
using json = nlohmann::json;

// Flat memory that logs every M-cycle, so the cycle list can be checked too.
class RecordingBus final : public CpuBus {
public:
    struct Access {
        std::uint16_t addr;
        char kind;  // 'r', 'w' or '-' (internal)
    };

    std::array<std::uint8_t, 0x10000> memory{};
    std::vector<Access> log;

    std::uint8_t read(std::uint16_t addr) override {
        log.push_back({addr, 'r'});
        return memory[addr];
    }
    void write(std::uint16_t addr, std::uint8_t value) override {
        log.push_back({addr, 'w'});
        memory[addr] = value;
    }
    void tick() override { log.push_back({0, '-'}); }
    [[nodiscard]] std::uint8_t pending_interrupts() const override { return 0; }
    void acknowledge_interrupt(std::uint8_t /*bit*/) override {}
};

void load_state(const json& s, Registers& r) {
    r.a = s["a"];
    r.f = s["f"];
    r.b = s["b"];
    r.c = s["c"];
    r.d = s["d"];
    r.e = s["e"];
    r.h = s["h"];
    r.l = s["l"];
    r.sp = s["sp"];
    r.pc = s["pc"];
}

// Returns an empty string on success, or a description of the first mismatch.
std::string run_case(const json& test, RecordingBus& bus) {
    const json& init = test["initial"];
    const json& want = test["final"];

    for (const auto& entry : init["ram"]) bus.memory[entry[0].get<std::uint16_t>()] = entry[1];
    bus.log.clear();

    Cpu cpu(bus);
    load_state(init, cpu.regs());
    cpu.set_ime(init["ime"].get<int>() != 0);
    cpu.step();

    std::string error;
    const Registers& r = cpu.regs();
    auto check = [&](const char* name, unsigned got, unsigned expected) {
        if (error.empty() && got != expected) error = std::format("{}: got {:X}, want {:X}", name, got, expected);
    };
    check("a", r.a, want["a"]);
    check("f", r.f, want["f"]);
    check("b", r.b, want["b"]);
    check("c", r.c, want["c"]);
    check("d", r.d, want["d"]);
    check("e", r.e, want["e"]);
    check("h", r.h, want["h"]);
    check("l", r.l, want["l"]);
    check("sp", r.sp, want["sp"]);
    check("pc", r.pc, want["pc"]);
    check("ime", cpu.ime() ? 1U : 0U, want["ime"]);
    for (const auto& entry : want["ram"]) {
        const std::uint16_t addr = entry[0];
        check(std::format("ram[{:04X}]", addr).c_str(), bus.memory[addr], entry[1]);
    }

    const json& cycles = test["cycles"];
    if (error.empty() && bus.log.size() != cycles.size()) {
        error = std::format("cycles: got {}, want {}", bus.log.size(), cycles.size());
    }
    for (std::size_t i = 0; error.empty() && i < cycles.size(); ++i) {
        const std::string pins = cycles[i][2];
        const char kind = pins[0] == 'r' ? 'r' : (pins[1] == 'w' ? 'w' : '-');
        if (bus.log[i].kind != kind) {
            error = std::format("cycle {}: got '{}', want '{}'", i, bus.log[i].kind, kind);
        } else if (kind != '-' && bus.log[i].addr != cycles[i][0].get<std::uint16_t>()) {
            error = std::format("cycle {}: addr {:04X}, want {:04X}", i, bus.log[i].addr,
                                cycles[i][0].get<std::uint16_t>());
        }
    }

    // Leave memory zeroed for the next case.
    for (const auto& entry : init["ram"]) bus.memory[entry[0].get<std::uint16_t>()] = 0;
    for (const auto& entry : want["ram"]) bus.memory[entry[0].get<std::uint16_t>()] = 0;
    return error;
}

}  // namespace

int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::cerr << "Usage: ezy_gb_sst <path/to/sm83/v1> [file-filter]\n";
        return 2;
    }
    const std::filesystem::path dir = argv[1];
    const std::string filter = argc > 2 ? argv[2] : "";

    // HALT and STOP enter low-power states; the suite records them as a fixed number of
    // idle cycles, which a one-instruction step() cannot express. They are covered by our
    // unit tests and by Blargg's ROMs instead.
    constexpr std::array<std::string_view, 2> kSkipped = {"10.json", "76.json"};

    std::vector<std::filesystem::path> files;
    for (const auto& entry : std::filesystem::directory_iterator(dir)) {
        const auto name = entry.path().filename().string();
        if (std::ranges::find(kSkipped, name) != kSkipped.end()) {
            std::cout << "SKIP " << name << " (low-power instruction)\n";
            continue;
        }
        if (entry.path().extension() == ".json" && name.find(filter) != std::string::npos) {
            files.push_back(entry.path());
        }
    }
    std::ranges::sort(files);

    auto bus = std::make_unique<RecordingBus>();
    int failed_files = 0;
    std::size_t total = 0;
    std::size_t failed = 0;
    for (const auto& path : files) {
        std::ifstream in(path);
        const json tests = json::parse(in);
        std::size_t file_failed = 0;
        std::string first_error;
        for (const auto& test : tests) {
            const std::string error = run_case(test, *bus);
            if (!error.empty()) {
                if (file_failed == 0) first_error = std::format("{}: {}", test["name"].get<std::string>(), error);
                ++file_failed;
            }
        }
        total += tests.size();
        failed += file_failed;
        if (file_failed > 0) {
            ++failed_files;
            std::cout << std::format("FAIL {:<12} {:>4}/{}  first: {}\n", path.filename().string(), file_failed,
                                     tests.size(), first_error);
        }
    }

    std::cout << std::format("\n{} files, {} cases, {} failed cases in {} files\n", files.size(), total, failed,
                             failed_files);
    return failed == 0 ? 0 : 1;
}
