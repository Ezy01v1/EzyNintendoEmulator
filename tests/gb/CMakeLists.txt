add_executable(ezy_gb_tests test_main.cpp test_cartridge.cpp test_bus.cpp)
target_link_libraries(ezy_gb_tests PRIVATE ezy_gb doctest::doctest ezy_warnings)
# Lets doctest print std::string_view on MSVC (it otherwise forward-declares std::ostream).
target_compile_definitions(ezy_gb_tests PRIVATE DOCTEST_CONFIG_USE_STD_HEADERS)
add_test(NAME ezy_gb_tests COMMAND ezy_gb_tests)
