// Runs the SingleStepTests/ARM7TDMI suite (https://github.com/SingleStepTests/ARM7TDMI,
// MIT, generated from NanoBoyAdvance): one .json.bin file per encoding class, each with
// thousands of cases of initial state -> one instruction -> final state + bus transactions.
//
// Reads the binary files directly (layout documented in the repo's transcode_json.py).
// Memory is not simulated: reads are answered from the recorded transactions, as the
// suite's README suggests, and writes are checked against them.
//
// Usage: ezy_gba_sst <path/to/ARM7TDMI/v1> [file-filter] [max-failures-shown]

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <vector>

#include "gba/cpu/arm7tdmi.hpp"

namespace {

using namespace ezy::gba;

struct State {
    std::array<std::uint32_t, 16> r{};
    std::array<std::uint32_t, 7> fiq{};  // r8-r14
    std::array<std::array<std::uint32_t, 2>, 4> r13_r14{};  // svc, abt, irq, und
    std::uint32_t cpsr = 0;
    std::array<std::uint32_t, 5> spsr{};  // fiq, svc, abt, irq, und
    std::array<std::uint32_t, 2> pipeline{};
};

struct Transaction {
    std::uint32_t kind = 0;  // 0 opcode fetch, 1 data read, 2 write
    std::uint32_t size = 0;
    std::uint32_t addr = 0;
    std::uint32_t data = 0;
};

struct Test {
    State initial;
    State final;
    std::vector<Transaction> transactions;
    std::uint32_t opcode = 0;
    std::uint32_t base_addr = 0;
};

// Banks in the suite's order (after FIQ): svc, abt, irq, und.
constexpr std::array<Bank, 4> kSuiteBanks = {kBankSvc, kBankAbt, kBankIrq, kBankUnd};
constexpr std::array<Bank, 5> kSuiteSpsrBanks = {kBankFiq, kBankSvc, kBankAbt, kBankIrq, kBankUnd};

class Reader {
public:
    explicit Reader(const std::vector<std::uint8_t>& data) : data_(data) {}
    std::uint32_t u32(std::size_t at) const {
        std::uint32_t v = 0;
        std::memcpy(&v, data_.data() + at, 4);
        return v;
    }

private:
    const std::vector<std::uint8_t>& data_;
};

std::size_t read_state(const Reader& in, std::size_t at, State& s) {
    const std::uint32_t size = in.u32(at);
    std::size_t p = at + 8;
    auto next = [&] {
        const auto v = in.u32(p);
        p += 4;
        return v;
    };
    for (auto& v : s.r) v = next();
    for (auto& v : s.fiq) v = next();
    for (auto& pair : s.r13_r14) pair = {next(), next()};
    s.cpsr = next();
    for (auto& v : s.spsr) v = next();
    s.pipeline = {next(), next()};
    return size;
}

std::vector<Test> load_tests(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    const std::vector<std::uint8_t> data(std::istreambuf_iterator<char>(file), {});
    const Reader in(data);
    if (data.size() < 8 || in.u32(0) != 0xD33DBAE0) return {};
    const std::uint32_t count = in.u32(4);
    std::vector<Test> tests(count);
    std::size_t p = 8;
    for (auto& t : tests) {
        const std::uint32_t full = in.u32(p);
        std::size_t q = p + 4;
        q += read_state(in, q, t.initial);
        q += read_state(in, q, t.final);
        const std::uint32_t tsize = in.u32(q);
        const std::uint32_t num = in.u32(q + 8);
        for (std::uint32_t i = 0; i < num; ++i) {
            const std::size_t e = q + 12 + i * 24;
            t.transactions.push_back({in.u32(e), in.u32(e + 4), in.u32(e + 8), in.u32(e + 12)});
        }
        q += tsize;
        t.opcode = in.u32(q + 8);
        t.base_addr = in.u32(q + 12);
        p += full;
    }
    return tests;
}

// Answers reads from the recorded transactions and records writes.
class ReplayBus final : public CpuBus {
public:
    explicit ReplayBus(const Test& test) : test_(test) {}

    std::uint8_t read8(std::uint32_t addr, Access) override { return static_cast<std::uint8_t>(data_read(addr, 1)); }
    std::uint16_t read16(std::uint32_t addr, Access access) override {
        return static_cast<std::uint16_t>((access & kCode) != 0 ? fetch(addr, 2) : data_read(addr, 2));
    }
    std::uint32_t read32(std::uint32_t addr, Access access) override {
        return (access & kCode) != 0 ? fetch(addr, 4) : data_read(addr, 4);
    }
    void write8(std::uint32_t addr, std::uint8_t v, Access) override { writes.push_back({2, 1, addr, v}); }
    void write16(std::uint32_t addr, std::uint16_t v, Access) override { writes.push_back({2, 2, addr, v}); }
    void write32(std::uint32_t addr, std::uint32_t v, Access) override { writes.push_back({2, 4, addr, v}); }

    std::vector<Transaction> writes;

private:
    std::uint32_t fetch(std::uint32_t addr, std::uint32_t size) const {
        for (const auto& t : test_.transactions) {
            if (t.kind == 0 && t.size == size && t.addr == addr) return t.data;
        }
        return addr;
    }
    std::uint32_t data_read(std::uint32_t addr, std::uint32_t size) const {
        for (const auto& t : test_.transactions) {
            if (t.kind == 1 && t.size == size && (t.addr & ~(size - 1)) == (addr & ~(size - 1))) return t.data;
        }
        return 0;
    }
    const Test& test_;
};

// The suite's convention (NanoBoyAdvance's): R[8..14] always hold the User bank, and each
// mode's bank holds that mode's registers, including the current mode's.
void load_cpu(Arm7tdmi& cpu, const State& s) {
    for (std::size_t i = 0; i < 7; ++i) {
        cpu.set_banked(kBankUser, i, s.r[8 + i]);
        cpu.set_banked(kBankFiq, i, s.fiq[i]);
    }
    for (std::size_t b = 0; b < 4; ++b) {
        cpu.set_banked(kSuiteBanks[b], 5, s.r13_r14[b][0]);
        cpu.set_banked(kSuiteBanks[b], 6, s.r13_r14[b][1]);
    }
    for (std::size_t b = 0; b < 5; ++b) cpu.set_spsr(kSuiteSpsrBanks[b], s.spsr[b]);
    cpu.set_cpsr_raw(s.cpsr);

    const Bank current = bank_of(s.cpsr);
    for (std::size_t i = 0; i < 16; ++i) cpu.set_reg(i, s.r[i]);
    for (std::size_t i = 8; i < 15; ++i) {
        const bool from_fiq = current == kBankFiq;
        const bool banked_r13_r14 = i >= 13 && current != kBankUser;
        if (from_fiq) {
            cpu.set_reg(i, s.fiq[i - 8]);
        } else if (banked_r13_r14) {
            cpu.set_reg(i, cpu.banked(current, i - 8));
        }
    }
    cpu.set_pipeline(s.pipeline[0], s.pipeline[1]);
}

// The CPU's state in the suite's convention (the inverse of load_cpu).
State save_cpu(const Arm7tdmi& cpu) {
    State s;
    const Bank current = bank_of(cpu.cpsr());
    for (std::size_t i = 0; i < 16; ++i) s.r[i] = cpu.reg(i);
    for (std::size_t i = 0; i < 7; ++i) {
        s.fiq[i] = current == kBankFiq ? cpu.reg(8 + i) : cpu.banked(kBankFiq, i);
        const bool live = current == kBankUser || (current != kBankFiq && i < 5);
        s.r[8 + i] = live ? cpu.reg(8 + i) : cpu.banked(kBankUser, i);
    }
    for (std::size_t b = 0; b < 4; ++b) {
        const Bank bank = kSuiteBanks[b];
        s.r13_r14[b] = bank == current ? std::array{cpu.reg(13), cpu.reg(14)}
                                       : std::array{cpu.banked(bank, 5), cpu.banked(bank, 6)};
    }
    s.cpsr = cpu.cpsr();
    for (std::size_t b = 0; b < 5; ++b) s.spsr[b] = cpu.spsr(kSuiteSpsrBanks[b]);
    s.pipeline = cpu.pipeline();
    return s;
}

// Empty string if the CPU matches the expected final state.
std::string compare(const Arm7tdmi& cpu, const Test& t, const ReplayBus& bus) {
    std::string diff;
    const State got = save_cpu(cpu);
    const State& f = t.final;
    for (std::size_t i = 0; i < 16; ++i) {
        if (got.r[i] != f.r[i]) diff += std::format(" r{}={:08X}(exp {:08X})", i, got.r[i], f.r[i]);
    }
    for (std::size_t i = 0; i < 7; ++i) {
        if (got.fiq[i] != f.fiq[i]) diff += std::format(" fiq_r{}={:08X}(exp {:08X})", 8 + i, got.fiq[i], f.fiq[i]);
    }
    constexpr std::array<const char*, 4> kNames = {"svc", "abt", "irq", "und"};
    for (std::size_t b = 0; b < 4; ++b) {
        for (std::size_t k = 0; k < 2; ++k) {
            if (got.r13_r14[b][k] != f.r13_r14[b][k]) {
                diff += std::format(" {}_r{}={:08X}(exp {:08X})", kNames[b], 13 + k, got.r13_r14[b][k], f.r13_r14[b][k]);
            }
        }
    }
    if (got.cpsr != f.cpsr) diff += std::format(" cpsr={:08X}(exp {:08X})", got.cpsr, f.cpsr);
    for (std::size_t b = 0; b < 5; ++b) {
        if (got.spsr[b] != f.spsr[b]) diff += std::format(" spsr[{}]={:08X}(exp {:08X})", b, got.spsr[b], f.spsr[b]);
    }
    if (got.pipeline != f.pipeline) {
        diff += std::format(" pipe={:08X},{:08X}(exp {:08X},{:08X})", got.pipeline[0], got.pipeline[1],
                            f.pipeline[0], f.pipeline[1]);
    }
    std::vector<Transaction> expected;
    std::ranges::copy_if(t.transactions, std::back_inserter(expected), [](const auto& x) { return x.kind == 2; });
    const bool writes_match = std::ranges::equal(bus.writes, expected, [](const auto& a, const auto& b) {
        return a.size == b.size && a.addr == b.addr && a.data == b.data;
    });
    if (!writes_match) diff += std::format(" writes({} vs exp {})", bus.writes.size(), expected.size());
    return diff;
}

}  // namespace

int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::cerr << "Usage: ezy_gba_sst <path/to/ARM7TDMI/v1> [file-filter] [max-failures-shown]\n";
        return 2;
    }
    const std::string filter = argc > 2 ? argv[2] : "arm_";
    const int max_shown = argc > 3 ? std::stoi(argv[3]) : 3;

    std::vector<std::filesystem::path> files;
    for (const auto& entry : std::filesystem::directory_iterator(argv[1])) {
        const std::string name = entry.path().filename().string();
        if (name.ends_with(".json.bin") && name.find(filter) != std::string::npos) files.push_back(entry.path());
    }
    std::ranges::sort(files);

    std::size_t total = 0;
    std::size_t total_failed = 0;
    for (const auto& path : files) {
        const auto tests = load_tests(path);
        std::size_t failed = 0;
        for (const auto& t : tests) {
            ReplayBus bus(t);
            Arm7tdmi cpu(bus);
            load_cpu(cpu, t.initial);
            cpu.step();
            const std::string diff = compare(cpu, t, bus);
            if (diff.empty()) continue;
            if (static_cast<int>(failed) < max_shown) {
                std::cout << std::format("  FAIL op={:08X} cpsr={:08X}:{}\n", t.opcode, t.initial.cpsr, diff);
            }
            ++failed;
        }
        total += tests.size();
        total_failed += failed;
        std::cout << std::format("{:<45} {:>6}/{:<6} passed\n", path.filename().string(), tests.size() - failed,
                                 tests.size());
    }
    std::cout << std::format("TOTAL {}/{} passed\n", total - total_failed, total);
    return total_failed == 0 ? 0 : 1;
}
