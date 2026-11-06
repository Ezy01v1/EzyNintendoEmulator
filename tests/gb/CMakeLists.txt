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

set(EZY_BLARGG_ROM ${PROJECT_SOURCE_DIR}/roms/cpu_instrs.gb)
if(EXISTS ${EZY_BLARGG_ROM})
    add_test(NAME gb_blargg_cpu_instrs COMMAND ezy_gb_runner ${EZY_BLARGG_ROM} 120 CONFIGURATIONS Release)
endif()
