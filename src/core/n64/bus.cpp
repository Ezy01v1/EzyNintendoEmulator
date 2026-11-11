#include "n64/bus.hpp"

namespace ezy::n64 {

namespace {

constexpr std::uint32_t kSpMem = 0x04000000, kSpRegs = 0x04040000, kDp = 0x04100000, kMi = 0x04300000,
                        kVi = 0x04400000, kAi = 0x04500000, kPi = 0x04600000, kRi = 0x04700000, kSi = 0x04800000,
                        kCartDom1 = 0x10000000, kIsViewer = 0x13FF0000, kPifRom = 0x1FC00000,
                        kPifRam = 0x1FC007C0;

// Extra CPU cycles for one uncached access (approximations: the test ROMs in this
// subproject don't measure them; the TIMINGNTSC ROMs may refine them later).
constexpr std::uint32_t kRdramUncachedCycles = 30;  // a single RDRAM transaction
constexpr std::uint32_t kRcpRegisterCycles = 20;    // RCP register / SP memory
constexpr std::uint32_t kCartCycles = 100;          // a PI bus read of the cartridge
constexpr std::uint32_t kPifCycles = 300;           // the PIF's serial bus is slow

constexpr std::uint32_t kIsViewerMagic = 0x49533634;  // "IS64"

}  // namespace

Bus::Bus(Scheduler& scheduler, const Cartridge& cart, RdpMode mode) : scheduler_(scheduler), cart_(cart) {
    vi_.set_region(cart.region());
    ai_.set_region(cart.region());
    vi_.start(scheduler_);
    set_direct_ram(rdram_.data(), kRdramSize);
    if (mode == RdpMode::Threaded) set_memory_guard(rdp_.enable_threaded(rdram_));
}

std::uint8_t* Bus::memory(std::uint32_t paddr) {
    if (paddr < kRdramSize) return &rdram_[paddr];
    if (paddr >= kIsViewer + 0x20 && paddr < kIsViewer + 0x10000) return &isviewer_[paddr - kIsViewer];
    return nullptr;
}

std::uint32_t Bus::read_io(std::uint32_t paddr) {
    paddr &= ~3U;
    if (paddr >= 0x03F00000 && paddr < 0x04000000) return rdram_regs_[(paddr >> 2) & 15];
    if (paddr >= kSpRegs && paddr < kDp) return rsp_.read_sp(paddr - kSpRegs);
    if (paddr >= kDp && paddr < kDp + 0x100000) return rdp_.read(paddr - kDp, scheduler_.now());
    if (paddr >= kMi && paddr < kVi) return mi_.read(paddr - kMi);
    if (paddr >= kVi && paddr < kAi) return vi_.read(paddr - kVi);
    if (paddr >= kAi && paddr < kPi) return ai_.read(paddr - kAi, scheduler_.now());
    if (paddr >= kPi && paddr < kRi) return pi_.read(paddr - kPi);
    if (paddr >= kRi && paddr < kSi) return ri_[(paddr >> 2) & 7];
    if (paddr == kIsViewer) return kIsViewerMagic;
    if (paddr >= kSi && paddr < kSi + 0x100000) return si_.read(paddr - kSi);
    if (paddr >= kCartDom1 && paddr < kPifRom) return cart_.read32(paddr - kCartDom1);
    if (paddr >= kPifRam && paddr < kPifRam + 64) {
        const auto ram = pif_.ram();
        const std::uint32_t o = paddr - kPifRam;
        return (static_cast<std::uint32_t>(ram[o]) << 24) | (ram[o + 1] << 16) | (ram[o + 2] << 8) | ram[o + 3];
    }
    if (paddr >= kPifRom && paddr < kPifRam) {
        const std::uint32_t o = paddr - kPifRom;
        if (o + 4 > pif_rom_.size()) return 0;
        return (static_cast<std::uint32_t>(pif_rom_[o]) << 24) | (pif_rom_[o + 1] << 16) | (pif_rom_[o + 2] << 8) |
               pif_rom_[o + 3];
    }
    return 0;
}

void Bus::write_io(std::uint32_t paddr, std::uint32_t value) {
    paddr &= ~3U;
    if (paddr >= 0x03F00000 && paddr < 0x04000000) {
        rdram_regs_[(paddr >> 2) & 15] = value;
    } else if (paddr >= kSpRegs && paddr < kDp) {
        rsp_.write_sp(paddr - kSpRegs, value, mi_, scheduler_, scheduler_.now());
    } else if (paddr >= kDp && paddr < kDp + 0x100000) {
        rdp_.write(paddr - kDp, value, scheduler_, scheduler_.now());
    } else if (paddr >= kMi && paddr < kVi) {
        mi_.write(paddr - kMi, value);
    } else if (paddr >= kVi && paddr < kAi) {
        vi_.write(paddr - kVi, value, mi_);
    } else if (paddr >= kAi && paddr < kPi) {
        if (((paddr - kAi) & 0x1F) == 4) drain_rdp();
        ai_.write(paddr - kAi, value, mi_, scheduler_, rdram_);
    } else if (paddr >= kPi && paddr < kRi) {
        if (((paddr - kPi) & 0x3F) == 8 || ((paddr - kPi) & 0x3F) == 12) drain_rdp();
        pi_.write(paddr - kPi, value, mi_, scheduler_, rdram_, cart_);
    } else if (paddr >= kRi && paddr < kSi) {
        ri_[(paddr >> 2) & 7] = value;
    } else if (paddr >= kSi && paddr < kSi + 0x100000) {
        if (((paddr - kSi) & 0x1F) == 4 || ((paddr - kSi) & 0x1F) == 16) drain_rdp();
        si_.write(paddr - kSi, value, mi_, scheduler_, rdram_, pif_);
    } else if (paddr >= kPifRam && paddr < kPifRam + 64) {
        const std::uint32_t o = paddr - kPifRam;
        auto ram = pif_.ram();
        ram[o] = static_cast<std::uint8_t>(value >> 24);
        ram[o + 1] = static_cast<std::uint8_t>(value >> 16);
        ram[o + 2] = static_cast<std::uint8_t>(value >> 8);
        ram[o + 3] = static_cast<std::uint8_t>(value);
        if (o == 60) pif_.after_write();
    } else if (paddr == kIsViewer + 0x14) {  // ISViewer: print that many bytes of the buffer
        const std::uint32_t length = value < 0x10000 - 0x20 ? value : 0x10000 - 0x20;
        isviewer_out_.append(reinterpret_cast<const char*>(&isviewer_[0x20]), length);
    }
}

// 8- and 16-bit CPU reads of the cartridge bus see the upper half of the 32-bit word at
// (address + 2) & ~3 (PeterLemon DMAAlignment-PI-ROM-FROM).
std::uint16_t Bus::cart_read16(std::uint32_t paddr) { return static_cast<std::uint16_t>(read_io((paddr + 2) & ~3U) >> 16); }

bool Bus::is_cart(std::uint32_t paddr) { return paddr >= 0x05000000 && paddr < kPifRom; }

std::uint8_t Bus::read8(std::uint32_t paddr) {
    guard_memory(paddr, 1, false);
    if (paddr >= kSpMem && paddr < kSpRegs) return rsp_.read_memory8(paddr);
    if (std::uint8_t* m = memory(paddr)) return *m;
    if (is_cart(paddr)) {
        const std::uint16_t half = cart_read16(paddr);
        return static_cast<std::uint8_t>((paddr & 1) ? half : half >> 8);
    }
    return static_cast<std::uint8_t>(read_io(paddr) >> ((3 - (paddr & 3)) * 8));
}

std::uint16_t Bus::read16(std::uint32_t paddr) {
    if (paddr >= kSpMem && paddr < kSpRegs) return static_cast<std::uint16_t>((read8(paddr) << 8) | read8(paddr + 1));
    if (memory(paddr)) return static_cast<std::uint16_t>((read8(paddr) << 8) | read8(paddr + 1));
    if (is_cart(paddr)) return cart_read16(paddr);
    return static_cast<std::uint16_t>(read_io(paddr) >> ((2 - (paddr & 2)) * 8));
}

std::uint32_t Bus::read32(std::uint32_t paddr) {
    guard_memory(paddr, 4, false);
    if (paddr >= kSpMem && paddr < kSpRegs) {
        std::uint32_t value = 0;
        for (unsigned i = 0; i < 4; ++i) value = (value << 8) | rsp_.read_memory8(paddr + i);
        return value;
    }
    if (std::uint8_t* m = memory(paddr)) {
        return (static_cast<std::uint32_t>(m[0]) << 24) | (m[1] << 16) | (m[2] << 8) | m[3];
    }
    return read_io(paddr);
}

std::uint64_t Bus::read64(std::uint32_t paddr) {
    return (static_cast<std::uint64_t>(read32(paddr)) << 32) | read32(paddr + 4);
}

void Bus::write8(std::uint32_t paddr, std::uint8_t value) {
    guard_memory(paddr, 1, true);
    if (paddr >= kSpMem && paddr < kSpRegs) { rsp_.write_memory(paddr, std::span(&value, 1)); return; }
    if (std::uint8_t* m = memory(paddr)) {
        *m = value;
        return;
    }
    write_io(paddr, static_cast<std::uint32_t>(value) << ((3 - (paddr & 3)) * 8));
}

void Bus::write16(std::uint32_t paddr, std::uint16_t value) {
    if (paddr >= kSpMem && paddr < kSpRegs) {
        const std::array bytes{static_cast<std::uint8_t>(value >> 8), static_cast<std::uint8_t>(value)};
        rsp_.write_memory(paddr, bytes);
        return;
    }
    if (memory(paddr)) {
        write8(paddr, static_cast<std::uint8_t>(value >> 8));
        write8(paddr + 1, static_cast<std::uint8_t>(value));
        return;
    }
    write_io(paddr, static_cast<std::uint32_t>(value) << ((2 - (paddr & 2)) * 8));
}

void Bus::write32(std::uint32_t paddr, std::uint32_t value) {
    guard_memory(paddr, 4, true);
    if (paddr >= kSpMem && paddr < kSpRegs) {
        const std::array bytes{static_cast<std::uint8_t>(value >> 24), static_cast<std::uint8_t>(value >> 16),
                               static_cast<std::uint8_t>(value >> 8), static_cast<std::uint8_t>(value)};
        rsp_.write_memory(paddr, bytes);
        return;
    }
    if (std::uint8_t* m = memory(paddr)) {
        m[0] = static_cast<std::uint8_t>(value >> 24);
        m[1] = static_cast<std::uint8_t>(value >> 16);
        m[2] = static_cast<std::uint8_t>(value >> 8);
        m[3] = static_cast<std::uint8_t>(value);
        return;
    }
    write_io(paddr, value);
}

void Bus::write64(std::uint32_t paddr, std::uint64_t value) {
    write32(paddr, static_cast<std::uint32_t>(value >> 32));
    write32(paddr + 4, static_cast<std::uint32_t>(value));
}

std::uint32_t Bus::uncached_cycles(std::uint32_t paddr) const {
    if (paddr < kRdramSize) return kRdramUncachedCycles;
    if (paddr >= kPifRom) return kPifCycles;
    if (paddr >= kCartDom1) return kCartCycles;
    return kRcpRegisterCycles;
}

void Bus::handle_event(EventType type, std::uint64_t when) {
    switch (type) {
        case EventType::PiDma: pi_.finish_dma(mi_); break;
        case EventType::ViLine: vi_.on_line(mi_, scheduler_, when); break;
        case EventType::SiDma: si_.finish_dma(mi_); break;
        case EventType::AiDma: drain_rdp(); ai_.on_dma_end(mi_, scheduler_, rdram_); break;
        case EventType::SpDma: drain_rdp(); rsp_.finish_dma_row(scheduler_, rdram_, when); break;
        case EventType::RspStep: rsp_.step(mi_, rdp_, scheduler_, when); break;
        case EventType::RdpWord: rdp_.step(mi_, scheduler_, rdram_, rsp_.dmem(), when); break;
        default: break;
    }
}

void Bus::set_pif_rom(std::span<const std::uint8_t> rom) { pif_rom_.assign(rom.begin(), rom.end()); }

std::string Bus::take_isviewer_output() {
    std::string out;
    out.swap(isviewer_out_);
    return out;
}

}  // namespace ezy::n64
