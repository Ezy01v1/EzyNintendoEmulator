add_executable(gfx_oracle main.cpp)
target_include_directories(gfx_oracle PRIVATE ${PROJECT_SOURCE_DIR}/tests/n64)
target_link_libraries(gfx_oracle PRIVATE ezy_n64 ezy_warnings)
