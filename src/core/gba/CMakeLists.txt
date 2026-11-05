add_library(ezy_gba STATIC cartridge.cpp bus.cpp cpu/arm7tdmi.cpp cpu/arm.cpp)
target_include_directories(ezy_gba PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(ezy_gba PRIVATE ezy_warnings)
