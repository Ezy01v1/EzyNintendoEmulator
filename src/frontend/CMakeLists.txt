add_executable(ezyemu main.cpp debugger.cpp chip8_system.cpp gameboy_system.cpp)
target_link_libraries(ezyemu PRIVATE ezy_chip8 ezy_gb SDL3::SDL3 imgui ezy_warnings)
