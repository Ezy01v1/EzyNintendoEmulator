// Headless N64 runner, for test ROMs.
//
//   ezy_n64_runner <rom> --screenshot <out.bmp> [frames]
//       Runs `frames` frames (default 120) and saves the VI image as a 24-bit BMP.
//   ezy_n64_runner <rom> --compare <reference.bmp> [frames]
//       Same, then compares the VI image with a 24-bit BMP of the same size (e.g. a
//       PeterLemon reference .png converted with ffmpeg). Exit code 0 on an exact match;
//       otherwise prints the sizes or the first differing pixel.
//   ezy_n64_runner <rom> --isviewer [seconds]
//       Prints what the ROM writes to the ISViewer port (n64-systemtest) for up to
//       `seconds` of emulated time (default 60). Exit code 0 when n64-systemtest's
//       summary says no base test failed.
//   ezy_n64_runner <rom> --pc [frames]
//       Debugging: prints the CPU's PC and Cause after each frame (default 10 frames).
// Option, anywhere after the ROM: --pif <file> (a real PIF ROM).
// --rdp-threaded / --rdp-sync selects host execution without changing emulated timing.
// --state-check [frames] compares a cross-mode save/load continuation for 120 frames.
#include <cstdint>
#include <charconv>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <vector>

#include "n64/n64.hpp"
#include "benchmark.hpp"

namespace {

using ezy::n64::N64;
using ezy::n64::Vi;

std::vector<std::uint8_t> read_file(const char* path) {
    std::ifstream file(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(file), {}};
}

void put_u16(std::vector<char>& out, std::uint16_t v) { out.insert(out.end(), reinterpret_cast<char*>(&v), reinterpret_cast<char*>(&v) + 2); }
void put_u32(std::vector<char>& out, std::uint32_t v) { out.insert(out.end(), reinterpret_cast<char*>(&v), reinterpret_cast<char*>(&v) + 4); }

// Minimal 24-bit uncompressed BMP (bottom-up rows, each padded to 4 bytes).
std::vector<char> encode_bmp(const Vi::Image& img) {
    const int row_size = (img.width * 3 + 3) & ~3;
    const auto data_size = static_cast<std::uint32_t>(row_size * img.height);
    std::vector<char> out;
    out.push_back('B'), out.push_back('M');
    put_u32(out, 54 + data_size), put_u32(out, 0), put_u32(out, 54), put_u32(out, 40);
    put_u32(out, static_cast<std::uint32_t>(img.width)), put_u32(out, static_cast<std::uint32_t>(img.height));
    put_u16(out, 1), put_u16(out, 24), put_u32(out, 0), put_u32(out, data_size);
    put_u32(out, 2835), put_u32(out, 2835), put_u32(out, 0), put_u32(out, 0);
    for (int y = img.height - 1; y >= 0; --y) {
        std::vector<char> row(static_cast<std::size_t>(row_size), 0);
        for (int x = 0; x < img.width; ++x) {
            const std::uint32_t p = img.argb[static_cast<std::size_t>(y * img.width + x)];
            const auto at = static_cast<std::size_t>(x * 3);
            row[at] = static_cast<char>(p & 0xFF);
            row[at + 1] = static_cast<char>((p >> 8) & 0xFF);
            row[at + 2] = static_cast<char>((p >> 16) & 0xFF);
        }
        out.insert(out.end(), row.begin(), row.end());
    }
    return out;
}

int run_compare(N64& n64, const char* reference, int frames) {
    const std::vector<std::uint8_t> ref = read_file(reference);
    if (ref.size() < 54) {
        std::cerr << "Could not read " << reference << '\n';
        return 2;
    }
    for (int i = 0; i < frames; ++i) n64.run_frame();
    Vi::Image img = n64.image();
    std::int32_t ref_w = 0, ref_h = 0;
    std::uint32_t offset = 0;
    std::memcpy(&offset, ref.data() + 10, 4);
    std::memcpy(&ref_w, ref.data() + 18, 4);
    std::memcpy(&ref_h, ref.data() + 22, 4);
    // PeterLemon's references of 240-line modes are 480 lines high, every line doubled.
    if (ref_w == img.width && ref_h == img.height * 2) {
        Vi::Image doubled{img.width, img.height * 2, {}};
        for (int y = 0; y < img.height; ++y) {
            const auto row = img.argb.begin() + static_cast<std::ptrdiff_t>(y) * img.width;
            doubled.argb.insert(doubled.argb.end(), row, row + img.width);
            doubled.argb.insert(doubled.argb.end(), row, row + img.width);
        }
        img = std::move(doubled);
    }
    if (ref_w != img.width || ref_h != img.height) {
        std::cout << "Failed: image is " << img.width << "x" << img.height << ", reference " << ref_w << "x" << ref_h
                  << '\n';
        return 1;
    }
    const std::vector<char> mine = encode_bmp(img);
    const int row_size = (img.width * 3 + 3) & ~3;
    for (std::size_t i = 54; i < mine.size(); ++i) {
        const std::size_t r = offset + (i - 54);
        if (r >= ref.size() || static_cast<std::uint8_t>(mine[i]) != ref[r]) {
            const auto pos = static_cast<int>(i - 54);
            std::cout << "Failed: first difference at x=" << (pos % row_size) / 3
                      << " y=" << img.height - 1 - pos / row_size << " after " << frames << " frames\n";
            return 1;
        }
    }
    std::cout << "Passed: " << reference << " after " << frames << " frames\n";
    return 0;
}

int run_isviewer(N64& n64, double seconds) {
    const auto max_cycles = static_cast<std::uint64_t>(seconds * 93'750'000.0);
    std::string all;
    while (n64.cycles() < max_cycles) {
        n64.run_frame();
        const std::string out = n64.bus().take_isviewer_output();
        if (out.empty()) continue;
        std::cout << out << std::flush;
        all += out;
        if (all.find("Finished in") != std::string::npos) {
            // The summary may arrive in more than one ISViewer chunk.
            for (int i = 0; i < 10; ++i) {
                n64.run_frame();
                const std::string more = n64.bus().take_isviewer_output();
                std::cout << more << std::flush;
                all += more;
            }
            break;
        }
    }
    // n64-systemtest's summary (src/tests/mod.rs): "Finished in X.XXs. Base: Failed N of M tests ...".
    return all.find("Base: Failed 0 of") != std::string::npos ? 0 : 1;
}

void print_rdp_stats(N64& n64) {
    n64.bus().drain_rdp();
    const auto stats = n64.bus().rdp_stats();
    std::cout << "RDP packets: " << stats.submitted << ", waits: " << stats.waits
              << ", queue waits: " << stats.queue_waits << '\n';
    const auto jit=n64.bus().rsp().jit_stats();
    std::cout << "RSP backend: " << (n64.bus().rsp().jit_active() ? "JIT" : "interpreter")
              << ", native: " << jit.native_instructions << ", fallback: " << jit.fallback_instructions
              << ", blocks: " << jit.compiled_blocks << ", compile ms: " << jit.compile_ns/1e6
              << ", reused: " << jit.reused_blocks << ", cache flushes: " << jit.cache_flushes << '\n';
}

}  // namespace

int main(int raw_argc, char* raw_argv[]) {
    std::vector<char*> args;
    std::vector<std::uint8_t> pif;
    auto rdp_mode = ezy::n64::RdpMode::Synchronous;
    auto rsp_mode = ezy::n64::RspMode::Interpreter;
    int time_from = 0;
    const char* profile_path = nullptr;
    const char* final_state_path = nullptr;
    for (int i = 0; i < raw_argc; ++i) {
        if (std::strcmp(raw_argv[i], "--pif") == 0 && i + 1 < raw_argc) pif = read_file(raw_argv[++i]);
        else if (std::strcmp(raw_argv[i], "--rdp-threaded") == 0) rdp_mode = ezy::n64::RdpMode::Threaded;
        else if (std::strcmp(raw_argv[i], "--rdp-sync") == 0) rdp_mode = ezy::n64::RdpMode::Synchronous;
        else if (std::strcmp(raw_argv[i], "--rsp-jit") == 0) rsp_mode = ezy::n64::RspMode::Jit;
        else if (std::strcmp(raw_argv[i], "--rsp-interpreter") == 0) rsp_mode = ezy::n64::RspMode::Interpreter;
        else if (std::strcmp(raw_argv[i], "--time-from") == 0 && i + 1 < raw_argc) {
            const std::string value = raw_argv[++i];
            const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), time_from);
            if (error != std::errc{} || end != value.data() + value.size() || time_from < 0) {
                std::cerr << "--time-from requires a nonnegative field index\n"; return 2;
            }
        }
        else if (std::strcmp(raw_argv[i], "--profile") == 0 && i + 1 < raw_argc) profile_path = raw_argv[++i];
        else if (std::strcmp(raw_argv[i], "--dump-final-state") == 0 && i + 1 < raw_argc) final_state_path = raw_argv[++i];
        else args.push_back(raw_argv[i]);
    }
    const int argc = static_cast<int>(args.size());
    if (argc < 3) {
        std::cerr << "Usage: ezy_n64_runner <rom> --screenshot <out.bmp> [frames]\n"
                     "       ezy_n64_runner <rom> --compare <reference.bmp> [frames]\n"
                     "       ezy_n64_runner <rom> --isviewer [seconds]\n"
                     "       ezy_n64_runner <rom> --state-check [frames]\n"
                     "       ezy_n64_runner <rom> --dump-state <out.bin> [frames]\n"
                     "       ezy_n64_runner <rom> --benchmark <out.bmp> <end> --time-from <first>\n"
                     "Benchmark options: --profile <out.csv>, --dump-final-state <out.bin>\n"
                     "Options: --pif <pif rom>, --rdp-threaded, --rdp-sync, --rsp-jit, --rsp-interpreter\n";
        return 2;
    }
    const std::vector<std::uint8_t> rom = read_file(args[1]);
    auto n64 = N64::create(rom, pif, rdp_mode, rsp_mode);
    if (!n64) {
        std::cerr << "Not an N64 ROM: " << args[1] << '\n';
        return 2;
    }
    std::cout << "Title: " << n64->cartridge().title() << '\n';
    std::cout << "RDP: " << (n64->bus().rdp_threaded() ? "threaded" : "synchronous") << '\n';
    std::cout << "RSP: " << (n64->bus().rsp().jit_active() ? "JIT available" : "interpreter") << '\n';
    const std::string mode = args[2];
    if (mode != "--benchmark" && (time_from != 0 || profile_path || final_state_path)) {
        std::cerr << "--time-from, --profile and --dump-final-state require --benchmark\n"; return 2;
    }
    if (mode == "--benchmark" && argc == 5) {
        int end_frame = 0;
        const std::string value = args[4];
        const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), end_frame);
        if (error != std::errc{} || end != value.data() + value.size() || end_frame <= time_from) {
            std::cerr << "Benchmark end must be greater than --time-from\n"; return 2;
        }
        if (!benchmark_frames(*n64, time_from, end_frame, profile_path)) return 2;
        const auto bmp = encode_bmp(n64->image());
        std::ofstream image(args[3], std::ios::binary);
        image.write(bmp.data(), static_cast<std::streamsize>(bmp.size()));
        image.close();
        if (!image) return 2;
        if (final_state_path) {
            const auto state = n64->save_state();
            std::ofstream file(final_state_path, std::ios::binary);
            file.write(reinterpret_cast<const char*>(state.data()), static_cast<std::streamsize>(state.size()));
            file.close();
            if (!file) return 2;
        }
        print_rdp_stats(*n64);
        return 0;
    }
    if (mode == "--screenshot" && argc >= 4) {
        const int frames = argc > 4 ? std::atoi(args[4]) : 120;
        for (int i = 0; i < frames; ++i) n64->run_frame();
        const std::vector<char> bmp = encode_bmp(n64->image());
        std::ofstream(args[3], std::ios::binary).write(bmp.data(), static_cast<std::streamsize>(bmp.size()));
        std::cout << "Saved " << args[3] << " (" << n64->image().width << "x" << n64->image().height << ")\n";
        print_rdp_stats(*n64);
        return 0;
    }
    if (mode == "--dump-state" && argc >= 4) {
        const int frames=argc>4?std::atoi(args[4]):120;
        for(int i=0;i<frames;++i) n64->run_frame();
        const auto state=n64->save_state();
        std::ofstream file(args[3],std::ios::binary);
        file.write(reinterpret_cast<const char*>(state.data()),static_cast<std::streamsize>(state.size()));
        file.close();
        if(!file) { std::cerr<<"Could not write "<<args[3]<<'\n'; return 2; }
        std::cout<<"Saved state after "<<frames<<" frames\n";
        print_rdp_stats(*n64);
        return 0;
    }
    if (mode == "--compare" && argc >= 4) {
        const int result = run_compare(*n64, args[3], argc > 4 ? std::atoi(args[4]) : 120);
        print_rdp_stats(*n64);
        return result;
    }
    if (mode == "--state-check") {
        const int frames = argc > 3 ? std::atoi(args[3]) : 300;
        for (int i = 0; i < frames; ++i) n64->run_frame();
        const auto state = n64->save_state();
        const auto other_mode = rdp_mode == ezy::n64::RdpMode::Threaded
            ? ezy::n64::RdpMode::Synchronous : ezy::n64::RdpMode::Threaded;
        const auto other_rsp = rsp_mode == ezy::n64::RspMode::Jit
            ? ezy::n64::RspMode::Interpreter : ezy::n64::RspMode::Jit;
        const auto other = N64::create(rom, pif, other_mode, other_rsp);
        if (!other || !other->load_state(state) || other->save_state() != state) return 1;
        for (int i = 0; i < 120; ++i) { n64->run_frame(); other->run_frame(); }
        if (n64->save_state() != other->save_state() || n64->image().argb != other->image().argb) {
            std::cerr << "Failed: cross-mode state continuation differs\n";
            return 1;
        }
        std::cout << "Passed: cross-mode state continuation after " << frames << "+120 frames\n";
        print_rdp_stats(*n64);
        return 0;
    }
    if (mode == "--isviewer") return run_isviewer(*n64, argc > 3 ? std::atof(args[3]) : 60.0);
    if (mode == "--pc") {
        const int frames = argc > 3 ? std::atoi(args[3]) : 10;
        for (int i = 0; i < frames; ++i) {
            n64->run_frame();
            std::printf("frame %3d  pc %016llX  cause %08llX  status %08llX  epc %016llX  badvaddr %016llX\n", i,
                        static_cast<unsigned long long>(n64->cpu().pc()),
                        static_cast<unsigned long long>(n64->cpu().cop0().read(ezy::n64::Cop0::kCause)),
                        static_cast<unsigned long long>(n64->cpu().cop0().read(ezy::n64::Cop0::kStatus)),
                        static_cast<unsigned long long>(n64->cpu().cop0().read(ezy::n64::Cop0::kEpc)),
                        static_cast<unsigned long long>(n64->cpu().cop0().read(ezy::n64::Cop0::kBadVAddr)));
        }
        std::cout << n64->bus().take_isviewer_output();
        return 0;
    }
    std::cerr << "Unknown mode " << mode << '\n';
    return 2;
}
