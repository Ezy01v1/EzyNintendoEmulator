add_executable(ezy_n64_runner main.cpp benchmark.cpp)
target_link_libraries(ezy_n64_runner PRIVATE ezy_n64 ezy_warnings)
if(WIN32)
    target_link_libraries(ezy_n64_runner PRIVATE dbghelp winmm)
endif()

add_executable(ezy_rsp_vector_bench EXCLUDE_FROM_ALL vector_bench.cpp)
target_link_libraries(ezy_rsp_vector_bench PRIVATE ezy_n64 ezy_warnings)
