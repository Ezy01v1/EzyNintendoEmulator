#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>

// Public header of ezy_jit: it never includes AsmJit and knows no guest CPU.
namespace ezy::jit {

// Read-only view of a threaded renderer's page reservations, for the inline fast path of
// its memory barrier: for the page `paddr >> 12`, an access must wait when the sequence of
// the slot that writes the page (and, for a write, of the slot that reads it) is newer than
// `*completed_seen`, or when `*failed` is set. The owner keeps writing these tables.
struct PageBarrierView {
    const std::uint16_t* writers;   // slot per 4 KiB page
    const std::uint16_t* readers;
    const std::uint64_t* slot_sequence;
    const std::uint64_t* completed_seen;
    const std::atomic<bool>* failed;
};

// The backend reads `failed` as one byte.
static_assert(sizeof(std::atomic<bool>) == 1 && std::atomic<bool>::is_always_lock_free);

// What every compiled block receives as its only argument. The pointers (guest, ram,
// code_pages) must not change while a block runs: the block may keep them in registers.
struct Context {
    std::uint8_t* guest;                 // base the front-end's guest offsets are relative to
    std::uint8_t* ram;                   // big-endian RAM, physical address 0
    std::uint32_t ram_size;
    const std::uint8_t* code_pages;      // one byte per 4 KiB of ram, nonzero: holds compiled code
    const PageBarrierView* barrier;      // nullptr: no barrier
    std::int64_t budget;                 // cycles the current block may still use (strict)
    std::int64_t extra_cycles;           // dynamic cycles added by helpers in this block
    void* user;                          // the front-end
    // The slow path the backend calls; noexcept, set by the front-end. No C++ exception may
    // cross a native frame. (A barrier's slow path is an exit, not a call: IrBuilder::barrier.)
    std::uint32_t (*code_write)(Context*, std::uint32_t paddr, std::uint32_t bytes) noexcept;  // 1 = compiled code changed
};

enum class ContextField : std::uint8_t { Budget, ExtraCycles };

// Byte offset of a field the IR can read and write.
[[nodiscard]] constexpr std::size_t context_offset(ContextField field) {
    return field == ContextField::Budget ? offsetof(Context, budget) : offsetof(Context, extra_cycles);
}

}  // namespace ezy::jit
