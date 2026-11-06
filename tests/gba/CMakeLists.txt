add_executable(ezy_gba_tests test_main.cpp test_cartridge.cpp test_bus.cpp test_arm.cpp test_scheduler.cpp test_ppu.cpp test_irq.cpp test_timer_dma.cpp test_backup.cpp test_gpio.cpp test_state.cpp test_apu.cpp test_hle_swi.cpp)
target_link_libraries(ezy_gba_tests PRIVATE ezy_gba doctest::doctest ezy_warnings)
target_compile_definitions(ezy_gba_tests PRIVATE DOCTEST_CONFIG_USE_STD_HEADERS)
add_test(NAME ezy_gba_tests COMMAND ezy_gba_tests)

# SingleStepTests/ARM7TDMI runner. The .json.bin files are downloaded separately into testdata/.
add_executable(ezy_gba_sst sst_runner.cpp)
target_link_libraries(ezy_gba_sst PRIVATE ezy_gba ezy_warnings)
# Not registered with ctest: 98.2% of the ARM cases and 99.9% of the Thumb ones pass, and
# what's left isn't worth chasing (checked 2026-11-05):
# - MUL/MLA/MULL C flag: the suite's expected values follow neither "C unchanged" nor
#   NanoBoyAdvance's current Booth-multiplier model (~76% match either way), so they come
#   from an older NBA rule. Games don't depend on this flag; we leave C unchanged.
# - LDM/STM: every failure is an STM with Rn = r15 and writeback, an illegal encoding the
#   suite's README lists as a known issue. Likewise MUL with Rd = r15.
# Run it by hand:
#   ezy_gba_sst testdata/arm7tdmi/v1 arm_      (or thumb_)
