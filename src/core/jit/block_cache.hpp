#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

#include "jit/code_buffer.hpp"
#include "jit/code_pages.hpp"

// Public header of ezy_jit: compiled blocks indexed by physical address and mode key.
namespace ezy::jit {

struct BlockExit {
    std::uint32_t cycles = 0, instructions = 0;  // static counts up to this exit
    std::uint32_t kind = 0;                      // front-end meaning
    std::uint32_t native = 0;                    // of `instructions`, those translated to native code
};

struct Block {
    BlockFn fn = nullptr;                // nullptr: "the interpreter runs this pc" (cached so it is not recompiled)
    std::uint32_t key = 0;               // mode key it was compiled for
    std::uint32_t paddr = 0, words = 0;  // never crosses a 4 KiB page
    std::uint32_t min_cycles = 0;        // static cycles of the whole block
    std::span<const BlockExit> exits;    // indexed by the tag BlockFn returns
    std::span<const std::uint32_t> code; // the words compiled (for the check mode)
};

// Not thread-safe. A Block& returned by insert() and its spans stay valid until flush(),
// even after invalidate() dropped the block from the lookup (a running block may be
// invalidated by its own store).
class BlockCache {
public:
    BlockCache(std::uint32_t ram_size, std::size_t code_budget);
    ~BlockCache();
    BlockCache(const BlockCache&) = delete;
    BlockCache& operator=(const BlockCache&) = delete;

    // nullptr if absent or compiled for another key (or paddr is not the block's own, e.g.
    // unaligned). Two loads plus the key and address compares.
    [[nodiscard]] const Block* find(std::uint32_t paddr, std::uint32_t key) const {
        const std::uint32_t page = paddr >> CodePages::kPageShift;
        if (page >= top_.size()) return nullptr;
        const Block* const* slots = top_[page].get();
        if (slots == nullptr) return nullptr;
        const Block* b = slots[(paddr >> 2) & (kSlots - 1)];
        return (b != nullptr && b->key == key && b->paddr == paddr) ? b : nullptr;
    }

    // Copies exits and code into an arena (freed only by flush), marks the page and
    // replaces any block at the same address. Throws std::logic_error if the block is empty,
    // unaligned, outside RAM, crosses a 4 KiB page or `code` does not hold `words` words.
    const Block& insert(const Block& block, std::span<const BlockExit> exits, std::span<const std::uint32_t> code);
    // Drops every block of every page overlapping [begin, end) and clears those marks.
    // Returns true if any block existed.
    bool invalidate(std::uint32_t begin, std::uint32_t end);
    void flush();  // everything, including the code buffer

    [[nodiscard]] CodeBuffer& code() { return code_; }
    // Bytes the inserted blocks use in the arena (Block, exits, code words) until flush().
    [[nodiscard]] std::size_t arena_bytes() const { return arena_bytes_; }
    [[nodiscard]] const CodePages& pages() const { return pages_; }

    // blocks = blocks currently findable (inserted, not replaced or invalidated since).
    struct Stats { std::uint64_t blocks = 0, invalidations = 0, flushes = 0; };
    [[nodiscard]] Stats stats() const { return stats_; }

private:
    static constexpr std::uint32_t kSlots = 1024;  // one slot per word of a 4 KiB page
    static constexpr std::size_t kChunk = 64 * 1024;

    void* arena_alloc(std::size_t bytes);

    std::uint32_t ram_size_;
    CodeBuffer code_;
    CodePages pages_;
    std::vector<std::unique_ptr<const Block*[]>> top_;  // per page, allocated on demand
    std::vector<std::unique_ptr<std::byte[]>> chunks_;
    std::size_t chunk_used_ = 0, chunk_size_ = 0, arena_bytes_ = 0;
    Stats stats_;
};

}  // namespace ezy::jit
