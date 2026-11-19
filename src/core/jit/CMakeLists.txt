# ezy_jit always builds. Its public headers never include AsmJit; AsmJit-dependent sources
# are added only when EZY_JIT_AVAILABLE.
add_library(ezy_jit STATIC jit.cpp)
target_include_directories(ezy_jit PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(ezy_jit PRIVATE ezy_warnings)
if(EZY_N64_CPU_JIT AND EZY_HOST_X64)
    target_compile_definitions(ezy_jit PUBLIC EZY_JIT_AVAILABLE=1)
    target_link_libraries(ezy_jit PRIVATE asmjit::asmjit)
else()
    target_compile_definitions(ezy_jit PUBLIC EZY_JIT_AVAILABLE=0)
endif()
