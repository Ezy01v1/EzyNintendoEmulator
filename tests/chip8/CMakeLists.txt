add_executable(ezy_chip8_tests test_main.cpp test_chip8.cpp test_disassembler.cpp)
target_link_libraries(ezy_chip8_tests PRIVATE ezy_chip8 doctest::doctest ezy_warnings)
add_test(NAME ezy_chip8_tests COMMAND ezy_chip8_tests)
