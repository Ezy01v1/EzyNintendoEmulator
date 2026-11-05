add_library(ezy_gba STATIC cartridge.cpp backup.cpp apu.cpp bus.cpp cpu/arm7tdmi.cpp cpu/arm.cpp cpu/thumb.cpp scheduler.cpp ppu.cpp timer.cpp gba.cpp hle_swi.cpp)
target_include_directories(ezy_gba PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(ezy_gba PRIVATE ezy_warnings)
