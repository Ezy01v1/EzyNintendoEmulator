add_executable(ezy_n64_tests test_main.cpp test_scheduler.cpp test_cartridge.cpp test_cpu.cpp test_cop0.cpp test_tlb.cpp test_cop1.cpp test_bus.cpp)
target_link_libraries(ezy_n64_tests PRIVATE ezy_n64 doctest::doctest ezy_warnings)
target_compile_definitions(ezy_n64_tests PRIVATE DOCTEST_CONFIG_USE_STD_HEADERS)
add_test(NAME ezy_n64_tests COMMAND ezy_n64_tests)
