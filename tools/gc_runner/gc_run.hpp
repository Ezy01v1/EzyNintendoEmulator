#pragma once

// The GameCube runner's run loop and its printout (design, "Runner y ezyemu"), header-only so
// tests/gc drives the machine exactly as ezy_gc_runner does.

#include <cstdint>
#include <cstdio>
#include <exception>
#include <ostream>
#include <string>

#include "gc/gamecube.hpp"
#include "gc/gekko/gekko.hpp"
#include "gc/gx_fifo.hpp"
#include "gc/pi.hpp"

namespace ezy::gc_runner {

inline std::string hex32(std::uint32_t value) {
    char buf[16];
    std::snprintf(buf, sizeof buf, "0x%08X", value);
    return buf;
}

// One line of stats: the field count and Stats' counters. The interrupts are rising edges per
// PI cause since the boot; the graphics counters are the last finished field's.
inline void print_stats(std::ostream& out, const gc::Stats& s) {
    using gc::Pi;
    struct Named {
        const char* name;
        unsigned bit;
    };
    static constexpr Named kCauses[] = {
        {"VI", Pi::kVi},   {"PE_TOKEN", Pi::kPeToken}, {"PE_FINISH", Pi::kPeFinish}, {"CP", Pi::kCp},
        {"DI", Pi::kDi},   {"SI", Pi::kSi},            {"EXI", Pi::kExi},            {"AI", Pi::kAi},
        {"DSP", Pi::kDsp}, {"MI", Pi::kMi},            {"GPU_ERR", Pi::kGpuError},   {"RSW", Pi::kRsw},
        {"DEBUG", Pi::kDebug}, {"HSP", Pi::kHsp},
    };
    static constexpr const char* kCommands[gc::kGxCommandCount] = {"nop", "cp", "xf", "indx", "dl",
                                                                   "inval", "bp", "prim"};
    out << "field " << s.fields << ": instructions " << s.instructions << " | interrupts";
    for (const auto& [name, bit] : kCauses) out << ' ' << name << '=' << s.interrupts[bit];
    out << " | DI reads " << s.di_reads << " | FIFO bytes " << s.fifo_bytes_last_field << " | GX";
    for (std::size_t i = 0; i < gc::kGxCommandCount; ++i) out << ' ' << kCommands[i] << '=' << s.gx_commands_last_field[i];
    out << " | primitives " << s.gx_primitives_last_field << " vertices " << s.gx_vertices_last_field << " | DSP mails "
        << s.dsp_mails << '\n';
}

// Goal 1's criterion over consecutive fields: the longest run in which every field had FIFO
// data and a VI interrupt.
struct CriterionStreak {
    int current = 0;             // the run ending at the last field added
    int longest = 0;
    int longest_last_field = 0;  // the last field of the longest run

    void add(int field, bool fifo_data, bool vi_interrupt) {
        current = fifo_data && vi_interrupt ? current + 1 : 0;
        if (current > longest) {
            longest = current;
            longest_last_field = field;
        }
    }
};

struct RunReport {
    int exit_code = 0;   // 0: every field ran; 1: emulation stopped (the reason was printed)
    int fields_run = 0;  // fields that finished
    // Goal 1's criterion, per finished field: the game wrote FIFO data in it / the VI raised
    // its interrupt in it.
    int fields_with_fifo_data = 0;
    int fields_with_vi_interrupt = 0;
    CriterionStreak streak;
};

// Runs `fields` fields, printing the stats every `print_every` fields (the runner: 60, one
// emulated second; 0: only at the end) and at the end. When emulation
// stops (GekkoError, BusError, or any other error the machine throws), prints the reason, the
// pc, the word or address, and the PCs gc.recent_pcs() holds, then returns exit code 1.
inline RunReport run_fields(gc::GameCube& gc, int fields, std::ostream& out, int print_every = 60) {
    RunReport report;
    auto summary = [&] {
        out << "fields with FIFO data: " << report.fields_with_fifo_data << " of " << report.fields_run
            << "; fields with a VI interrupt: " << report.fields_with_vi_interrupt << " of " << report.fields_run << '\n'
            << "longest run of fields with both: " << report.streak.longest;
        if (report.streak.longest > 0) {
            out << " (fields " << report.streak.longest_last_field - report.streak.longest + 1 << '-'
                << report.streak.longest_last_field << ')';
        }
        out << '\n';
    };
    try {
        for (int i = 0; i < fields; ++i) {
            const std::uint64_t vi_before = gc.stats().interrupts[gc::Pi::kVi];
            gc.run_field();
            ++report.fields_run;
            const gc::Stats s = gc.stats();
            if (s.fifo_bytes_last_field > 0) ++report.fields_with_fifo_data;
            const bool vi = s.interrupts[gc::Pi::kVi] > vi_before;
            if (vi) ++report.fields_with_vi_interrupt;
            report.streak.add(report.fields_run, s.fifo_bytes_last_field > 0, vi);
            if (print_every > 0 && report.fields_run % print_every == 0 && report.fields_run != fields) print_stats(out, s);
        }
    } catch (const gc::GekkoError& e) {
        out << "Stopped after " << gc.stats().instructions << " instructions, in field " << report.fields_run + 1 << ": "
            << e.what() << '\n'
            << "  pc " << hex32(e.pc) << ", word/address " << hex32(e.word_or_address) << '\n';
        report.exit_code = 1;
    } catch (const std::exception& e) {
        out << "Stopped after " << gc.stats().instructions << " instructions, in field " << report.fields_run + 1 << ": "
            << e.what() << '\n'
            << "  pc " << hex32(gc.cpu().pc()) << '\n';
        report.exit_code = 1;
    }
    if (report.exit_code != 0) {
        const auto pcs = gc.recent_pcs();
        out << "  last " << pcs.size() << " pcs, oldest first:\n";
        for (const std::uint32_t pc : pcs) out << "    " << hex32(pc) << '\n';
    }
    print_stats(out, gc.stats());
    summary();
    return report;
}

}  // namespace ezy::gc_runner
