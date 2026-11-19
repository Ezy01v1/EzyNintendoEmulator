#include <doctest/doctest.h>

#include <array>
#include <stdexcept>

#include "jit/block_cache.hpp"

using namespace ezy::jit;

namespace {

constexpr std::uint32_t kRam = 0x10000;  // 16 pages

Block make(std::uint32_t paddr, std::uint32_t key, std::uint32_t words = 2) {
    Block b;
    b.key = key;
    b.paddr = paddr;
    b.words = words;
    b.min_cycles = words;
    return b;
}

const Block& put(BlockCache& cache, std::uint32_t paddr, std::uint32_t key, std::uint32_t words = 2) {
    const std::array<BlockExit, 2> exits{{{3, 1, 7}, {5, 2, 9}}};
    const std::array<std::uint32_t, 4> code{1, 2, 3, 4};
    return cache.insert(make(paddr, key, words), exits, std::span<const std::uint32_t>(code).first(words));
}

}  // namespace

TEST_CASE("CodePages marks pages and answers range queries") {
    CodePages pages(kRam);
    CHECK(!pages.marked(0, kRam));
    pages.mark(2);
    CHECK(pages.table()[2] == 1);
    CHECK(pages.marked(0x2000, 0x2004));
    CHECK(pages.marked(0x1FFC, 0x2004));   // spans pages 1 and 2
    CHECK(!pages.marked(0x1000, 0x2000));
    CHECK(!pages.marked(0x3000, 0x3000));  // empty range
    pages.clear(2);
    CHECK(!pages.marked(0, kRam));
}

TEST_CASE("BlockCache finds a block by physical address and mode key only") {
    BlockCache cache(kRam, 1 << 16);
    put(cache, 0x1000, 1);
    CHECK(cache.find(0x1000, 1) != nullptr);
    CHECK(cache.find(0x1000, 2) == nullptr);
    CHECK(cache.find(0x1004, 1) == nullptr);
    CHECK(cache.find(kRam + 0x1000, 1) == nullptr);
    CHECK(cache.pages().marked(0x1000, 0x1004));
    CHECK(cache.stats().blocks == 1);
}

TEST_CASE("BlockCache replaces a block with another key at the same address") {
    BlockCache cache(kRam, 1 << 16);
    put(cache, 0x1000, 1);
    put(cache, 0x1000, 2);
    CHECK(cache.find(0x1000, 1) == nullptr);
    CHECK(cache.find(0x1000, 2) != nullptr);
    CHECK(cache.stats().blocks == 1);
}

TEST_CASE("BlockCache copies exits and code into the cache") {
    BlockCache cache(kRam, 1 << 16);
    const Block& b = put(cache, 0x1008, 1, 3);
    CHECK(b.exits.size() == 2);
    CHECK(b.exits[1].kind == 9);
    REQUIRE(b.code.size() == 3);
    CHECK(b.code[2] == 3);
    CHECK(cache.find(0x1008, 1) == &b);
}

TEST_CASE("BlockCache invalidate drops every block of the touched pages and clears their marks") {
    BlockCache cache(kRam, 1 << 16);
    put(cache, 0x1000, 1);
    put(cache, 0x1FF8, 1);
    put(cache, 0x2000, 1);
    CHECK(cache.invalidate(0x1FFC, 0x2000));
    CHECK(cache.find(0x1000, 1) == nullptr);
    CHECK(cache.find(0x1FF8, 1) == nullptr);
    CHECK(cache.find(0x2000, 1) != nullptr);
    CHECK(!cache.pages().marked(0x1000, 0x2000));
    CHECK(cache.pages().marked(0x2000, 0x2004));
    CHECK(cache.stats().invalidations == 1);
    CHECK(cache.stats().blocks == 1);
    CHECK(!cache.invalidate(0x1000, 0x2000));  // nothing left there
    CHECK(cache.stats().invalidations == 1);
}

TEST_CASE("BlockCache invalidate spans several pages") {
    BlockCache cache(kRam, 1 << 16);
    put(cache, 0x1000, 1);
    put(cache, 0x3000, 1);
    CHECK(cache.invalidate(0x0, kRam + 0x4000));  // clamped to RAM
    CHECK(cache.find(0x1000, 1) == nullptr);
    CHECK(cache.find(0x3000, 1) == nullptr);
    CHECK(!cache.pages().marked(0, kRam));
}

TEST_CASE("BlockCache keeps a dropped block's exits readable until flush") {
    BlockCache cache(kRam, 1 << 16);
    const Block& b = put(cache, 0x1000, 1);
    cache.invalidate(0x1000, 0x1004);
    CHECK(cache.find(0x1000, 1) == nullptr);
    CHECK(b.exits[0].cycles == 3);
    CHECK(b.code[1] == 2);
}

TEST_CASE("BlockCache keeps references valid while the arena grows") {
    BlockCache cache(kRam, 1 << 16);
    const Block& first = put(cache, 0x0, 1);
    for (std::uint32_t i = 0; i < 4000; ++i) put(cache, (i % 16) * 0x1000 + (i / 16 % 1024) * 4 % 0x1000, i + 2);
    CHECK(first.exits[0].cycles == 3);
    CHECK(first.code[0] == 1);
}

TEST_CASE("BlockCache flush empties the cache and the code buffer") {
    BlockCache cache(kRam, 1 << 16);
    put(cache, 0x1000, 1);
    cache.flush();
    CHECK(cache.find(0x1000, 1) == nullptr);
    CHECK(cache.code().used() == 0);
    CHECK(!cache.pages().marked(0, kRam));
    CHECK(cache.stats().flushes == 1);
    CHECK(cache.stats().blocks == 0);
    put(cache, 0x1000, 1);
    CHECK(cache.find(0x1000, 1) != nullptr);
}

TEST_CASE("BlockCache rejects blocks that cross a page or leave RAM") {
    BlockCache cache(kRam, 1 << 16);
    CHECK_THROWS_AS(put(cache, 0x1FFC, 1, 2), std::logic_error);
    CHECK_THROWS_AS(put(cache, kRam, 1, 1), std::logic_error);
    CHECK_THROWS_AS(put(cache, 0x1002, 1, 1), std::logic_error);
}

TEST_CASE("BlockCache rejects code that does not match the block and reports its arena") {
    BlockCache cache(kRam, 1 << 16);
    CHECK(cache.arena_bytes() == 0);
    const std::array<std::uint32_t, 3> code{1, 2, 3};
    CHECK_THROWS_AS(cache.insert(make(0x1000, 1, 2), {}, code), std::logic_error);
    put(cache, 0x1000, 1);
    CHECK(cache.arena_bytes() > 0);
    cache.flush();
    CHECK(cache.arena_bytes() == 0);
}
