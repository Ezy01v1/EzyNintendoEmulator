// VR4300 JIT: block building and the dispatcher. The integer ALU, the branches and the
// aligned loads and stores run as native code (vr4300_jit_alu.cpp, vr4300_jit_branch.cpp,
// vr4300_jit_memory.cpp); every other instruction runs through the interpreter, called
// from the block ("in place"). Either way the block skips the per-instruction fetch,
// interrupt check and retire, which execute() does once at the end.
#include "n64/cpu/vr4300_jit.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <limits>
#include <span>
#include <stdexcept>
#include <utility>
#include <vector>

#include "jit/ir.hpp"
#include "jit/x64_backend.hpp"
#include "n64/cpu/vr4300_jit_builder.hpp"

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

// ---- BlockBuilder (vr4300_jit_builder.hpp) ----

std::uint32_t BlockBuilder::reg_offset(int index) const {
    if (index == kHi) return offsets_.hi;
    if (index == kLo) return offsets_.lo;
    return offsets_.gpr + 8 * static_cast<std::uint32_t>(index);
}

jit::Value BlockBuilder::reg(int index) {
    auto& id = cached_[static_cast<std::size_t>(index)];
    if (id == kNone) id = ir_.load_guest(jit::Type::I64, reg_offset(index)).id;
    return jit::Value{id};
}

void BlockBuilder::set_reg(int index, jit::Value value) {
    cached_[static_cast<std::size_t>(index)] = value.id;
    dirty_ |= std::uint64_t{1} << index;
}

jit::Value BlockBuilder::gpr(int index) { return index == 0 ? i64(0) : reg(index); }

void BlockBuilder::set_gpr(int index, jit::Value value) {
    if (index != 0) set_reg(index, value);
}

jit::Value BlockBuilder::idle_candidate() { return ir_.load_guest(jit::Type::I64, offsets_.idle_candidate); }

void BlockBuilder::set_idle_candidate(jit::Value value) { ir_.store_guest(offsets_.idle_candidate, value); }

jit::Value BlockBuilder::next_value() { return next_is_imm_ ? i64(next_imm_) : next_; }

void BlockBuilder::store_pc_state(PcState state, std::uint64_t at) {
    const auto flags = [this](bool taken, bool slot) {
        ir_.store_guest(offsets_.branch_taken, i32(taken ? 1 : 0), 1);
        ir_.store_guest(offsets_.in_delay_slot, i32(slot ? 1 : 0), 1);
    };
    switch (state) {
        case PcState::Clean: break;
        case PcState::Sequential:
            ir_.store_guest(offsets_.pc, i64(at));
            ir_.store_guest(offsets_.next_pc, i64(at + 4));
            flags(false, false);
            break;
        case PcState::InSlot:
            ir_.store_guest(offsets_.pc, i64(at));
            ir_.store_guest(offsets_.next_pc, next_value());
            flags(true, false);
            break;
        case PcState::AfterSlot:
            ir_.store_guest(offsets_.pc, next_value());
            ir_.store_guest(offsets_.next_pc, next_is_imm_ ? i64(next_imm_ + 4)
                                                          : ir_.binary(jit::Op::Add, next_, i64(4)));
            flags(false, true);
            break;
    }
}

void BlockBuilder::store_registers() {
    for (int index = 0; index < kRegs; ++index) {
        if (dirty_ & (std::uint64_t{1} << index)) {
            ir_.store_guest(reg_offset(index), jit::Value{cached_[static_cast<std::size_t>(index)]});
        }
    }
}

void BlockBuilder::write_back() {
    store_registers();
    store_pc_state(pc_state_, vaddr_);
}

jit::ExitId BlockBuilder::add_exit(std::uint32_t instructions, std::uint32_t cycles, std::uint32_t native,
                                   std::uint32_t kind) {
    exits_.push_back({cycles, instructions, kind, native});
    return ir_.add_exit();
}

jit::ExitId BlockBuilder::exit_before() {
    const jit::ExitId exit = add_exit(done_, cycles_, native_, kExitBefore);
    ir_.begin_exit(exit);
    write_back();  // what the instructions before this one left
    ir_.end_exit();
    return exit;
}

void BlockBuilder::exit_before_if(jit::Value cond) { ir_.exit_if(cond, exit_before()); }

void BlockBuilder::exit_after_if(jit::Value cond, std::uint32_t cycles) {
    // What end_native() is about to leave: after a native branch's slot, AfterSlot (pc_ = N);
    // otherwise sequential at the next instruction.
    const jit::ExitId exit = add_exit(done_ + 1, cycles_ + cycles, native_ + 1, kExitAfter);
    ir_.begin_exit(exit);
    store_registers();
    store_pc_state(pc_state_ == PcState::InSlot ? PcState::AfterSlot : PcState::Sequential, vaddr_ + 4);
    ir_.end_exit();
    ir_.exit_if(cond, exit);
}

void BlockBuilder::skip_slot_if(jit::Value not_taken) {
    // The branch retires; step() then leaves the CPU sequential at the slot + 4.
    const jit::ExitId exit = add_exit(done_ + 1, cycles_ + 1, native_ + 1, kExitNormal);
    ir_.begin_exit(exit);
    store_registers();
    store_pc_state(PcState::Sequential, vaddr_ + 8);
    ir_.end_exit();
    ir_.exit_if(not_taken, exit);
}

void BlockBuilder::end_native(std::uint32_t cycles) {
    ++done_;
    ++native_;
    cycles_ += cycles;
    vaddr_ += 4;
    pc_state_ = pc_state_ == PcState::InSlot ? PcState::AfterSlot : PcState::Sequential;
}

void BlockBuilder::end_branch(jit::Value next) {
    end_native(1);
    pc_state_ = PcState::InSlot;
    next_ = next;
    next_is_imm_ = false;
}

void BlockBuilder::end_branch(std::uint64_t next) {
    end_native(1);
    pc_state_ = PcState::InSlot;
    next_imm_ = next;
    next_is_imm_ = true;
}

void BlockBuilder::sync() {
    write_back();
    dirty_ = 0;
    pc_state_ = PcState::Clean;
    cached_.fill(kNone);
}

jit::ExitId BlockBuilder::helper_exit(std::uint32_t kind, bool after) {
    return after ? add_exit(done_ + 1, cycles_ + 1, native_, kind) : add_exit(done_, cycles_, native_, kind);
}

void BlockBuilder::end_in_place() {
    ++done_;
    ++cycles_;
    vaddr_ += 4;
}

void BlockBuilder::finish() {
    const jit::ExitId exit = add_exit(done_, cycles_, native_, kExitNormal);
    ir_.begin_exit(exit);
    write_back();
    ir_.end_exit();
    ir_.exit(exit);
}

// ---- Vr4300Jit ----

Vr4300Jit::Vr4300Jit(Vr4300& cpu, CpuBus& bus, std::size_t code_budget)
    : cpu_(cpu), bus_(bus), cache_(bus.direct_ram_size(), code_budget) {
    available_ = cache_.code().available();
    // Vr4300 is not standard-layout, so no offsetof: plain pointer differences.
    offsets_.gpr = offset_of(cpu, cpu.gpr_.data());
    offsets_.hi = offset_of(cpu, &cpu.hi_);
    offsets_.lo = offset_of(cpu, &cpu.lo_);
    offsets_.pc = offset_of(cpu, &cpu.pc_);
    offsets_.next_pc = offset_of(cpu, &cpu.next_pc_);
    offsets_.in_delay_slot = offset_of(cpu, &cpu.in_delay_slot_);
    offsets_.branch_taken = offset_of(cpu, &cpu.branch_taken_);
    offsets_.idle_candidate = offset_of(cpu, &cpu.idle_candidate_);
    ctx_.guest = reinterpret_cast<std::uint8_t*>(&cpu);
    ctx_.ram = bus.direct_ram();
    ctx_.ram_size = bus.direct_ram_size();
    ctx_.code_pages = cache_.pages().table();
    ctx_.barrier = nullptr;  // execute() reads the bus's view
    ctx_.code_write = &native_code_write;
    ctx_.user = this;
    static_assert(CpuBus::kCodePageShift == jit::CodePages::kPageShift);
    bus_.set_code_watch(cache_.pages().table(), &on_code_write, this);  // flush() keeps the table
}

Vr4300Jit::~Vr4300Jit() {
    if (bus_.code_watch_user() == this) bus_.set_code_watch(nullptr, nullptr, nullptr);
}

void Vr4300Jit::on_code_write(void* self, std::uint32_t begin, std::uint32_t end) {
    static_cast<Vr4300Jit*>(self)->invalidate_ram(begin, end);
}

std::uint32_t Vr4300Jit::native_code_write(jit::Context* ctx, std::uint32_t paddr, std::uint32_t bytes) noexcept {
    auto& self = *static_cast<Vr4300Jit*>(ctx->user);
    // The catch is defensive: BlockCache::invalidate cannot throw, but an exception must not
    // cross the native frame.
    try {
        // What CpuBus::note_ram_write -> on_code_write does; the store's range is within a page.
        return self.cache_.invalidate(paddr, paddr + bytes) ? 1 : 0;
    } catch (...) {
        self.error_ = std::current_exception();
        return 1;  // leave: execute() rethrows it
    }
}

Vr4300Jit::Stats Vr4300Jit::stats() const {
    Stats s = stats_;
    s.invalidations = cache_.stats().invalidations;
    s.flushes = cache_.stats().flushes;
    return s;
}

void Vr4300Jit::invalidate_ram(std::uint32_t begin, std::uint32_t end) {
    if (cache_.invalidate(begin, end)) code_changed_ = true;
}

void Vr4300Jit::verify(const jit::Block& block) const {
    const auto words = static_cast<std::uint32_t>(block.code.size());
    bus_.guard_memory(block.paddr, std::size_t{words} * 4, false);  // the threaded RDP may still write there
    const std::uint8_t* m = bus_.direct_ram() + block.paddr;
    for (std::uint32_t i = 0; i < words; ++i, m += 4) {
        const std::uint32_t ram = (std::uint32_t{m[0]} << 24) | (std::uint32_t{m[1]} << 16) | (std::uint32_t{m[2]} << 8) | m[3];
        if (ram == block.code[i]) continue;
        char message[160];
        std::snprintf(message, sizeof(message),
                      "CPU JIT check: the block at 0x%08X no longer matches RAM (0x%08X: compiled 0x%08X, RAM 0x%08X)",
                      block.paddr, block.paddr + 4 * i, block.code[i], ram);
        throw std::runtime_error(message);
    }
}

void Vr4300Jit::flush() { cache_.flush(); }

bool Vr4300Jit::kernel_mode() const { return cpu_.mode() == Vr4300::Mode::Kernel; }

// What the compiled code depends on besides the words: whether 64-bit ops are allowed.
// CU1 and FR are left out on purpose: COP1 runs in place and the interpreter checks them
// itself, while libultra flips CU1 between threads (on MK64 most compiles were replacements
// of the same address for the other CU1 value). A native FPU must add them back, with
// several keys per address so both variants stay cached.
std::uint32_t Vr4300Jit::mode_key() const { return cpu_.allow_64bit_ops() ? 1U : 0U; }

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
    code_changed_ = false;
    Vr4300::InPlace status = (flags & kFlagMemory) ? cpu_.execute_memory_in_place(instr, cycles)
                                                   : cpu_.execute_in_place(instr, cycles);
    // A store over compiled code: the code watch already dropped those blocks (this one's
    // memory stays valid until a flush); leave, since this block may be one of them.
    if (status == Vr4300::InPlace::Ok && code_changed_) status = Vr4300::InPlace::CodeChanged;
    if (status == Vr4300::InPlace::ExitBefore) return kBefore;
    ctx_.extra_cycles += cycles - 1;
    if (status != Vr4300::InPlace::Ok) return kAfter;  // Exception, CodeChanged: it retired
    // Extra cycles: done + extra + remaining static = the block's static cycles + extra. If
    // that reaches the budget, leave now (only this instruction may overshoot it).
    if (cycles > 1 && ctx_.extra_cycles + static_cast<std::int64_t>(block_cycles) >= ctx_.budget) return kAfter;
    // A branch-likely not taken skipped its delay slot: the block must not run it.
    if ((flags & kFlagBranch) && !cpu_.branch_taken_) return kSkip;
    return kOk;
}

// A native instruction is emitted by emit_alu (an overflow leaves before it), emit_branch
// (a likely branch not taken leaves after it, its slot nullified) or emit_memory (an
// access outside kseg0 RAM, unaligned, or that must wait for the renderer leaves before
// it; a store that dropped compiled code leaves after it, kExitAfter). For an
// in-place instruction i: write the pending state back, call the helper with (word, flags,
// the block's static cycles), then leave if it says so:
//   kBefore  -> {i}       it did not run (ExitBefore) or a host error interrupted it
//   kAfter   -> {i+1}     exception, code change, or its extra cycles reached the budget
//   kSkip    -> {i+1}     an in-place likely branch (BC1*L) skipped the slot: a normal end
const jit::Block* Vr4300Jit::compile(std::uint32_t paddr, std::uint32_t key) {
    const auto started = std::chrono::steady_clock::now();
    if (cache_.arena_bytes() > cache_.code().budget()) cache_.flush();

    const std::uint32_t page_end = (paddr | 0xFFFU) + 1;
    const std::uint32_t end = std::min(page_end, bus_.direct_ram_size());
    const std::uint32_t max_words = std::min<std::uint32_t>(kMaxBlockInstructions, (end - paddr) / 4);
    // The threaded RDP may still write there: it reported the write when the draw was
    // submitted, so waiting for it before reading the words keeps the block current.
    bus_.guard_memory(paddr, std::size_t{max_words} * 4, false);
    const std::uint8_t* ram = bus_.direct_ram() + paddr;
    const auto word = [ram](std::uint32_t i) {
        const std::uint8_t* m = ram + 4 * i;
        return (std::uint32_t{m[0]} << 24) | (std::uint32_t{m[1]} << 16) | (std::uint32_t{m[2]} << 8) | m[3];
    };

    std::vector<std::uint32_t> words;
    words.reserve(max_words);
    for (std::uint32_t i = 0; i < max_words; ++i) {
        const std::uint32_t w = word(i);
        if (classify(w, true) == Kind::OutOfBlock) break;
        if (is_branch(w)) {
            // The block ends after the slot; a slot that does not fit, or that is itself a
            // branch or out of block, ends it before the branch.
            if (i + 1 >= max_words) break;
            const std::uint32_t slot = word(i + 1);
            if (classify(slot, true) == Kind::OutOfBlock || is_branch(slot)) break;
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
    const bool allow_64bit = (key & 1) != 0;
    // Native or in place, and the static cycles: a native instruction's are all static (the
    // multiplies' extra ones included); an in-place one counts 1, its extra cycles are dynamic.
    std::vector<std::uint32_t> native_cycles(n, 0);
    std::uint32_t static_cycles = 0;
    for (std::uint32_t i = 0; i < n; ++i) {
        const std::uint32_t w = words[i];
        // The slot of an in-place branch (BC1*) stays in place: only the CPU knows the
        // next_pc_ that branch left.
        const bool after_in_place_branch = i > 0 && is_branch(words[i - 1]) && native_cycles[i - 1] == 0;
        if (!after_in_place_branch && classify(w, allow_64bit) == Kind::Native) {
            native_cycles[i] = is_branch(w) || memory_access(w).bytes != 0 ? 1 : alu_cycles(w, allow_64bit);
        }
        static_cycles += native_cycles[i] != 0 ? native_cycles[i] : 1;
    }

    jit::IrBuilder b;
    BlockBuilder builder(b, offsets_, kKseg0 + paddr, allow_64bit, ctx_.ram_size);
    const auto is = [&](jit::Value r, Status st) {
        return b.compare(jit::Cond::Eq, r, b.constant(jit::Type::I64, st));
    };
    for (std::uint32_t i = 0; i < n; ++i) {
        const std::uint32_t w = words[i];
        if (native_cycles[i] != 0) {
            if (is_branch(w)) {
                if (!emit_branch(builder, w)) throw std::logic_error("CPU JIT: a native branch has no emitter");
                continue;  // emit_branch ended it
            }
            if (!emit_memory(builder, w) && !emit_alu(builder, w)) {
                throw std::logic_error("CPU JIT: a Native instruction has no emitter");
            }
            builder.end_native(native_cycles[i]);
            continue;
        }
        builder.sync();
        // A native load or store left in place (the slot of BC1*) still needs the memory checks.
        const std::uint64_t flags =
            (memory_access(w).bytes != 0 ? kFlagMemory : 0) | (is_branch(w) ? kFlagBranch : 0);
        const jit::Value r = b.call(&run_instruction, b.constant(jit::Type::I64, w),
                                    b.constant(jit::Type::I64, flags), b.constant(jit::Type::I64, static_cycles));
        b.exit_if(is(r, kBefore), builder.helper_exit(kExitBefore, false));
        b.exit_if(is(r, kAfter), builder.helper_exit(kExitAfter, true));
        if (flags & kFlagBranch) b.exit_if(is(r, kSkip), builder.helper_exit(kExitNormal, true));
        builder.end_in_place();
    }
    builder.finish();
    const jit::IrBlock ir = b.finish();

    jit::Compiled compiled = jit::compile(ir, cache_.code());
    if (compiled.error == jit::CompileError::OutOfSpace) {
        cache_.flush();  // outside native code: every older block is gone, none is running
        compiled = jit::compile(ir, cache_.code());
    }
    // Still failing (a block larger than the whole buffer, or unsupported): the interpreter runs it.
    block.fn = compiled.error == jit::CompileError::None ? compiled.fn : nullptr;
    block.words = n;
    block.min_cycles = static_cycles;
    const jit::Block& stored = cache_.insert(block, builder.exits(), words);
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
    // The renderer's barrier view: a threaded RDP may come or go between calls.
    if (const auto view = bus_.barrier_view()) {
        barrier_view_ = *view;
        ctx_.barrier = &barrier_view_;
    } else {
        ctx_.barrier = nullptr;
    }
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
            // An arbitrary resume (after a stop for the budget or an event) inside a compiled
            // block: compiling here would build a near-duplicate; the interpreter steps on until
            // a jump lands somewhere. A jump (in_delay_slot_ still set) may be a loop head.
            if (block == nullptr && !cpu_.in_delay_slot_ && cache_.covered(paddr)) {
                ++stats_.resume_skips;
                break;
            }
            if (block == nullptr) block = compile(paddr, key);
            if (check_) verify(*block);
            if (block->fn == nullptr) {
                if (classify(block->code[0], true) == Kind::OutOfBlock) ++stats_.out_of_block;
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
            stats_.in_place += exit.instructions - exit.native;
            stats_.native += exit.native;
            if (error_) {
                failure = std::exchange(error_, nullptr);
                break;
            }
            if (exit.kind != kExitNormal) {  // a side exit ends the run
                if (exit.kind == kExitBefore) ++stats_.exits_before;
                break;
            }
            // At an idle loop: stop so N64::step() can skip it in one go (that batches past
            // the RSP, RDP and SP DMA events, which dispatching this block would not).
            if (cpu_.pc_ == cpu_.idle_candidate_ && cpu_.idle_loop_cycles() != 0) break;
        }
    } catch (...) {  // from compile() (the memory guard) or verify()
        failure = std::current_exception();
    }
    // What ran is retired even when a host error follows, as step() would have.
    retire(result.cycles, result.instructions, until_compare);
    if (failure) std::rethrow_exception(failure);
    return result;
}

}  // namespace ezy::n64
