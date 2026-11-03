if(BUILD_TESTING AND NOT WIN32 AND Python3_Interpreter_FOUND)
    add_test_executable(agc_p0_table_atomic_tests EXCLUDE_FROM_ALL tests/P0TableAtomic.cpp)
    target_include_directories(agc_p0_table_atomic_tests PRIVATE ${LIBS_INCLUDE_DIR})
    target_link_libraries(agc_p0_table_atomic_tests PRIVATE libSceAgcDriver libkernel libc)
    set_target_properties(agc_p0_table_atomic_tests PROPERTIES CXX_EXTENSIONS OFF EXCLUDE_FROM_ALL OFF)
    add_test(NAME agc_p0_table_atomic COMMAND ${Python3_EXECUTABLE}
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_p0_table_atomic.py $<TARGET_FILE:agc_p0_table_atomic_tests>)
    set_tests_properties(agc_p0_table_atomic PROPERTIES TIMEOUT 30)
endif()
