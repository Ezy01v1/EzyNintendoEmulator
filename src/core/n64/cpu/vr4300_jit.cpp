// VR4300 JIT: block building and the dispatcher. Every instruction still runs through the
// interpreter, called from the block ("in place"); what the block adds is skipping the
// per-instruction fetch, interrupt check and retire, which execute() does once at the end.
#include "n64/cpu/vr4300_jit.hpp"

#include <algorithm>
#include <chrono>
#include <limits>
#include <span>
#include <utility>
#include <vector>

#include "jit/ir.hpp"
#include "jit/x64_backend.hpp"

namespace ezy::n64 {

namespace {

constexpr std::uint64_t kKseg0 = 0xFFFFFFFF80000000ULL;

std::uint32_t offset_of(const Vr4300& cpu, const void* member) {
    return static_cast<std::uint32_t>(static_cast<const std::uint8_t*>(member) -
                                      reinterpret_cast<const std::uint8_t*>(&cpu));
}

std::uint64_t elapsed_ns(std::chrono::steady_clock::time_point since) {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - since).count());
}

}  // namespace

Vr4300Jit::Vr4300Jit(Vr4300& cpu, CpuBus& bus, std::size_t code_budget)
    : cpu_(cpu), bus_(bus), cache_(bus.direct_ram_size(), code_budget) {
    available_ = cache_.code().available();
    // Vr4300 is not standard-layout, so no offsetof: plain pointer differences.
    offsets_.gpr = offset_of(cpu, cpu.gpr_.data());
    offsets_.hi = offset_of(cpu, &cpu.hi_);
    offsets_.lo = offset_of(cpu, &cpu.lo_);
    offsets_.pc = offset_of(cpu, &cpu.pc_);
    offsets_.next_pc = offset_of(cpu, &cpu.next_pc_);
    offsets_.branch_taken = offset_of(cpu, &cpu.branch_taken_);
    offsets_.idle_candidate = offset_of(cpu, &cpu.idle_candidate_);
    ctx_.guest = reinterpret_cast<std::uint8_t*>(&cpu);
    ctx_.ram = bus.direct_ram();
    ctx_.ram_size = bus.direct_ram_size();
    ctx_.code_pages = cache_.pages().table();
    ctx_.barrier = nullptr;
    ctx_.user = this;
}

Vr4300Jit::Stats Vr4300Jit::stats() const {
    Stats s = stats_;
    s.invalidations = cache_.stats().invalidations;
    s.flushes = cache_.stats().flushes;
    return s;
}

void Vr4300Jit::invalidate_ram(std::uint32_t begin, std::uint32_t end) { cache_.invalidate(begin, end); }

void Vr4300Jit::flush() { cache_.flush(); }

bool Vr4300Jit::kernel_mode() const { return cpu_.mode() == Vr4300::Mode::Kernel; }

// What the compiled code depends on besides the words: 64-bit ops allowed, CU1 and FR.
std::uint32_t Vr4300Jit::mode_key() const {
    const std::uint32_t status = cpu_.cop0_.status();
    return (cpu_.allow_64bit_ops() ? 1U : 0U) | ((status & Cop0::kCU1) ? 2U : 0U) | ((status & Cop0::kFR) ? 4U : 0U);
}

std::uint64_t Vr4300Jit::run_instruction(jit::Context* ctx, std::uint64_t instr, std::uint64_t flags,
                                         std::uint64_t block_cycles) noexcept {
    auto& self = *static_cast<Vr4300Jit*>(ctx->user);
    try {
        return self.run(static_cast<std::uint32_t>(instr), flags, block_cycles);
    } catch (...) {
        // No C++ exception may cross the native frames: execute() rethrows it.
        self.error_ = std::current_exception();
        return kBefore;
    }
}

std::uint64_t Vr4300Jit::run(std::uint32_t instr, std::uint64_t flags, std::uint64_t block_cycles) {
    int cycles = 1;
    Vr4300::InPlace status = Vr4300::InPlace::Ok;
    std::uint32_t store_begin = 0, store_bytes = 0;
    if (flags & kFlagMemory) {
        const MemoryAccess access = memory_access(instr);
        if (access.store) {  // its range, from the base register as it is before the store
            const std::uint64_t base = cpu_.gpr_[(instr >> 21) & 31];
            std::uint64_t vaddr = base + static_cast<std::uint64_t>(static_cast<std::int16_t>(instr & 0xFFFF));
            if (access.aligned_down) vaddr &= ~std::uint64_t{access.bytes - 1U};
            store_begin = static_cast<std::uint32_t>(vaddr & 0x1FFFFFFF);
            store_bytes = access.bytes;
        }
        status = cpu_.execute_memory_in_place(instr, cycles);
    } else {
        status = cpu_.execute_in_place(instr, cycles);
    }
    if (status == Vr4300::InPlace::ExitBefore || status == Vr4300::InPlace::HostError) return kBefore;
    ctx_.extra_cycles += cycles - 1;
    if (status != Vr4300::InPlace::Ok) return kAfter;  // Exception, CodeChanged: it retired
    // A store over compiled code: drop those blocks (this one's memory stays valid until a
    // flush) and leave, since this block may be one of them.
    if (store_bytes != 0 && cache_.pages().marked(store_begin, store_begin + store_bytes) &&
        cache_.invalidate(store_begin, store_begin + store_bytes)) {
        return kAfter;
    }
    // Extra cycles: done + extra + remaining static = the block's static cycles + extra. If
    // that reaches the budget, leave now (only this instruction may overshoot it).
    if (cycles > 1 && ctx_.extra_cycles + static_cast<std::int64_t>(block_cycles) >= ctx_.budget) return kAfter;
    // A branch-likely not taken skipped its delay slot: the block must not run it.
    if ((flags & kFlagBranch) && !cpu_.branch_taken_) return kSkip;
    return kOk;
}

// Per instruction i of n: call the helper with (word, flags, n), then leave if it says so:
//   kBefore  -> {i, i}       it did not run (ExitBefore) or a host error interrupted it
//   kAfter   -> {i+1, i+1}   exception, code change, or its extra cycles reached the budget
//   kSkip    -> {i+1, i+1}   a likely branch skipped the slot: a normal end of the block
const jit::Block* Vr4300Jit::compile(std::uint32_t paddr, std::uint32_t key) {
    const auto started = std::chrono::steady_clock::now();
    if (cache_.arena_bytes() > cache_.code().budget()) cache_.flush();

    const std::uint32_t page_end = (paddr | 0xFFFU) + 1;
    const std::uint32_t end = std::min(page_end, bus_.direct_ram_size());
    const std::uint32_t max_words = std::min<std::uint32_t>(kMaxBlockInstructions, (end - paddr) / 4);
    bus_.guard_memory(paddr, std::size_t{max_words} * 4, false);  // the threaded RDP may still write there
    const std::uint8_t* ram = bus_.direct_ram() + paddr;
    const auto word = [ram](std::uint32_t i) {
        const std::uint8_t* m = ram + 4 * i;
        return (std::uint32_t{m[0]} << 24) | (std::uint32_t{m[1]} << 16) | (std::uint32_t{m[2]} << 8) | m[3];
    };

    std::vector<std::uint32_t> words;
    words.reserve(max_words);
    for (std::uint32_t i = 0; i < max_words; ++i) {
        const std::uint32_t w = word(i);
        if (classify(w) == Kind::OutOfBlock) break;
        if (is_branch(w)) {
            // The block ends after the slot; a slot that does not fit, or that is itself a
            // branch or out of block, ends it before the branch.
            if (i + 1 >= max_words) break;
            const std::uint32_t slot = word(i + 1);
            if (classify(slot) == Kind::OutOfBlock || is_branch(slot)) break;
            words.push_back(w);
            words.push_back(slot);
            break;
        }
        words.push_back(w);
    }

    jit::Block block;
    block.key = key;
    block.paddr = paddr;
    if (words.empty()) {  // the interpreter runs this pc; cached so it is not scanned again
        const std::uint32_t first = word(0);
        block.words = 1;
        const jit::Block& stored = cache_.insert(block, {}, std::span<const std::uint32_t>(&first, 1));
        stats_.compile_ns += elapsed_ns(started);
        return &stored;
    }

    const auto n = static_cast<std::uint32_t>(words.size());
    jit::IrBuilder b;
    std::vector<jit::BlockExit> exits;
    const auto add_exit = [&](std::uint32_t count, std::uint32_t kind) {
        exits.push_back({count, count, kind});
        return b.add_exit();
    };
    const auto is = [&](jit::Value r, Status st) {
        return b.compare(jit::Cond::Eq, r, b.constant(jit::Type::I64, st));
    };
    for (std::uint32_t i = 0; i < n; ++i) {
        const std::uint32_t w = words[i];
        const std::uint64_t flags = (classify(w) == Kind::Memory ? kFlagMemory : 0) | (is_branch(w) ? kFlagBranch : 0);
        const jit::Value r = b.call(&run_instruction, b.constant(jit::Type::I64, w),
                                    b.constant(jit::Type::I64, flags), b.constant(jit::Type::I64, n));
        b.exit_if(is(r, kBefore), add_exit(i, kExitBefore));
        b.exit_if(is(r, kAfter), add_exit(i + 1, kExitAfter));
        if (flags & kFlagBranch) b.exit_if(is(r, kSkip), add_exit(i + 1, kNormal));
    }
    b.exit(add_exit(n, kNormal));
    const jit::IrBlock ir = b.finish();

    jit::Compiled compiled = jit::compile(ir, cache_.code());
    if (compiled.error == jit::CompileError::OutOfSpace) {
        cache_.flush();  // outside native code: every older block is gone, none is running
        compiled = jit::compile(ir, cache_.code());
    }
    // Still failing (a block larger than the whole buffer, or unsupported): the interpreter runs it.
    block.fn = compiled.error == jit::CompileError::None ? compiled.fn : nullptr;
    block.words = n;
    block.min_cycles = n;  // one cycle each; the extra ones are dynamic
    const jit::Block& stored = cache_.insert(block, exits, words);
    if (block.fn != nullptr) ++stats_.compiled;
    stats_.compile_ns += elapsed_ns(started);
    return &stored;
}

void Vr4300Jit::retire(std::uint64_t cycles, std::uint64_t instructions, std::uint64_t until_compare) {
    if (instructions == 0) return;
    cpu_.cop0_.retire_many(cycles, instructions);
    // Only the last instruction can reach Compare (each block started below the limit and
    // leaves as soon as its extra cycles reach it): set IP7 as its retire() would.
    if (cycles >= until_compare) cpu_.cop0_.raw(Cop0::kCause) |= Cop0::kIP7;
}

Vr4300Jit::Result Vr4300Jit::execute(std::uint64_t event_budget) {
    Result result;
    // The interpreter takes these steps: an interrupt, a delay slot, other modes.
    if (!available_ || cpu_.branch_taken_ || cpu_.next_pc_ != cpu_.pc_ + 4 || cpu_.interrupt_pending() ||
        !kernel_mode()) {
        return result;
    }
    const std::uint64_t until_compare = cpu_.cycles_until_compare();
    const std::uint64_t limit = std::min(event_budget, until_compare);
    const std::uint32_t key = mode_key();
    bool line_latched = false;
    std::exception_ptr failure;
    try {
        for (;;) {
            const std::uint64_t pc = cpu_.pc_;
            if (pc - kKseg0 >= 0x20000000ULL) break;  // kseg0 only
            if (pc & 3) break;  // a JR to an unaligned target: step()'s fetch raises AdEL
            const auto paddr = static_cast<std::uint32_t>(pc & 0x1FFFFFFF);
            if (std::uint64_t{paddr} + 4 > bus_.direct_ram_size()) break;
            const jit::Block* block = cache_.find(paddr, key);
            if (block == nullptr) block = compile(paddr, key);
            if (block->fn == nullptr) {
                if (classify(block->code[0]) == Kind::OutOfBlock) ++stats_.out_of_block;
                break;
            }
            if (result.cycles + block->min_cycles >= limit) break;
            if (!line_latched) {  // what each step() does first; the line cannot change in here
                cpu_.cop0_.set_ip2(bus_.interrupt_line());
                line_latched = true;
            }
            ctx_.budget = static_cast<std::int64_t>(
                std::min<std::uint64_t>(limit - result.cycles, std::numeric_limits<std::int64_t>::max()));
            ctx_.extra_cycles = 0;
            const std::uint32_t id = block->fn(&ctx_);
            const jit::BlockExit& exit = block->exits[id];
            result.cycles += exit.cycles + static_cast<std::uint64_t>(ctx_.extra_cycles);
            result.instructions += exit.instructions;
            stats_.in_place += exit.instructions;
            if (error_) {
                failure = std::exchange(error_, nullptr);
                break;
            }
            if (exit.kind != kNormal) {  // a side exit ends the run
                if (exit.kind == kExitBefore) ++stats_.exits_before;
                break;
            }
        }
    } catch (...) {  // from compile(): the memory guard
        failure = std::current_exception();
    }
    // What ran is retired even when a host error follows, as step() would have.
    retire(result.cycles, result.instructions, until_compare);
    if (failure) std::rethrow_exception(failure);
    return result;
}

}  // namespace ezy::n64
