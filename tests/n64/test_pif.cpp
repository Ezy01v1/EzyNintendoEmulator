#include <doctest/doctest.h>

#include <cstdint>
#include <initializer_list>
#include <vector>

#include "n64/bus.hpp"
#include "n64/cartridge.hpp"
#include "n64/cpu/vr4300.hpp"
#include "n64/hle_boot.hpp"
#include "n64/pif.hpp"
#include "n64/scheduler.hpp"

using namespace ezy::n64;

namespace {

void put(Pif& pif, std::initializer_list<std::uint8_t> bytes) {
    std::size_t i = 0;
    for (const std::uint8_t b : bytes) pif.ram()[i++] = b;
}

Cartridge boot_cart(char region) {
    std::vector<std::uint8_t> rom(0x2000, 0);
    rom[0] = 0x80, rom[1] = 0x37, rom[2] = 0x12, rom[3] = 0x40;
    rom[0x3E] = static_cast<std::uint8_t>(region);
    for (std::size_t i = 0x40; i < 0x1000; ++i) rom[i] = static_cast<std::uint8_t>(i);
    return *Cartridge::load(rom);
}

}  // namespace

TEST_CASE("controller status reports a controller with no pak") {
    Pif pif;
    put(pif, {0x01, 0x03, 0x00, 0xFF, 0xFF, 0xFF, 0xFE});
    pif.run_commands();
    CHECK(pif.ram()[3] == 0x05);
    CHECK(pif.ram()[4] == 0x00);
    CHECK(pif.ram()[5] == 0x02);
}

TEST_CASE("controller read returns buttons and stick") {
    Pif pif;
    pif.set_buttons(0, 0x8000, 10, -5);  // A pressed
    put(pif, {0x01, 0x04, 0x01, 0xFF, 0xFF, 0xFF, 0xFF, 0xFE});
    pif.run_commands();
    CHECK(pif.ram()[3] == 0x80);
    CHECK(pif.ram()[4] == 0x00);
    CHECK(pif.ram()[5] == 10);
    CHECK(pif.ram()[6] == 0xFB);
}

TEST_CASE("empty port reports no device") {
    Pif pif;
    put(pif, {0x00, 0x01, 0x03, 0x00, 0xFF, 0xFF, 0xFF, 0xFE});  // skip port 0, ask port 1
    pif.run_commands();
    CHECK((pif.ram()[2] & 0x80) != 0);
}

TEST_CASE("SI write DMA runs the PIF commands and interrupts") {
    Scheduler scheduler;
    const Cartridge cart = boot_cart('E');
    Bus bus(scheduler, cart);
    bus.pif().set_buttons(0, 0x1000, 0, 0);  // Start
    const std::uint8_t block[] = {0x01, 0x04, 0x01, 0xFF, 0xFF, 0xFF, 0xFF, 0xFE};
    for (std::uint32_t i = 0; i < 64; ++i) bus.write8(0x1000 + i, i < sizeof block ? block[i] : 0);
    bus.write8(0x1000 + 63, 0x01);  // run the joybus commands
    bus.write32(0x04800000, 0x1000);       // SI_DRAM_ADDR
    bus.write32(0x04800010, 0x1FC007C0);   // SI_PIF_AD_WR64B
    CHECK((bus.read32(0x04800018) & 1) == 1);  // busy
    for (int n = 0; n < 1000 && !(bus.read32(0x04300008) & 2); ++n) {
        scheduler.advance_to(*scheduler.next_event_time());
        while (const auto due = scheduler.pop_due()) bus.handle_event(due->type, due->when);
    }
    CHECK((bus.read32(0x04300008) & 2) != 0);  // MI: SI interrupt
    bus.write32(0x04800018, 0);                // acknowledge
    CHECK((bus.read32(0x04300008) & 2) == 0);
    bus.write32(0x04800000, 0x2000);
    bus.write32(0x04800004, 0x1FC007C0);  // SI_PIF_AD_RD64B
    CHECK(bus.read8(0x2000 + 3) == 0x10);
}

TEST_CASE("HLE boot copies IPL3 to DMEM and starts at 0xA4000040") {
    Scheduler scheduler;
    const Cartridge cart = boot_cart('E');
    Bus bus(scheduler, cart);
    Vr4300 cpu(bus);
    hle_boot(cpu, bus, cart);
    CHECK(cpu.pc() == 0xFFFFFFFFA4000040ULL);
    CHECK(bus.read32(0x04000040) == 0x40414243);
    CHECK(bus.read8(0x04000FFF) == 0xFF);
    CHECK(cpu.gpr(29) == 0xFFFFFFFFA4001FF0ULL);
    CHECK(cpu.cop0().read(Cop0::kStatus) == 0x34000000);
}

TEST_CASE("HLE boot sets s4 from the region and s6 from the CIC") {
    Scheduler scheduler;
    const Cartridge ntsc = boot_cart('E');
    Bus bus(scheduler, ntsc);
    Vr4300 cpu(bus);
    hle_boot(cpu, bus, ntsc);
    CHECK(cpu.gpr(20) == 1);
    CHECK(cpu.gpr(22) == 0x3F);
    const Cartridge pal = boot_cart('P');
    Scheduler scheduler2;
    Bus bus2(scheduler2, pal);
    Vr4300 cpu2(bus2);
    hle_boot(cpu2, bus2, pal);
    CHECK(cpu2.gpr(20) == 0);
}
