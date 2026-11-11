#include "n64/hle_boot.hpp"

#include <algorithm>

#include "n64/bus.hpp"
#include "n64/cartridge.hpp"
#include "n64/cpu/vr4300.hpp"

namespace ezy::n64 {

void hle_boot(Vr4300& cpu, Bus& bus, const Cartridge& cart) {
    const auto rom = cart.rom();
    bus.rsp().write_memory(0x40, rom.subspan(0x40, 0xFC0));

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

    // PI domain 1 timing, which the PIF takes from the first ROM word (80 37 12 40 on a
    // normal cartridge): LAT = byte 3, PWD = byte 2, PGS = byte 1 & 0xF, RLS = byte 1 >> 4.
    bus.write32(0x04600014, rom[3]);
    bus.write32(0x04600018, rom[2]);
    bus.write32(0x0460001C, rom[1] & 0x0F);
    bus.write32(0x04600020, rom[1] >> 4);

    // RDRAM interface as the IPL leaves it after RDRAM is set up. RI_SELECT != 0 tells
    // the IPL3 that RDRAM is already initialised (n64brew "RDRAM Interface").
    bus.write32(0x04700000, 0x0E);        // RI_MODE
    bus.write32(0x04700004, 0x40);        // RI_CONFIG
    bus.write32(0x0470000C, 0x14);        // RI_SELECT
    bus.write32(0x04700010, 0x00063634);  // RI_REFRESH
    // The IPL3 stores the RDRAM size (osMemSize) only when it initialises RDRAM itself,
    // which RI_SELECT just told it to skip: store it here (n64-systemtest's heap reads
    // it; 0 there ends in a null pointer). CIC 6105 games look for it at 0x3F0.
    bus.write32(0x318, Bus::kRdramSize);
    bus.write32(0x3F0, Bus::kRdramSize);

    cpu.set_pc(0xFFFFFFFFA4000040ULL);
}

}  // namespace ezy::n64
