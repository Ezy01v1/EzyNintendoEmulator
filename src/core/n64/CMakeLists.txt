add_library(ezy_n64 STATIC scheduler.cpp cartridge.cpp)
target_include_directories(ezy_n64 PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(ezy_n64 PRIVATE ezy_warnings)
