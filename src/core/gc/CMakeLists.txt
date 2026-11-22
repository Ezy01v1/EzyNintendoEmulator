add_library(ezy_gc STATIC gekko/gekko.cpp gekko/gekko_integer.cpp gekko/gekko_fpu.cpp gekko/gekko_paired.cpp
            disc.cpp dol.cpp hle_boot.cpp memory.cpp scheduler.cpp)
target_include_directories(ezy_gc PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(ezy_gc PRIVATE ezy_warnings)
