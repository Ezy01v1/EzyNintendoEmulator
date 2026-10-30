add_executable(ezyemu main.cpp debugger.cpp)
target_link_libraries(ezyemu PRIVATE ezy_chip8 SDL3::SDL3 imgui ezy_warnings)
