#include "gc/gekko/gekko.hpp"

#include <cstdio>

#include "gc/gekko/gekko_ops.hpp"

namespace ezy::gc {

namespace {

// Thrown by Gekko::raise() to end the executing instruction once its exception has been
// entered; caught only by step(). Not a std::exception, so nothing else catches it.
struct InstructionAborted {};

std::string hex32(std::uint32_t value) {
    char text[11];
    std::snprintf(text, sizeof(text), "0x%08X", value);
    return text;
}

enum class BatResult { Hit, Miss, Protection };

// The 4 BAT pairs of one side (PEM 7.4). BATU: BEPI (bits 0-14), BL (19-29: block size mask
// in 128 KiB units), Vs (30), Vp (31). BATL: BRPN (0-14), WIMG (25-28), PP (30-31).
BatResult bat_lookup(const std::array<std::uint32_t, 8>& bats, bool user, std::uint32_t ea, bool write,
                     std::uint32_t& paddr) {
    for (std::size_t i = 0; i < bats.size(); i += 2) {
        const std::uint32_t upper = bats[i], lower = bats[i + 1];
        if ((upper & (user ? 1u : 2u)) == 0) continue;
        const std::uint32_t mask = 0xFFFE0000 & ~(((upper >> 2) & 0x7FF) << 17);
        if ((ea & mask) != (upper & mask)) continue;
        // PP: 00 no access, 01 and 11 read-only, 10 read/write.
        const std::uint32_t pp = lower & 3;
        if (pp == 0 || (write && (pp & 1) != 0)) return BatResult::Protection;
        paddr = (lower & mask) | (ea & ~mask);
        return BatResult::Hit;
    }
    return BatResult::Miss;
}

[[noreturn]] void op_unimplemented(Gekko& cpu, std::uint32_t word) { cpu.unimplemented(word); }

void op_table4(Gekko& cpu, std::uint32_t word) { gekko_decode_tables().op4[ppc_fields::xo10(word)](cpu, word); }
void op_table19(Gekko& cpu, std::uint32_t word) { gekko_decode_tables().op19[ppc_fields::xo10(word)](cpu, word); }
void op_table31(Gekko& cpu, std::uint32_t word) { gekko_decode_tables().op31[ppc_fields::xo10(word)](cpu, word); }
void op_table59(Gekko& cpu, std::uint32_t word) { gekko_decode_tables().op59[ppc_fields::xo10(word)](cpu, word); }
void op_table63(Gekko& cpu, std::uint32_t word) { gekko_decode_tables().op63[ppc_fields::xo10(word)](cpu, word); }

GekkoDecodeTables build_tables() {
    GekkoDecodeTables t;
    t.primary.fill(op_unimplemented);
    t.op4.fill(op_unimplemented);
    t.op19.fill(op_unimplemented);
    t.op31.fill(op_unimplemented);
    t.op59.fill(op_unimplemented);
    t.op63.fill(op_unimplemented);
    t.primary[4] = op_table4;
    t.primary[19] = op_table19;
    t.primary[31] = op_table31;
    t.primary[59] = op_table59;
    t.primary[63] = op_table63;
    install_integer_ops(t);
    install_fpu_ops(t);
    install_paired_ops(t);
    return t;
}

}  // namespace

GekkoError::GekkoError(const std::string& what, std::uint32_t pc_, std::uint32_t word_or_address_)
    : std::runtime_error("Gekko: " + what + " at pc " + hex32(pc_)), pc(pc_), word_or_address(word_or_address_) {}

BusError::BusError(const std::string& detail_, std::uint32_t address)
    : GekkoError(FullMessage{}, "bus: " + detail_ + " (pc unknown)", 0, address), detail(detail_), pc_known(false) {}

BusError::BusError(const BusError& from_bus, std::uint32_t pc_)
    : GekkoError(FullMessage{}, "Gekko: bus: " + from_bus.detail + " at pc " + hex32(pc_), pc_,
                 from_bus.word_or_address),
      detail(from_bus.detail),
      pc_known(true) {}

const GekkoDecodeTables& gekko_decode_tables() {
    static const GekkoDecodeTables tables = build_tables();
    return tables;
}

Gekko::Gekko(CpuBus& bus) : bus_(bus), tables_(&gekko_decode_tables()) { reset(); }

void Gekko::reset() {
    s_ = GekkoState{};
    s_.msr = kMsrIp;  // 750 manual, "Reset settings": MSR = 0x00000040
    // HID0 = HID2 = 0 here, as at power-on: the caches are off (dcbz and dcbz_l raise
    // alignment) and psq_l/psq_st are illegal. The IPL enables them before starting a game,
    // so the HLE boot (Task 3) must set them like the IPL does: Dolphin's Boot_BS2Emu.cpp
    // SetupHID uses HID0 = 0x0011C464 (ICE and DCE on) and HID2 = 0xE0000000 (LSQE, WPE, PSE).
    set_pc(0xFFF00000 | static_cast<std::uint32_t>(Exception::SystemReset));
    bus_.set_locked_cache_enabled(false);  // HID2[LCE] = 0: no locked cache
}

int Gekko::step() {
    // Interrupts are taken between instructions, external before decrementer (750 manual,
    // exception priorities).
    if ((s_.msr & kMsrEe) != 0) {
        if (s_.external_interrupt) {
            enter_exception(Exception::External, s_.pc);
            return 1;
        }
        if (s_.decrementer_pending) {
            s_.decrementer_pending = false;
            enter_exception(Exception::Decrementer, s_.pc);
            return 1;
        }
    }

    std::uint32_t paddr = s_.pc;
    if ((s_.msr & kMsrIr) != 0) {
        switch (bat_lookup(s_.ibat, (s_.msr & kMsrPr) != 0, s_.pc, false, paddr)) {
        case BatResult::Hit:
            break;
        case BatResult::Miss:
            enter_exception(Exception::Isi, s_.pc, 0x40000000);  // SRR1[1]: no translation
            return 1;
        case BatResult::Protection:
            enter_exception(Exception::Isi, s_.pc, 0x08000000);  // SRR1[4]: protection
            return 1;
        }
    }
    // The bus cannot know the pc, so its errors get it here: s_.pc is still the instruction
    // whose fetch or access failed. The emulation stops there (the instruction may have
    // half-run: a split access, an lmw); nothing is undone. No cost unless something throws.
    try {
        const std::uint32_t word = bus_.read32(paddr);
        s_.npc = s_.pc + 4;
        try {
            tables_->primary[word >> 26](*this, word);
        } catch (const InstructionAborted&) {
            // The instruction raised an exception: npc already points at its vector.
        }
    } catch (const BusError& e) {
        throw BusError(e, s_.pc);
    }
    s_.pc = s_.npc;
    return 1;
}

void Gekko::advance_timebase(std::uint64_t ticks) {
    s_.tb += ticks;
    const auto before = static_cast<std::int32_t>(s_.dec);
    s_.dec -= static_cast<std::uint32_t>(ticks);
    // The exception is requested when DEC[0] goes from 0 to 1 (PEM 2.3.14.1).
    if (before >= 0 && ticks > static_cast<std::uint64_t>(before)) s_.decrementer_pending = true;
}

std::optional<std::uint32_t> Gekko::translate(std::uint32_t ea, bool instruction, bool write) const {
    if ((s_.msr & (instruction ? kMsrIr : kMsrDr)) == 0) return ea;
    std::uint32_t paddr = 0;
    if (bat_lookup(instruction ? s_.ibat : s_.dbat, (s_.msr & kMsrPr) != 0, ea, write, paddr) != BatResult::Hit)
        return std::nullopt;
    return paddr;
}

std::uint32_t Gekko::data_address(std::uint32_t ea, bool write) {
    if ((s_.msr & kMsrDr) == 0) return ea;
    std::uint32_t paddr = 0;
    const BatResult result = bat_lookup(s_.dbat, (s_.msr & kMsrPr) != 0, ea, write, paddr);
    if (result == BatResult::Hit) return paddr;
    s_.dar = ea;
    s_.dsisr = (result == BatResult::Miss ? kDsisrNoTranslation : kDsisrProtection) | (write ? kDsisrStore : 0);
    raise(Exception::Dsi);
}

std::uint8_t Gekko::read8(std::uint32_t ea) { return bus_.read8(data_address(ea, false)); }

std::uint16_t Gekko::read16(std::uint32_t ea) {
    if ((ea & 1) == 0) return bus_.read16(data_address(ea, false));
    const std::uint32_t first = data_address(ea, false), second = data_address(ea + 1, false);
    return static_cast<std::uint16_t>((bus_.read8(first) << 8) | bus_.read8(second));
}

std::uint32_t Gekko::read32(std::uint32_t ea) {
    if ((ea & 3) == 0) return bus_.read32(data_address(ea, false));
    std::uint32_t paddr[4];
    for (std::uint32_t i = 0; i < 4; ++i) paddr[i] = data_address(ea + i, false);
    std::uint32_t value = 0;
    for (const std::uint32_t p : paddr) value = (value << 8) | bus_.read8(p);
    return value;
}

std::uint64_t Gekko::read64(std::uint32_t ea) {
    if ((ea & 7) == 0) return bus_.read64(data_address(ea, false));
    if ((ea & 3) == 0) {
        const std::uint32_t high = data_address(ea, false), low = data_address(ea + 4, false);
        return (static_cast<std::uint64_t>(bus_.read32(high)) << 32) | bus_.read32(low);
    }
    std::uint32_t paddr[8];
    for (std::uint32_t i = 0; i < 8; ++i) paddr[i] = data_address(ea + i, false);
    std::uint64_t value = 0;
    for (const std::uint32_t p : paddr) value = (value << 8) | bus_.read8(p);
    return value;
}

void Gekko::write8(std::uint32_t ea, std::uint8_t value) { bus_.write8(data_address(ea, true), value); }

// Unaligned stores translate every byte before writing any, so a DSI leaves memory alone.
void Gekko::write16(std::uint32_t ea, std::uint16_t value) {
    if ((ea & 1) == 0) return bus_.write16(data_address(ea, true), value);
    const std::uint32_t first = data_address(ea, true), second = data_address(ea + 1, true);
    bus_.write8(first, static_cast<std::uint8_t>(value >> 8));
    bus_.write8(second, static_cast<std::uint8_t>(value));
}

void Gekko::write32(std::uint32_t ea, std::uint32_t value) {
    if ((ea & 3) == 0) return bus_.write32(data_address(ea, true), value);
    std::uint32_t paddr[4];
    for (std::uint32_t i = 0; i < 4; ++i) paddr[i] = data_address(ea + i, true);
    for (std::uint32_t i = 0; i < 4; ++i) bus_.write8(paddr[i], static_cast<std::uint8_t>(value >> (24 - 8 * i)));
}

void Gekko::write64(std::uint32_t ea, std::uint64_t value) {
    if ((ea & 7) == 0) return bus_.write64(data_address(ea, true), value);
    if ((ea & 3) == 0) {
        const std::uint32_t high = data_address(ea, true), low = data_address(ea + 4, true);
        bus_.write32(high, static_cast<std::uint32_t>(value >> 32));
        bus_.write32(low, static_cast<std::uint32_t>(value));
        return;
    }
    std::uint32_t paddr[8];
    for (std::uint32_t i = 0; i < 8; ++i) paddr[i] = data_address(ea + i, true);
    for (std::uint32_t i = 0; i < 8; ++i) bus_.write8(paddr[i], static_cast<std::uint8_t>(value >> (56 - 8 * i)));
}

void Gekko::enter_exception(Exception kind, std::uint32_t srr0, std::uint32_t srr1_cause) {
    // SRR1 keeps MSR bits 0, 5-9 and 16-31 (bits 1-4 and 10-15 hold the cause); the new MSR
    // clears POW, EE, PR, FP, FE0, SE, BE, FE1, PM, IR, DR and RI, keeps ILE, ME and IP, and
    // copies ILE into LE (PEM 6.4; the masks are Dolphin's PowerPC.cpp CheckExceptions).
    s_.srr0 = srr0;
    s_.srr1 = (s_.msr & 0x87C0FFFF) | srr1_cause;
    const bool ile = (s_.msr & kMsrIle) != 0;
    s_.msr = (s_.msr & ~0x0004EF36u & ~kMsrLe) | (ile ? kMsrLe : 0);
    const std::uint32_t base = (s_.msr & kMsrIp) != 0 ? 0xFFF00000 : 0;
    s_.pc = s_.npc = base | static_cast<std::uint32_t>(kind);
}

void Gekko::raise(Exception kind, std::uint32_t srr1_cause) {
    // sc saves the address after it; every other instruction-caused exception saves the
    // instruction itself, so the handler can retry it.
    enter_exception(kind, kind == Exception::Syscall ? s_.pc + 4 : s_.pc, srr1_cause);
    throw InstructionAborted{};
}

void Gekko::raise_alignment(std::uint32_t ea, std::uint32_t word) {
    // DSISR for alignment (PEM 6.4.6): indexed (X-form) instructions give bits 29-30 in
    // DSISR 15-16, bit 25 in 17 and bits 21-24 in 18-21; D-form ones bit 5 in 17 and bits
    // 1-4 in 18-21. Then rD/rS in 22-26 and rA in 27-31. dcbz_l (opcode 4) is X-form too.
    const std::uint32_t opcode = word >> 26;
    std::uint32_t dsisr = 0;
    if (opcode == 31 || opcode == 4) {
        const std::uint32_t xo = (word >> 1) & 0x3FF;
        dsisr = ((xo & 3) << 15) | (((xo >> 5) & 1) << 14) | (((xo >> 6) & 0xF) << 10);
    } else {
        dsisr = ((opcode & 1) << 14) | (((opcode >> 1) & 0xF) << 10);
    }
    dsisr |= (((word >> 21) & 31) << 5) | ((word >> 16) & 31);
    s_.dar = ea;
    s_.dsisr = dsisr;
    raise(Exception::Alignment);
}

void Gekko::unimplemented(std::uint32_t word) const {
    throw GekkoError("unimplemented instruction " + hex32(word), s_.pc, word);
}

bool Gekko::try_read_spr(int number, std::uint32_t& value) const {
    const auto in = [number](int first, int count) { return number >= first && number < first + count; };
    switch (number) {
    case spr::kXer: value = s_.xer; return true;
    case spr::kLr: value = s_.lr; return true;
    case spr::kCtr: value = s_.ctr; return true;
    case spr::kDsisr: value = s_.dsisr; return true;
    case spr::kDar: value = s_.dar; return true;
    case spr::kDec: value = s_.dec; return true;
    case spr::kSrr0: value = s_.srr0; return true;
    case spr::kSrr1: value = s_.srr1; return true;
    case spr::kTbl:
    case spr::kTblWrite: value = static_cast<std::uint32_t>(s_.tb); return true;
    case spr::kTbu:
    case spr::kTbuWrite: value = static_cast<std::uint32_t>(s_.tb >> 32); return true;
    case spr::kPvr: value = kPvr; return true;
    case spr::kHid0: value = s_.hid0; return true;
    case spr::kHid1: value = s_.hid1; return true;
    case spr::kHid2: value = s_.hid2; return true;
    case spr::kHid4: value = s_.hid4; return true;
    case spr::kWpar: value = s_.wpar; return true;
    case spr::kDmaU: value = s_.dmau; return true;
    case spr::kDmaL: value = s_.dmal; return true;
    case spr::kL2cr: value = s_.l2cr; return true;
    default: break;
    }
    if (in(spr::kSprg0, 4)) value = s_.sprg[static_cast<std::size_t>(number - spr::kSprg0)];
    else if (in(spr::kIbat0u, 8)) value = s_.ibat[static_cast<std::size_t>(number - spr::kIbat0u)];
    else if (in(spr::kDbat0u, 8)) value = s_.dbat[static_cast<std::size_t>(number - spr::kDbat0u)];
    else if (in(spr::kGqr0, 8)) value = s_.gqr[static_cast<std::size_t>(number - spr::kGqr0)];
    else if (in(spr::kMmcr0, spr::kPerfMonitorCount))
        value = s_.perf_monitor[static_cast<std::size_t>(number - spr::kMmcr0)];
    else if (in(spr::kUmmcr0, spr::kPerfMonitorCount))  // the user-level copies read the same registers
        value = s_.perf_monitor[static_cast<std::size_t>(number - spr::kUmmcr0)];
    else return false;
    return true;
}

bool Gekko::try_write_spr(int number, std::uint32_t value) {
    const auto in = [number](int first, int count) { return number >= first && number < first + count; };
    switch (number) {
    case spr::kXer: s_.xer = value & kXerMask; return true;
    case spr::kLr: s_.lr = value; return true;
    case spr::kCtr: s_.ctr = value; return true;
    case spr::kDsisr: s_.dsisr = value; return true;
    case spr::kDar: s_.dar = value; return true;
    case spr::kDec:
        // A write that sets DEC[0] from 0 to 1 requests the exception too (PEM 2.3.14.1;
        // Dolphin's mtspr does the same).
        if ((s_.dec & 0x80000000) == 0 && (value & 0x80000000) != 0) s_.decrementer_pending = true;
        s_.dec = value;
        return true;
    case spr::kSrr0: s_.srr0 = value; return true;
    case spr::kSrr1: s_.srr1 = value; return true;
    case spr::kTblWrite: s_.tb = (s_.tb & 0xFFFFFFFF00000000ULL) | value; return true;
    case spr::kTbuWrite: s_.tb = (s_.tb & 0xFFFFFFFFULL) | (static_cast<std::uint64_t>(value) << 32); return true;
    // HID0: only DCE is read (dcbz/dcbz_l); every other bit (ICE, the cache locks, branch
    // prediction, power modes...) is accepted, no effect, since no cache or pipeline is
    // modeled. ICFI and DCFI (flash-invalidate the instruction / data cache) clear themselves
    // once the invalidation starts (750 manual, HID0 table; Dolphin's mtspr clears ICFI), so
    // they are not stored and read back as 0. A JIT must treat each write with ICFI set as
    // "drop all compiled code"; since the bit never stays set, a later read-modify-write of
    // HID0 does not repeat it.
    case spr::kHid0: s_.hid0 = value & ~(kHid0Icfi | kHid0Dcfi); return true;
    // HID1 (the PLL configuration, read-only on hardware): accepted, no effect.
    case spr::kHid1: s_.hid1 = value; return true;
    // HID2: LCE (dcbz_l; and the bus maps the locked cache only while it is set, so every
    // write tells it) and LSQE (psq_l/psq_st) are read. PSE (paired singles enabled) is
    // accepted, no effect, like Dolphin, which runs paired singles whatever it says. WPE (the
    // write-gather pipe) and the DMA queue/error bits are stored only.
    case spr::kHid2:
        s_.hid2 = value;
        bus_.set_locked_cache_enabled((value & kHid2Lce) != 0);
        return true;
    case spr::kHid4: s_.hid4 = value; return true;  // accepted, no effect
    case spr::kWpar: s_.wpar = value; return true;  // the write-gather pipe address: stored only
    case spr::kDmaU: s_.dmau = value; return true;
    case spr::kDmaL:
        s_.dmal = value;
        locked_cache_dma();
        return true;
    case spr::kL2cr: s_.l2cr = value; return true;  // no L2 cache modeled: accepted, no effect
    default: break;
    }
    if (in(spr::kSprg0, 4)) s_.sprg[static_cast<std::size_t>(number - spr::kSprg0)] = value;
    else if (in(spr::kIbat0u, 8)) s_.ibat[static_cast<std::size_t>(number - spr::kIbat0u)] = value;
    else if (in(spr::kDbat0u, 8)) s_.dbat[static_cast<std::size_t>(number - spr::kDbat0u)] = value;
    else if (in(spr::kGqr0, 8)) s_.gqr[static_cast<std::size_t>(number - spr::kGqr0)] = value;
    // The performance monitor: PMC1-4 and SIA are stored and read back. Counting is not
    // modeled (Dolphin's UpdatePerformanceMonitor counts some events and can raise the
    // performance-monitor exception), so an MMCR write that selects an event or enables that
    // interrupt stops emulation (word_or_address = the value) instead of silently counting
    // nothing; writes with none of those fields (the SDK's zeros at boot, MKDD's MMCR0 at
    // 0x800A9654) are stored.
    else if (in(spr::kMmcr0, spr::kPerfMonitorCount)) {
        const int index = number - spr::kMmcr0;
        const std::uint32_t counting = index == 0 ? kMmcr0Counting : index == 4 ? kMmcr1Counting : 0;
        if ((value & counting) != 0) {
            throw GekkoError("mtspr " + std::string(index == 0 ? "MMCR0" : "MMCR1") + " = " + hex32(value) +
                                 " selects performance-monitor events or interrupts (counting is not modeled)",
                             s_.pc, value);
        }
        s_.perf_monitor[static_cast<std::size_t>(index)] = value;
    }
    else return false;  // unknown, or read-only (PVR, TBL/TBU's read numbers, UMMCR0..UPMC4)
    return true;
}

// The locked-cache DMA (750CL manual, "Locked cache DMA"; Dolphin's mtspr DMAL): a write of
// DMAL with T set moves whole 32-byte lines at once (no transfer time is modeled, as in
// Dolphin). DMAU: MEM_ADDR (bits 5-31, a physical address, as the SDK's LCLoadBlocks masks it)
// | LEN_U (bits 0-4). DMAL: LC_ADDR (bits 5-31, in the locked cache at 0xE0000000) | LD
// (bit 4: 1 = memory to cache) | LEN_L (bits 2-3) | T (bit 1: trigger) | F (bit 0: flush).
// Lines = LEN_U << 2 | LEN_L, 0 meaning 128. T reads back as 0 afterwards (Dolphin clears
// only T; F, which would cancel queued DMAs, has nothing to cancel here and is stored).
//
// The cache side must lie inside the 16 KiB cache (0xE0000000-0xE0003FFF), checked before
// anything moves: BusError otherwise (Dolphin masks the index instead; stopping says more).
// Both sides go through the bus, so a memory address with nothing behind it, or HID2[LCE]
// clear (the bus then has no cache), throws the same BusError as any unmapped access. The
// whole source is read before the first write, so a failing DMA moves nothing.
void Gekko::locked_cache_dma() {
    if ((s_.dmal & 2) == 0) return;
    constexpr std::uint32_t kCacheBase = 0xE0000000, kCacheSize = 0x4000;
    const std::uint32_t memory = s_.dmau & ~0x1Fu, cache = s_.dmal & ~0x1Fu;
    std::uint32_t lines = ((s_.dmau & 0x1F) << 2) | ((s_.dmal >> 2) & 3);
    if (lines == 0) lines = 128;
    const std::uint32_t bytes = lines * 32;
    if (cache < kCacheBase || cache - kCacheBase > kCacheSize - bytes)
        throw BusError("locked-cache DMA of " + std::to_string(bytes) + " bytes at " + hex32(cache) +
                           " runs outside the locked cache",
                       cache);
    const bool to_cache = (s_.dmal & 0x10) != 0;
    const std::uint32_t from = to_cache ? memory : cache, to = to_cache ? cache : memory;
    std::array<std::uint64_t, 128 * 32 / 8> buffer{};
    for (std::uint32_t i = 0; i < bytes / 8; ++i) buffer[i] = bus_.read64(from + i * 8);
    for (std::uint32_t i = 0; i < bytes / 8; ++i) bus_.write64(to + i * 8, buffer[i]);
    s_.dmal &= ~2u;
}

std::uint32_t Gekko::spr(int number) const {
    std::uint32_t value = 0;
    if (!try_read_spr(number, value))
        throw GekkoError("unknown SPR " + std::to_string(number), s_.pc, static_cast<std::uint32_t>(number));
    return value;
}

void Gekko::set_spr(int number, std::uint32_t value) {
    if (!try_write_spr(number, value))
        throw GekkoError("unknown or read-only SPR " + std::to_string(number), s_.pc,
                         static_cast<std::uint32_t>(number));
}

}  // namespace ezy::gc
