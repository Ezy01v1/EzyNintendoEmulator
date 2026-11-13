#include "benchmark.hpp"
#include "n64/n64.hpp"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <string>
#include <thread>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <mmsystem.h>
#if defined(_M_X64) || defined(__x86_64__)
#include <DbgHelp.h>
#endif
#endif

namespace {

void csv_string(std::ostream& out, const std::string& value) {
    out << '"';
    for (char ch : value) { if (ch == '"') out << '"'; out << ch; }
    out << '"';
}

double process_cpu_seconds() {
#if defined(_WIN32)
    FILETIME created{}, exited{}, kernel{}, user{};
    if (!GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user)) return -1;
    const auto ticks = [](FILETIME time) {
        return (std::uint64_t{time.dwHighDateTime} << 32) | time.dwLowDateTime;
    };
    return static_cast<double>(ticks(kernel) + ticks(user)) / 10'000'000.0;
#else
    const auto ticks = std::clock();
    return ticks == static_cast<std::clock_t>(-1) ? -1 : static_cast<double>(ticks) / CLOCKS_PER_SEC;
#endif
}

class Sampler {
public:
    ~Sampler() { stop(); }
    bool start() {
#if defined(_WIN32) && (defined(_M_X64) || defined(__x86_64__))
        target_ = OpenThread(THREAD_GET_CONTEXT | THREAD_SUSPEND_RESUME, FALSE, GetCurrentThreadId());
        if (!target_) return false;
        timer_period_ = timeBeginPeriod(1) == TIMERR_NOERROR;
        worker_ = std::thread([this] {
            while (!finished_.load(std::memory_order_relaxed)) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                if (finished_.load(std::memory_order_relaxed)) break;
                CONTEXT context{};
                context.ContextFlags = CONTEXT_CONTROL;
                if (SuspendThread(target_) == static_cast<DWORD>(-1)) { ++errors_; continue; }
                // No allocations, locks or symbol resolution while the target is suspended.
                const bool captured = GetThreadContext(target_, &context) != FALSE;
                const auto resumed = ResumeThread(target_);
                if (!captured || resumed == static_cast<DWORD>(-1)) { ++errors_; continue; }
                MEMORY_BASIC_INFORMATION region{};
                const bool queried = VirtualQuery(reinterpret_cast<const void*>(context.Rip), &region, sizeof(region)) != 0;
                const bool executable = (region.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ |
                    PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) != 0;
                ++samples_[{context.Rip, queried && region.Type == MEM_PRIVATE && executable}];
            }
        });
        return true;
#else
        return false;
#endif
    }
    void stop() {
#if defined(_WIN32) && (defined(_M_X64) || defined(__x86_64__))
        finished_.store(true, std::memory_order_relaxed);
        if (worker_.joinable()) worker_.join();
        if (target_) { CloseHandle(target_); target_ = nullptr; }
        if (timer_period_) { timeEndPeriod(1); timer_period_ = false; }
#endif
    }
    bool write(const char* path) {
#if defined(_WIN32) && (defined(_M_X64) || defined(__x86_64__))
        const auto process = GetCurrentProcess();
        SymSetOptions(SYMOPT_UNDNAME | SYMOPT_LOAD_LINES | SYMOPT_DEFERRED_LOADS);
        if (!SymInitialize(process, nullptr, TRUE)) return false;
        std::map<std::pair<std::string, std::string>, std::uint64_t> functions;
        std::uint64_t total = 0;
        for (const auto& [location, count] : samples_) {
            const auto [address, native] = location;
            std::string name = native ? "RSP native code" : "unresolved", file;
            alignas(SYMBOL_INFO) std::byte storage[sizeof(SYMBOL_INFO) + MAX_SYM_NAME]{};
            auto* symbol = reinterpret_cast<SYMBOL_INFO*>(storage);
            symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
            symbol->MaxNameLen = MAX_SYM_NAME;
            DWORD64 offset = 0;
            if (!native && SymFromAddr(process, address, &offset, symbol)) name = symbol->Name;
            IMAGEHLP_LINE64 line{};
            line.SizeOfStruct = sizeof(line);
            DWORD displacement = 0;
            if (!native && SymGetLineFromAddr64(process, address, &displacement, &line)) file = line.FileName;
            functions[{name, file}] += count;
            total += count;
        }
        SymCleanup(process);
        std::ofstream output(path);
        output << "Function,File,Samples,Percent\n";
        for (const auto& [function, count] : functions) {
            csv_string(output, function.first); output << ',';
            csv_string(output, function.second); output << ',' << count << ','
                << (total ? 100.0 * static_cast<double>(count) / static_cast<double>(total) : 0) << '\n';
        }
        output.close();
        std::cout << "Profile: samples=" << total << " capture_errors=" << errors_ << " path=" << path << '\n';
        return output.good() && total > 0 && errors_ == 0;
#else
        (void)path;
        return false;
#endif
    }
private:
#if defined(_WIN32) && (defined(_M_X64) || defined(__x86_64__))
    HANDLE target_ = nullptr;
    bool timer_period_ = false;
    std::thread worker_;
    std::atomic<bool> finished_{false};
    std::map<std::pair<DWORD64, bool>, std::uint64_t> samples_;
    std::uint64_t errors_ = 0;
#endif
};
}

bool benchmark_frames(ezy::n64::N64& n64, int first, int end, const char* profile_path) {
    if (first < 0 || first >= end) return false;
    Sampler sampler;
    for (int frame = 0; frame < first; ++frame) n64.run_frame();
    n64.bus().drain_rdp();
    if (profile_path && !sampler.start()) {
        std::cerr << "Sampling requires Windows x64 and access to the main thread\n";
        return false;
    }
    const auto cpu_start = process_cpu_seconds();
    const auto started = std::chrono::steady_clock::now();
    for (int frame = first; frame < end; ++frame) n64.run_frame();
    n64.bus().drain_rdp();
    const double wall = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    const double cpu_end = process_cpu_seconds();
    sampler.stop();
    if (cpu_start < 0 || cpu_end < 0) { std::cerr << "Could not read process CPU time\n"; return false; }
    const double cpu = cpu_end - cpu_start;
    std::cout << std::setprecision(9) << "Benchmark: {\"first\":" << first << ",\"end\":" << end
              << ",\"fields\":" << end - first << ",\"wall_seconds\":" << wall
              << ",\"cpu_seconds\":" << cpu << ",\"fields_per_second\":" << (end - first) / wall
              << ",\"profiled\":" << (profile_path ? "true" : "false") << "}\n";
    return !profile_path || sampler.write(profile_path);
}
