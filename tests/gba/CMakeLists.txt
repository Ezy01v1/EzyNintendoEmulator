add_executable(ezy_gba_tests test_main.cpp test_cartridge.cpp test_bus.cpp test_arm.cpp test_scheduler.cpp test_ppu.cpp test_irq.cpp test_timer_dma.cpp test_backup.cpp test_apu.cpp)
target_link_libraries(ezy_gba_tests PRIVATE ezy_gba doctest::doctest ezy_warnings)
target_compile_definitions(ezy_gba_tests PRIVATE DOCTEST_CONFIG_USE_STD_HEADERS)
add_test(NAME ezy_gba_tests COMMAND ezy_gba_tests)

# SingleStepTests/ARM7TDMI runner. The .json.bin files are downloaded separately into testdata/.
add_executable(ezy_gba_sst sst_runner.cpp)
target_link_libraries(ezy_gba_sst PRIVATE ezy_gba ezy_warnings)
# Not registered with ctest yet: MUL/MULL carry and a few LDM/STM r15 edge cases still
# differ (see the commit message). Run it by hand:
#   ezy_gba_sst testdata/arm7tdmi/v1 arm_
