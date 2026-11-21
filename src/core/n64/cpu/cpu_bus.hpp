#pragma once

#include <cstdint>
#include <cstddef>
#include <optional>

#include "jit/context.hpp"

namespace ezy::n64 {

// What the VR4300 sees of the system: physical, big-endian memory accesses. Address
// translation (kseg0/kseg1/TLB) happens in the CPU, so tests can use a flat array.
class CpuBus {
public:
    virtual ~CpuBus() = default;

    virtual std::uint8_t read8(std::uint32_t paddr) = 0;
    virtual std::uint16_t read16(std::uint32_t paddr) = 0;
    virtual std::uint32_t read32(std::uint32_t paddr) = 0;
    virtual std::uint64_t read64(std::uint32_t paddr) = 0;
    virtual void write8(std::uint32_t paddr, std::uint8_t value) = 0;
    virtual void write16(std::uint32_t paddr, std::uint16_t value) = 0;
    virtual void write32(std::uint32_t paddr, std::uint32_t value) = 0;
    virtual void write64(std::uint32_t paddr, std::uint64_t value) = 0;

    // Extra CPU cycles for one uncached access to this physical address.
    [[nodiscard]] virtual std::uint32_t uncached_cycles(std::uint32_t paddr) const = 0;
    // The RCP's interrupt line (MI), wired to Cause.IP2.
    [[nodiscard]] virtual bool interrupt_line() const = 0;

    // Plain memory at physical address 0 (RDRAM) that the CPU may read and write directly,
    // skipping the virtual calls. Accesses there must have no side effects. Empty by default.
    [[nodiscard]] std::uint8_t* direct_ram() const { return direct_ram_; }
    [[nodiscard]] std::uint32_t direct_ram_size() const { return direct_ram_size_; }
    void guard_memory(std::uint32_t address, std::size_t bytes, bool write) const {
        if (memory_guard_) prepare_memory(address, bytes, write);
    }
    virtual void prepare_memory(std::uint32_t, std::size_t, bool) const {}
    // The inline form of prepare_memory()'s check for direct RAM, when the bus has one (a
    // threaded RDP): an access whose page the view says is clear needs no prepare_memory()
    // call. None (the default): prepare_memory() never waits for direct RAM. The CPU JIT
    // reads it again on every Vr4300Jit::execute(), as the renderer may change in between.
    [[nodiscard]] virtual std::optional<jit::PageBarrierView> barrier_view() const { return std::nullopt; }

    // The code watch: every writer of direct RAM (CPU stores, DMA, HLE tasks, the RDP)
    // reports the range it writes through note_ram_write(), so compiled code over it can be
    // dropped. `pages` holds one byte per 4 KiB page of direct RAM, nonzero where code was
    // compiled (the CPU JIT's table); `fn` runs only for ranges over such pages, once per run
    // of consecutive marked pages, with the range clamped to them. pages == nullptr: off.
    // One watcher per bus.
    static constexpr unsigned kCodePageShift = 12;
    using CodeWriteFn = void (*)(void* user, std::uint32_t begin, std::uint32_t end);
    void set_code_watch(const std::uint8_t* pages, CodeWriteFn fn, void* user) {
        code_pages_ = fn != nullptr ? pages : nullptr;
        code_write_ = fn;
        code_user_ = user;
    }
    [[nodiscard]] void* code_watch_user() const { return code_user_; }
    // [paddr, paddr + bytes) was (or is about to be) written; the part outside direct RAM
    // is ignored. With no watch: one null check.
    void note_ram_write(std::uint32_t paddr, std::size_t bytes) const {
        if (code_pages_ == nullptr || bytes == 0 || paddr >= direct_ram_size_) return;
        const std::uint32_t end =
            bytes >= direct_ram_size_ - paddr ? direct_ram_size_ : paddr + static_cast<std::uint32_t>(bytes);
        const std::uint32_t first = paddr >> kCodePageShift, last = (end - 1) >> kCodePageShift;
        if (first == last) {  // the common case: a store, a small DMA
            if (code_pages_[first] != 0) code_write_(code_user_, paddr, end);
            return;
        }
        note_pages(paddr, end, first, last);
    }

protected:
    void set_memory_guard(bool enabled) { memory_guard_ = enabled; }
    void set_direct_ram(std::uint8_t* base, std::uint32_t size) {
        direct_ram_ = base;
        direct_ram_size_ = size;
    }

private:
    void note_pages(std::uint32_t begin, std::uint32_t end, std::uint32_t first, std::uint32_t last) const {
        for (std::uint32_t p = first; p <= last; ++p) {
            if (code_pages_[p] == 0) continue;
            std::uint32_t run_end = p;
            while (run_end < last && code_pages_[run_end + 1] != 0) ++run_end;
            const std::uint32_t from = p == first ? begin : p << kCodePageShift;
            const std::uint32_t to = run_end == last ? end : (run_end + 1) << kCodePageShift;
            code_write_(code_user_, from, to);
            p = run_end;
        }
    }

    std::uint8_t* direct_ram_ = nullptr;
    std::uint32_t direct_ram_size_ = 0;
    bool memory_guard_ = false;
    const std::uint8_t* code_pages_ = nullptr;
    CodeWriteFn code_write_ = nullptr;
    void* code_user_ = nullptr;
};

}  // namespace ezy::n64
