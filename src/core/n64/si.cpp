#include "n64/si.hpp"

namespace ezy::n64 {

std::uint32_t Si::read(std::uint32_t offset) const {
    switch (offset & 0x1F) {
        case 0x00: return dram_addr_;
        case 0x18: return (busy_ ? 1U : 0U) | (interrupt_ ? 1U << 12 : 0U);  // SI_STATUS
        default: return 0;
    }
}

void Si::write(std::uint32_t offset, std::uint32_t value, Mi& mi, Scheduler& scheduler, std::span<std::uint8_t> rdram,
               Pif& pif) {
    switch (offset & 0x1F) {
        case 0x00: dram_addr_ = value & 0x00FFFFF8; return;
        case 0x04:  // SI_PIF_AD_RD64B: PIF RAM -> RDRAM
            for (std::size_t i = 0; i < 64; ++i) {
                if (dram_addr_ + i < rdram.size()) rdram[dram_addr_ + i] = pif.ram()[i];
            }
            break;
        case 0x10:  // SI_PIF_AD_WR64B: RDRAM -> PIF RAM
            for (std::size_t i = 0; i < 64; ++i) {
                if (dram_addr_ + i < rdram.size()) pif.ram()[i] = rdram[dram_addr_ + i];
            }
            pif.after_write();
            break;
        case 0x18:  // SI_STATUS: any write clears the interrupt
            interrupt_ = false;
            mi.clear(MiInterrupt::Si);
            return;
        default: return;
    }
    busy_ = true;
    scheduler.schedule_in(EventType::SiDma, kDmaCycles);
}

void Si::finish_dma(Mi& mi) {
    busy_ = false;
    interrupt_ = true;
    mi.raise(MiInterrupt::Si);
}

}  // namespace ezy::n64
