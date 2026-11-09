add_library(ezy_n64 STATIC scheduler.cpp cartridge.cpp cpu/vr4300.cpp cpu/vr4300_ops.cpp cpu/cop0.cpp cpu/tlb.cpp cpu/cop1.cpp bus.cpp pi.cpp rsp_regs.cpp rsp_scalar.cpp rsp_vector.cpp rsp_vector_memory.cpp rdp.cpp rdp_texture.cpp vi.cpp pif.cpp si.cpp hle_boot.cpp ai.cpp n64.cpp gfx_hle/task.cpp gfx_hle/f3dex.cpp gfx_hle/f3dex_vertex.cpp gfx_hle/f3dex_triangle.cpp gfx_hle/f3dex_clip.cpp)
target_include_directories(ezy_n64 PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(ezy_n64 PRIVATE ezy_warnings)
