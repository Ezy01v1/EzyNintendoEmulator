add_executable(ezy_gb_tests test_main.cpp test_cartridge.cpp test_bus.cpp test_cpu.cpp test_timer.cpp test_ppu.cpp test_apu.cpp test_cgb.cpp test_hdma.cpp test_compat_palettes.cpp test_state.cpp test_color.cpp)
target_link_libraries(ezy_gb_tests PRIVATE ezy_gb doctest::doctest ezy_warnings)
# Lets doctest print std::string_view on MSVC (it otherwise forward-declares std::ostream).
target_compile_definitions(ezy_gb_tests PRIVATE DOCTEST_CONFIG_USE_STD_HEADERS)
add_test(NAME ezy_gb_tests COMMAND ezy_gb_tests)

# SingleStepTests/sm83 runner. The JSON data is downloaded separately into testdata/.
add_executable(ezy_gb_sst sst_runner.cpp)
target_link_libraries(ezy_gb_sst PRIVATE ezy_gb nlohmann_json::nlohmann_json ezy_warnings)

set(EZY_SST_DIR ${PROJECT_SOURCE_DIR}/testdata/sm83/v1)
if(EXISTS ${EZY_SST_DIR})
    # 500k cases: only practical with optimizations, so it runs with `ctest -C Release`.
    add_test(NAME gb_single_step_tests COMMAND ezy_gb_sst ${EZY_SST_DIR} CONFIGURATIONS Release)
endif()

# Test ROMs (see roms/README.md for where to get them). Each runs only if present, and
# only with `ctest -C Release` because they take a while unoptimised.
set(EZY_ROMS ${PROJECT_SOURCE_DIR}/roms)
function(ezy_gb_rom_test name rom)
    if(EXISTS ${EZY_ROMS}/${rom})
        add_test(NAME ${name} COMMAND ezy_gb_runner ${EZY_ROMS}/${rom} ${ARGN} CONFIGURATIONS Release)
    endif()
endfunction()

ezy_gb_rom_test(gb_blargg_cpu_instrs_cgb cpu_instrs.gb 120 --model cgb)
ezy_gb_rom_test(gb_blargg_cpu_instrs_dmg cpu_instrs.gb 120 --model dmg)
ezy_gb_rom_test(gb_blargg_instr_timing_cgb instr_timing.gb 30 --model cgb)
ezy_gb_rom_test(gb_blargg_instr_timing_dmg instr_timing.gb 30 --model dmg)
ezy_gb_rom_test(gb_mooneye_boot_regs_cgb mooneye/misc/boot_regs-cgb.gb --mooneye --model cgb)
ezy_gb_rom_test(gb_mooneye_boot_regs_dmg mooneye/acceptance/boot_regs-dmgABC.gb --mooneye --model dmg)
ezy_gb_rom_test(gb_mooneye_vblank_stat_intr_cgb mooneye/misc/ppu/vblank_stat_intr-C.gb --mooneye --model cgb)
if(EXISTS ${EZY_ROMS}/cgb-acid2-reference.bmp)
    ezy_gb_rom_test(gb_cgb_acid2 cgb-acid2.gbc --compare ${EZY_ROMS}/cgb-acid2-reference.bmp)
endif()
if(EXISTS ${EZY_ROMS}/dmg-acid2-reference-dmg.bmp)
    ezy_gb_rom_test(gb_dmg_acid2_dmg dmg-acid2.gb --compare ${EZY_ROMS}/dmg-acid2-reference-dmg.bmp --model dmg)
endif()
if(EXISTS ${EZY_ROMS}/dmg-acid2-reference-cgb.bmp)
    ezy_gb_rom_test(gb_dmg_acid2_cgb dmg-acid2.gb --compare ${EZY_ROMS}/dmg-acid2-reference-cgb.bmp --model cgb)
endif()

# Mooneye timing ROMs from the accuracy pass. They were verified on DMG hardware, so they
# run with --model dmg (see docs/superpowers/specs/2026-11-06-gb-accuracy-design.md).
set(EZY_MOONEYE_DMG_TIMING
    ppu/intr_1_2_timing-GS ppu/intr_2_0_timing ppu/intr_2_mode0_timing ppu/intr_2_mode0_timing_sprites
    ppu/intr_2_mode3_timing ppu/intr_2_oam_ok_timing ppu/stat_irq_blocking ppu/stat_lyc_onoff
    ppu/vblank_stat_intr-GS ppu/hblank_ly_scx_timing-GS ppu/lcdon_timing-GS ppu/lcdon_write_timing-GS
    oam_dma/basic oam_dma/reg_read oam_dma/sources-GS oam_dma_start oam_dma_restart oam_dma_timing
    timer/div_write timer/rapid_toggle timer/tim00 timer/tim00_div_trigger timer/tim01
    timer/tim01_div_trigger timer/tim10 timer/tim10_div_trigger timer/tim11 timer/tim11_div_trigger
    timer/tima_reload timer/tima_write_reloading timer/tma_write_reloading
    call_timing call_timing2 call_cc_timing call_cc_timing2 ret_timing ret_cc_timing reti_timing
    jp_timing jp_cc_timing rst_timing push_timing add_sp_e_timing ld_hl_sp_e_timing
    interrupts/ie_push di_timing-GS halt_ime0_nointr_timing halt_ime1_timing2-GS)
foreach(rom IN LISTS EZY_MOONEYE_DMG_TIMING)
    string(REPLACE "/" "_" name ${rom})
    ezy_gb_rom_test(gb_mooneye_${name} mooneye/acceptance/${rom}.gb --mooneye --model dmg)
endforeach()
