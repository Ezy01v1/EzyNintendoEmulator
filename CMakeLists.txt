cmake_minimum_required(VERSION 3.25)
project(EzyEmulator LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(EZY_BUILD_FRONTEND "Build the SDL3 frontend" ON)
option(EZY_BUILD_TESTS "Build unit tests" ON)

# Strict warnings for our own targets only (not third-party code).
add_library(ezy_warnings INTERFACE)
if(MSVC)
    target_compile_options(ezy_warnings INTERFACE /W4 /permissive- /utf-8)
else()
    target_compile_options(ezy_warnings INTERFACE -Wall -Wextra -Wpedantic -Wconversion)
endif()

include(FetchContent)

add_subdirectory(src/core/chip8)
add_subdirectory(src/core/gb)

if(EZY_BUILD_FRONTEND)
    # Static SDL avoids having to copy SDL3.dll next to the executable.
    set(SDL_SHARED OFF CACHE BOOL "" FORCE)
    set(SDL_STATIC ON CACHE BOOL "" FORCE)
    set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(SDL3
        GIT_REPOSITORY https://github.com/libsdl-org/SDL.git
        GIT_TAG release-3.2.10
        GIT_SHALLOW TRUE)
    FetchContent_MakeAvailable(SDL3)

    # Dear ImGui ships no CMakeLists, so we build the core + SDL3 backends ourselves.
    FetchContent_Declare(imgui
        GIT_REPOSITORY https://github.com/ocornut/imgui.git
        GIT_TAG v1.91.8
        GIT_SHALLOW TRUE)
    FetchContent_MakeAvailable(imgui)
    add_library(imgui STATIC
        ${imgui_SOURCE_DIR}/imgui.cpp
        ${imgui_SOURCE_DIR}/imgui_draw.cpp
        ${imgui_SOURCE_DIR}/imgui_tables.cpp
        ${imgui_SOURCE_DIR}/imgui_widgets.cpp
        ${imgui_SOURCE_DIR}/imgui_demo.cpp
        ${imgui_SOURCE_DIR}/backends/imgui_impl_sdl3.cpp
        ${imgui_SOURCE_DIR}/backends/imgui_impl_sdlrenderer3.cpp)
    target_include_directories(imgui PUBLIC ${imgui_SOURCE_DIR} ${imgui_SOURCE_DIR}/backends)
    target_link_libraries(imgui PUBLIC SDL3::SDL3)

    add_subdirectory(src/frontend)
endif()

if(EZY_BUILD_TESTS)
    # doctest's CMakeLists predates CMake 4's minimum-version policy.
    set(CMAKE_POLICY_VERSION_MINIMUM 3.5)
    FetchContent_Declare(doctest
        GIT_REPOSITORY https://github.com/doctest/doctest.git
        GIT_TAG v2.4.11
        GIT_SHALLOW TRUE)
    FetchContent_MakeAvailable(doctest)
    unset(CMAKE_POLICY_VERSION_MINIMUM)

    enable_testing()
    add_subdirectory(tests/chip8)
    add_subdirectory(tests/gb)
endif()
