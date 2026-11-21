# ezy_jit always builds. Its public headers never include AsmJit; the AsmJit-dependent code
# (code_buffer_impl.hpp, x64_backend.cpp, x64_assembler.cpp) compiles only when
# EZY_JIT_AVAILABLE, otherwise CodeBuffer::available() is false and compile() reports
# Unsupported. reg_alloc.cpp never needs AsmJit.
add_library(ezy_jit STATIC jit.cpp ir.cpp block_cache.cpp code_buffer.cpp reg_alloc.cpp x64_backend.cpp
    x64_assembler.cpp)
target_include_directories(ezy_jit PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(ezy_jit PRIVATE ezy_warnings)
# MSVC 2022 hits an internal compiler error (C1001/LNK1000) in x64_backend.cpp under LTCG.
set_target_properties(ezy_jit PROPERTIES INTERPROCEDURAL_OPTIMIZATION_RELEASE OFF)
if(EZY_N64_CPU_JIT AND EZY_HOST_X64)
    target_compile_definitions(ezy_jit PUBLIC EZY_JIT_AVAILABLE=1)
    target_link_libraries(ezy_jit PRIVATE asmjit::asmjit)
else()
    target_compile_definitions(ezy_jit PUBLIC EZY_JIT_AVAILABLE=0)
endif()
