#include "n64/hle_boot.hpp"

#include <algorithm>

#include "n64/bus.hpp"
#include "n64/cartridge.hpp"
#include "n64/cpu/vr4300.hpp"

namespace ezy::n64 {

void hle_boot(Vr4300& cpu, Bus& bus, const Cartridge& cart) {
    const auto rom = cart.rom();
    std::copy(rom.begin() + 0x40, rom.begin() + 0x1000, bus.rsp().mem() + 0x40);

    // GPRs as IPL2 leaves them for the IPL3.
    constexpr int s3 = 19, s4 = 20, s5 = 21, s6 = 22, s7 = 23, sp = 29;
    cpu.set_gpr(s3, 0);  // boot device: cartridge
    switch (cart.region()) {
        case Region::Pal: cpu.set_gpr(s4, 0); break;
        case Region::Mpal: cpu.set_gpr(s4, 2); break;
        default: cpu.set_gpr(s4, 1); break;
    }
    cpu.set_gpr(s5, 0);  // cold reset
    cpu.set_gpr(s6, cic_seed(cart.cic()));
    cpu.set_gpr(s7, 0);  // console version
    cpu.set_gpr(sp, 0xFFFFFFFFA4001FF0ULL);

    Cop0& cop0 = cpu.cop0();
    cop0.raw(Cop0::kStatus) = 0x34000000;  // CU1, CU0, FR; kernel mode, BEV and ERL clear
    cop0.raw(Cop0::kConfig) = 0x7006E463;
    cop0.raw(Cop0::kRandom) = 31;
    // What the PIF boot leaves in Cause: BD, CE = 3, ExcCode = 31 (PeterLemon COP0Cause).
    cop0.raw(Cop0::kCause) = 0xB000007C;
    cop0.raw(Cop0::kLLAddr) = 0xFFFFFFFF;  // PeterLemon LL_LLD_SC_SCD

    // RDRAM interface as the IPL leaves it after RDRAM is set up. RI_SELECT != 0 tells
    // the IPL3 that RDRAM is already initialised (n64brew "RDRAM Interface").
    bus.write32(0x04700000, 0x0E);        // RI_MODE
    bus.write32(0x04700004, 0x40);        // RI_CONFIG
    bus.write32(0x0470000C, 0x14);        // RI_SELECT
    bus.write32(0x04700010, 0x00063634);  // RI_REFRESH

    cpu.set_pc(0xFFFFFFFFA4000040ULL);
}

}  // namespace ezy::n64
