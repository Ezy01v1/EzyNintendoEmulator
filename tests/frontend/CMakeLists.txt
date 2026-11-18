# Frontend pieces that need no window or audio device (header-only, no SDL).
add_executable(ezy_frontend_tests test_time_stretch.cpp)
target_include_directories(ezy_frontend_tests PRIVATE ${PROJECT_SOURCE_DIR}/src/frontend)
target_link_libraries(ezy_frontend_tests PRIVATE doctest::doctest_with_main ezy_warnings)
target_compile_definitions(ezy_frontend_tests PRIVATE DOCTEST_CONFIG_USE_STD_HEADERS)
add_test(NAME ezy_frontend_tests COMMAND ezy_frontend_tests)
