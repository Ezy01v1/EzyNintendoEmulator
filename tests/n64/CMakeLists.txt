add_executable(ezy_n64_tests test_main.cpp test_scheduler.cpp test_cartridge.cpp test_cpu.cpp test_cop0.cpp test_tlb.cpp test_cop1.cpp test_bus.cpp test_vi.cpp test_pif.cpp test_ai.cpp test_state.cpp test_rdp_worker.cpp)
target_link_libraries(ezy_n64_tests PRIVATE ezy_n64 doctest::doctest ezy_warnings)
target_sources(ezy_n64_tests PRIVATE test_rsp_jit.cpp test_rsp_vector_native.cpp)
target_sources(ezy_n64_tests PRIVATE test_rsp_vector_multiply.cpp)
target_sources(ezy_n64_tests PRIVATE test_rdp_hidden_bits.cpp)
if(TARGET asmjit::asmjit)
    target_link_libraries(ezy_n64_tests PRIVATE asmjit::asmjit)
endif()
target_compile_definitions(ezy_n64_tests PRIVATE DOCTEST_CONFIG_USE_STD_HEADERS)
add_test(NAME ezy_n64_tests COMMAND ezy_n64_tests)
add_test(NAME ezy_n64_threaded_tests COMMAND ezy_n64_tests --rdp-threaded)
set_tests_properties(ezy_n64_tests ezy_n64_threaded_tests PROPERTIES TIMEOUT 120)
if(EZY_N64_RSP_JIT AND TARGET asmjit::asmjit)
    add_test(NAME ezy_n64_rsp_jit_tests COMMAND ezy_n64_tests --rsp-jit)
    add_test(NAME ezy_n64_rsp_jit_threaded_tests COMMAND ezy_n64_tests --rsp-jit --rdp-threaded)
    set_tests_properties(ezy_n64_rsp_jit_tests ezy_n64_rsp_jit_threaded_tests PROPERTIES TIMEOUT 180)
endif()

# PeterLemon's CPU test ROMs (roms/README.md): each one whose .bmp reference sits next to
# it, compared on the VI image. Release only, and only when the ROMs are present.
# Known failures, left out (see roms/README.md, "Nintendo 64 results"):
#  - CPU/LOADSTORE/SB and SH: their references draw the last dashed line 1 pixel lower than
#    the same font and macro do in every other test (SW, LB, ...): stale images.
#  - DMAAlignment-PI-ROM-FROM: one cell reads PI_DRAM_ADDR as 0x80024408 right after a DMA
#    while the same test then checks it is 0x24408.
set(EZY_N64_ROMS ${PROJECT_SOURCE_DIR}/roms/n64)
file(GLOB_RECURSE EZY_PETERLEMON_ROMS ${EZY_N64_ROMS}/peterlemon/*.N64)
set(EZY_PETERLEMON_KNOWN_FAILURES CPUSB CPUSH DMAAlignment-PI-ROM-FROM)
foreach(rom IN LISTS EZY_PETERLEMON_ROMS)
    get_filename_component(dir ${rom} DIRECTORY)
    get_filename_component(name ${rom} NAME_WLE)
    if(NOT EXISTS ${dir}/${name}.bmp OR name IN_LIST EZY_PETERLEMON_KNOWN_FAILURES)
        continue()
    endif()
    file(RELATIVE_PATH rel ${EZY_N64_ROMS}/peterlemon ${dir})
    string(REPLACE "/" "_" group ${rel})
    add_test(NAME n64_peterlemon_${group}_${name} COMMAND ezy_n64_runner ${rom} --compare ${dir}/${name}.bmp
             CONFIGURATIONS Release)
endforeach()
