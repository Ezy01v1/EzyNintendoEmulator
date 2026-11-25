#pragma once

// Builds a small synthetic GameCube disc image (GCM layout) in a temp file: the header with
// the magic, BI2, a DOL assembled with ppc_asm.hpp, and an FST with one file. The layout
// follows YAGCD's "DVD" chapter (header 0x000-0x43F, BI2 0x440-0x243F).

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include "ppc_asm.hpp"

// A file in the temp directory holding `bytes`, removed on destruction (the tests leave
// nothing behind). Declare it before the Disc that opens it, so the Disc closes it first.
struct TempImage {
    std::filesystem::path path;

    TempImage(std::string_view name, const std::vector<std::uint8_t>& bytes)
        : path(std::filesystem::temp_directory_path() / (std::string("ezy_gc_") + std::string(name) + ".iso")) {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    }
    TempImage(const TempImage&) = delete;
    TempImage& operator=(const TempImage&) = delete;
    ~TempImage() {
        std::error_code ec;
        std::filesystem::remove(path, ec);
    }
    operator const std::filesystem::path&() const { return path; }
};

struct DiscBuilder {
    // Where the pieces go in the image.
    static constexpr std::uint32_t kDolOffset = 0x4000;
    static constexpr std::uint32_t kFstOffset = 0x8000;
    static constexpr std::uint32_t kFileOffset = 0x9000;
    static constexpr std::uint32_t kImageSize = 0xA000;

    // The DOL: one text section, one data section, a BSS range.
    static constexpr std::uint32_t kTextAddress = 0x80003100;
    static constexpr std::uint32_t kDataAddress = 0x80004000;
    static constexpr std::uint32_t kBssAddress = 0x80005000;
    static constexpr std::uint32_t kBssSize = 0x100;
    static constexpr std::uint32_t kDataWord = 0x12345678;  // first word of the data section
    // The program stores 0x600DF00D here, then the data section's first word 4 bytes later.
    static constexpr std::uint32_t kResultAddress = 0x80001800;

    std::vector<std::uint8_t> image = std::vector<std::uint8_t>(kImageSize, 0);
    std::string file_contents = "hello, disc";

    void put32(std::uint32_t at, std::uint32_t value) {
        for (int i = 0; i < 4; ++i) image[at + static_cast<std::uint32_t>(i)] = static_cast<std::uint8_t>(value >> (24 - 8 * i));
    }
    void put_bytes(std::uint32_t at, std::string_view bytes) {
        for (std::size_t i = 0; i < bytes.size(); ++i) image[at + i] = static_cast<std::uint8_t>(bytes[i]);
    }

    // The program: stores 0x600DF00D at kResultAddress, copies the data word next to it, and
    // loops forever (b 0). Runs with the boot BATs, so addresses are 0x8xxxxxxx.
    static std::vector<std::uint32_t> program() {
        using namespace ppc;
        return {
            lis(4, 0x8000),                                     // r4 = 0x80000000
            lis(3, 0x600D),                                     // r3 = 0x600D0000
            ori(3, 3, 0xF00D),                                  // r3 = 0x600DF00D
            stw(3, 0x1800, 4),                                  // [0x80001800] = r3
            lwz(5, 0x4000, 4),                                  // r5 = [0x80004000] (data)
            stw(5, 0x1804, 4),                                  // [0x80001804] = r5
            b(0),                                               // loop
        };
    }

    DiscBuilder() {
        // Header: game id, maker, magic, the DOL and FST offsets.
        put_bytes(0x000, "GTST01");
        put32(0x01C, 0xC2339F3D);
        put_bytes(0x020, "ezyemu test disc");
        put32(0x420, kDolOffset);
        put32(0x424, kFstOffset);
        put32(0x428, fst_size());
        put32(0x42C, fst_size());
        // BI2: simulated memory size at +4, region at +0x18 (1 = USA).
        put32(0x444, 0x01800000);
        put32(0x458, 1);

        // DOL header at kDolOffset: text0 and data0, BSS, entry.
        const auto words = program();
        const std::uint32_t text_size = static_cast<std::uint32_t>(words.size() * 4);
        const std::uint32_t text_file = 0x100, data_file = 0x200, data_size = 0x20;
        put32(kDolOffset + 0x00, text_file);       // text0 file offset
        put32(kDolOffset + 0x1C, data_file);       // data0 file offset
        put32(kDolOffset + 0x48, kTextAddress);    // text0 address
        put32(kDolOffset + 0x64, kDataAddress);    // data0 address
        put32(kDolOffset + 0x90, text_size);       // text0 size
        put32(kDolOffset + 0xAC, data_size);       // data0 size
        put32(kDolOffset + 0xD8, kBssAddress);
        put32(kDolOffset + 0xDC, kBssSize);
        put32(kDolOffset + 0xE0, kTextAddress);    // entry
        for (std::size_t i = 0; i < words.size(); ++i) put32(kDolOffset + text_file + static_cast<std::uint32_t>(i * 4), words[i]);
        put32(kDolOffset + data_file, kDataWord);
        for (std::uint32_t i = 4; i < data_size; i += 4) put32(kDolOffset + data_file + i, 0xDA7A0000 | i);

        // FST: the root directory (2 entries), one file, the string table.
        put32(kFstOffset + 0x00, 0x01000000);  // root: directory, name offset 0
        put32(kFstOffset + 0x04, 0);
        put32(kFstOffset + 0x08, 2);           // number of entries
        put32(kFstOffset + 0x0C, 0x00000000);  // file, name offset 0
        put32(kFstOffset + 0x10, kFileOffset);
        put32(kFstOffset + 0x14, static_cast<std::uint32_t>(file_contents.size()));
        put_bytes(kFstOffset + 0x18, std::string_view("hello.txt\0", 10));
        put_bytes(kFileOffset, file_contents);
    }

    static constexpr std::uint32_t fst_size() { return 0x18 + 10; }

    // Writes the image to a temp file, deleted when the returned object goes away. Keep it
    // alive (named) for as long as a Disc has the file open.
    [[nodiscard]] TempImage write(std::string_view name) const { return TempImage(name, image); }
};
