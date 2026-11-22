add_executable(ezy_gc_tests test_main.cpp test_gekko_integer.cpp test_gekko_system.cpp test_gekko_fpu.cpp
                            test_gekko_paired.cpp)
target_link_libraries(ezy_gc_tests PRIVATE ezy_gc doctest::doctest ezy_warnings)
# Lets doctest print std::string_view on MSVC (it otherwise forward-declares std::ostream).
target_compile_definitions(ezy_gc_tests PRIVATE DOCTEST_CONFIG_USE_STD_HEADERS)
add_test(NAME ezy_gc_tests COMMAND ezy_gc_tests)
set_tests_properties(ezy_gc_tests PROPERTIES TIMEOUT 120)
