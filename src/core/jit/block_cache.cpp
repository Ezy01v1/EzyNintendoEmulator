#include "jit/block_cache.hpp"

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <new>
#include <stdexcept>

namespace ezy::jit {

BlockCache::BlockCache(std::uint32_t ram_size, std::size_t code_budget)
    : ram_size_(ram_size), code_(code_budget), pages_(ram_size), top_((ram_size + 0xFFFU) >> CodePages::kPageShift) {}

BlockCache::~BlockCache() = default;

void* BlockCache::arena_alloc(std::size_t bytes) {
    constexpr std::size_t kAlign = alignof(std::max_align_t);
    bytes = (bytes + kAlign - 1) & ~(kAlign - 1);
    if (chunks_.empty() || chunk_used_ + bytes > chunk_size_) {
        chunk_size_ = std::max(kChunk, bytes);
        chunks_.push_back(std::make_unique<std::byte[]>(chunk_size_ + kAlign));
        chunk_used_ = 0;
    }
    auto base = reinterpret_cast<std::uintptr_t>(chunks_.back().get());
    base = (base + kAlign - 1) & ~(kAlign - 1);
    return reinterpret_cast<void*>(base + chunk_used_);  // chunk_used_ advanced below
}

const Block& BlockCache::insert(const Block& block, std::span<const BlockExit> exits,
                                std::span<const std::uint32_t> code) {
    if (block.words == 0 || (block.paddr & 3U) != 0 || block.paddr >= ram_size_)
        throw std::logic_error("jit block cache: bad block address");
    const std::uint64_t last = std::uint64_t{block.paddr} + 4ULL * block.words - 1;
    if ((last >> CodePages::kPageShift) != (block.paddr >> CodePages::kPageShift) || last >= ram_size_)
        throw std::logic_error("jit block cache: block crosses a page or leaves RAM");

    const std::size_t head = (sizeof(Block) + alignof(BlockExit) - 1) & ~(alignof(BlockExit) - 1);
    const std::size_t exits_bytes = exits.size() * sizeof(BlockExit);
    const std::size_t code_off = (head + exits_bytes + alignof(std::uint32_t) - 1) & ~(alignof(std::uint32_t) - 1);
    const std::size_t total = code_off + code.size() * sizeof(std::uint32_t);

    auto* raw = static_cast<std::byte*>(arena_alloc(total));
    constexpr std::size_t kAlign = alignof(std::max_align_t);
    chunk_used_ += (total + kAlign - 1) & ~(kAlign - 1);

    auto* exits_dst = reinterpret_cast<BlockExit*>(raw + head);
    auto* code_dst = reinterpret_cast<std::uint32_t*>(raw + code_off);
    if (!exits.empty()) std::memcpy(exits_dst, exits.data(), exits_bytes);
    if (!code.empty()) std::memcpy(code_dst, code.data(), code.size() * sizeof(std::uint32_t));

    Block* stored = new (raw) Block(block);
    stored->exits = {exits_dst, exits.size()};
    stored->code = {code_dst, code.size()};

    const std::uint32_t page = block.paddr >> CodePages::kPageShift;
    if (!top_[page]) top_[page] = std::make_unique<const Block*[]>(kSlots);  // nullptr-initialised
    const Block*& slot = top_[page][(block.paddr >> 2) & (kSlots - 1)];
    if (slot == nullptr) ++stats_.blocks;
    slot = stored;
    pages_.mark(page);
    return *stored;
}

bool BlockCache::invalidate(std::uint32_t begin, std::uint32_t end) {
    if (end <= begin || top_.empty()) return false;
    const std::uint32_t last = std::min<std::uint32_t>((end - 1) >> CodePages::kPageShift,
                                                       static_cast<std::uint32_t>(top_.size()) - 1);
    bool dropped = false;
    for (std::uint32_t p = begin >> CodePages::kPageShift; p <= last; ++p) {
        if (top_[p]) {
            const Block** slots = top_[p].get();
            for (std::uint32_t i = 0; i < kSlots; ++i) {
                if (slots[i] != nullptr) {
                    slots[i] = nullptr;
                    --stats_.blocks;
                    dropped = true;
                }
            }
        }
        pages_.clear(p);
    }
    if (dropped) ++stats_.invalidations;
    return dropped;
}

void BlockCache::flush() {
    for (auto& page : top_) page.reset();
    chunks_.clear();
    chunk_used_ = chunk_size_ = 0;
    pages_.clear_all();
    code_.reset();
    stats_.blocks = 0;
    ++stats_.flushes;
}

}  // namespace ezy::jit
