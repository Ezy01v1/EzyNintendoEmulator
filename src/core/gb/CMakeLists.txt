add_library(ezy_gb STATIC cartridge.cpp bus.cpp cpu.cpp timer.cpp)
target_include_directories(ezy_gb PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(ezy_gb PRIVATE ezy_warnings)
