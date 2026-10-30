add_executable(ezyemu main.cpp)
target_link_libraries(ezyemu PRIVATE ezy_chip8 SDL3::SDL3 ezy_warnings)
