# Host detection and the shared AsmJit dependency (RSP JIT and CPU JIT).
option(EZY_N64_RSP_JIT "Build the optional x86-64 RSP compiler" ON)
option(EZY_N64_CPU_JIT "Build the optional x86-64 VR4300 (CPU) compiler" ON)

set(EZY_HOST_X64 OFF)
if(CMAKE_SIZEOF_VOID_P EQUAL 8)
    if(MSVC AND CMAKE_CXX_COMPILER_ARCHITECTURE_ID STREQUAL "x64")
        set(EZY_HOST_X64 ON)
    elseif(NOT MSVC AND CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|amd64|AMD64)$")
        set(EZY_HOST_X64 ON)
    endif()
endif()

if((EZY_N64_RSP_JIT OR EZY_N64_CPU_JIT) AND EZY_HOST_X64)
    set(ASMJIT_STATIC ON CACHE BOOL "Build AsmJit statically" FORCE)
    set(ASMJIT_TEST OFF CACHE BOOL "Do not build dependency tests" FORCE)
    FetchContent_Declare(asmjit
        GIT_REPOSITORY https://github.com/asmjit/asmjit.git
        GIT_TAG 677a10bac2c24f7e068cf1f9e675bf51c7b534ea)
    FetchContent_MakeAvailable(asmjit)
endif()
