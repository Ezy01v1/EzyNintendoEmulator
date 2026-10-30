add_library(ezy_chip8 STATIC chip8.cpp disassembler.cpp)
target_include_directories(ezy_chip8 PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(ezy_chip8 PRIVATE ezy_warnings)
