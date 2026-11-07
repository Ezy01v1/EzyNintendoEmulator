#include "n64/pi.hpp"

namespace ezy::n64 {

namespace {

constexpr std::uint32_t kStatusDmaBusy = 1, kStatusIoBusy = 2, kStatusError = 4, kStatusInterrupt = 8;

// Domain 1 address 2 is the cartridge ROM; domain 2 (0x05000000-0x0FFFFFFF) is SRAM,
// FlashRAM and the 64DD, not emulated in this subproject (reads as 0).
constexpr std::uint32_t kRomBase = 0x10000000;

bool is_domain2(std::uint32_t cart) { return cart >= 0x05000000 && cart < 0x10000000 && !(cart >= 0x06000000 && cart < 0x08000000); }

}  // namespace

std::uint32_t Pi::read(std::uint32_t offset) const {
    switch (offset & 0x3F) {
        case 0x00: return dram_addr_;
        case 0x04: return cart_addr_;
        case 0x08: return rd_len_;
        case 0x0C: return wr_len_;
        case 0x10:
            return (busy_ ? kStatusDmaBusy : 0) | (error_ ? kStatusError : 0) | (interrupt_ ? kStatusInterrupt : 0);
        default: {
            const std::uint32_t i = ((offset & 0x3F) - 0x14) / 4;
            return i < domain_.size() ? domain_[i] : 0;
        }
    }
}

std::uint64_t Pi::dma_cycles(std::uint32_t cart_addr, std::uint32_t length) const {
    const std::size_t d = is_domain2(cart_addr) ? 4 : 0;
    const std::uint64_t lat = domain_[d] & 0xFF, pwd = domain_[d + 1] & 0xFF, pgs = domain_[d + 2] & 0xF,
                        rls = domain_[d + 3] & 0x3;
    const std::uint64_t page = std::uint64_t{1} << (pgs + 2);
    const std::uint64_t pages = (length + page - 1) / page;
    const std::uint64_t halfwords = (length + 1) / 2;
    // Each page pays the latency, each 16-bit transfer the pulse width and release. These
    // are RCP cycles (62.5 MHz); the CPU runs at 1.5 times that.
    const std::uint64_t rcp_cycles = pages * (lat + 1) + halfwords * (pwd + 1 + rls + 1);
    return rcp_cycles * 3 / 2;
}

void Pi::write(std::uint32_t offset, std::uint32_t value, Mi& mi, Scheduler& scheduler, std::span<std::uint8_t> rdram,
               const Cartridge& cart) {
    switch (offset & 0x3F) {
        case 0x00: dram_addr_ = value & 0x00FFFFFE; return;
        case 0x04: cart_addr_ = value & 0xFFFFFFFE; return;
        case 0x10:
            if (value & 1) {  // reset the controller: stops a DMA, clears the error
                busy_ = false;
                error_ = false;
                scheduler.cancel(EventType::PiDma);
            }
            if (value & 2) {
                interrupt_ = false;
                mi.clear(MiInterrupt::Pi);
            }
            return;
        case 0x08:    // PI_RD_LEN: RDRAM -> cartridge (only SRAM/Flash would take it)
        case 0x0C: {  // PI_WR_LEN: cartridge -> RDRAM
            const bool to_rdram = (offset & 0x3F) == 0x0C;
            if (to_rdram) wr_len_ = value & 0x00FFFFFF;
            else rd_len_ = value & 0x00FFFFFF;
            const std::uint32_t length = (value & 0x00FFFFFF) + 1;
            // RDRAM side: the transfer starts at the 8-byte boundary below PI_DRAM_ADDR.
            const std::uint32_t dram = dram_addr_ & ~7U;
            if (to_rdram) {
                for (std::uint32_t i = 0; i < length; ++i) {
                    const std::uint32_t c = cart_addr_ + i;
                    std::uint8_t byte = 0;
                    if (c >= kRomBase && c - kRomBase < cart.rom().size()) byte = cart.rom()[c - kRomBase];
                    const std::uint32_t d = dram + i;
                    if (d < rdram.size()) rdram[d] = byte;
                }
            }
            busy_ = true;
            scheduler.schedule_in(EventType::PiDma, dma_cycles(cart_addr_, length));
            // Afterwards the address registers point past the transfer, 8- and 2-aligned.
            dram_addr_ = (dram + length + 7) & 0x00FFFFF8;
            cart_addr_ = (cart_addr_ + length + 1) & 0xFFFFFFFE;
            return;
        }
        default: {
            const std::uint32_t i = ((offset & 0x3F) - 0x14) / 4;
            if (i < domain_.size()) domain_[i] = value & 0xFF;
            return;
        }
    }
}

void Pi::finish_dma(Mi& mi) {
    busy_ = false;
    interrupt_ = true;
    mi.raise(MiInterrupt::Pi);
}

}  // namespace ezy::n64
