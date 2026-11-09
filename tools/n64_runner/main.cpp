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
// Options, anywhere after the ROM: --pif <file> (a real PIF ROM); --threaded-rdp (draw on a
// second thread like ezyemu: faster, not cycle-exact); --time-from <frame> (with --screenshot:
// prints the speed from that frame on, e.g. 1500 for MK64's demo race); --rdp-trace <file>
// [--trace-from <frame>] (with --screenshot: writes every RDP command word consumed from that
// frame on, as little-endian uint64, for tools/rdp_trace_diff).
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <vector>

#include "n64/n64.hpp"

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

}  // namespace

int main(int raw_argc, char* raw_argv[]) {
    std::vector<char*> args;
    std::vector<std::uint8_t> pif;
    bool threaded_rdp = false;
    int time_from = -1;
    const char* trace_path = nullptr;
    int trace_from = 0;
    for (int i = 0; i < raw_argc; ++i) {
        if (std::strcmp(raw_argv[i], "--pif") == 0 && i + 1 < raw_argc) pif = read_file(raw_argv[++i]);
        else if (std::strcmp(raw_argv[i], "--threaded-rdp") == 0) threaded_rdp = true;
        else if (std::strcmp(raw_argv[i], "--time-from") == 0 && i + 1 < raw_argc) time_from = std::atoi(raw_argv[++i]);
        else if (std::strcmp(raw_argv[i], "--rdp-trace") == 0 && i + 1 < raw_argc) trace_path = raw_argv[++i];
        else if (std::strcmp(raw_argv[i], "--trace-from") == 0 && i + 1 < raw_argc) trace_from = std::atoi(raw_argv[++i]);
        else args.push_back(raw_argv[i]);
    }
    const int argc = static_cast<int>(args.size());
    if (argc < 3) {
        std::cerr << "Usage: ezy_n64_runner <rom> --screenshot <out.bmp> [frames]\n"
                     "       ezy_n64_runner <rom> --compare <reference.bmp> [frames]\n"
                     "       ezy_n64_runner <rom> --isviewer [seconds]\n"
                     "Options: --pif <pif rom>, --threaded-rdp, --time-from <frame>\n";
        return 2;
    }
    const std::vector<std::uint8_t> rom = read_file(args[1]);
    auto n64 = N64::create(rom, pif);
    if (!n64) {
        std::cerr << "Not an N64 ROM: " << args[1] << '\n';
        return 2;
    }
    n64->set_threaded_rdp(threaded_rdp);
    std::cout << "Title: " << n64->cartridge().title() << '\n';
    const std::string mode = args[2];
    if (mode == "--screenshot" && argc >= 4) {
        const int frames = argc > 4 ? std::atoi(args[4]) : 120;
        auto timed_from = std::chrono::steady_clock::now();
        std::vector<std::uint64_t> trace;
        for (int i = 0; i < frames; ++i) {
            if (i == time_from) timed_from = std::chrono::steady_clock::now();
            if (trace_path && i == trace_from) n64->bus().rdp().set_trace(&trace);
            n64->run_frame();
        }
        if (trace_path) {
            n64->bus().rdp().sync();
            n64->bus().rdp().set_trace(nullptr);
            std::ofstream(trace_path, std::ios::binary)
                .write(reinterpret_cast<const char*>(trace.data()), static_cast<std::streamsize>(trace.size() * 8));
            std::cout << "Traced " << trace.size() << " RDP words to " << trace_path << '\n';
        }
        if (time_from >= 0 && time_from < frames) {
            const std::chrono::duration<double> spent = std::chrono::steady_clock::now() - timed_from;
            std::cout << "Frames " << time_from << "-" << frames << ": " << spent.count() << " s ("
                      << (frames - time_from) / spent.count() << " frames/s)\n";
        }
        const std::vector<char> bmp = encode_bmp(n64->image());
        std::ofstream(args[3], std::ios::binary).write(bmp.data(), static_cast<std::streamsize>(bmp.size()));
        std::cout << "Saved " << args[3] << " (" << n64->image().width << "x" << n64->image().height << ")\n";
        return 0;
    }
    if (mode == "--compare" && argc >= 4) return run_compare(*n64, args[3], argc > 4 ? std::atoi(args[4]) : 120);
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
