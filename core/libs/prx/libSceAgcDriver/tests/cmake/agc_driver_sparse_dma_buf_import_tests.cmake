if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_link_libraries(agc_driver_sparse_dma_buf_import_tests PRIVATE libkernel)
endif()

if(BUILD_TESTING)
    add_test(NAME agc_driver_fragmented_dma_buf_import COMMAND agc_driver_sparse_dma_buf_import_tests --fragmented-only)
    set_tests_properties(agc_driver_fragmented_dma_buf_import PROPERTIES
        ENVIRONMENT "ANYPS5_NO_SHADER_CACHE=1" SKIP_RETURN_CODE 77 TIMEOUT 120 WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        add_test(NAME agc_driver_anonymous_dma_buf_import COMMAND agc_driver_sparse_dma_buf_import_tests --anonymous-only)
        set_tests_properties(agc_driver_anonymous_dma_buf_import PROPERTIES
            ENVIRONMENT "ANYPS5_NO_SHADER_CACHE=1" SKIP_RETURN_CODE 77 TIMEOUT 120 WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
    endif()
    if(ANYPS5_REQUIRE_CPU_LEAD_TEST)
        add_test(NAME agc_driver_sparse_dma_buf_cpu_lead COMMAND agc_driver_sparse_dma_buf_import_tests --require-cpu-lead)
        set_tests_properties(agc_driver_sparse_dma_buf_cpu_lead PROPERTIES ENVIRONMENT "ANYPS5_NO_SHADER_CACHE=1" TIMEOUT 120 WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
    endif()
    if(WIN32 AND ANYPS5_REQUIRE_CPU_LEAD_TEST)
        set_tests_properties(agc_driver_sparse_dma_buf_cpu_lead PROPERTIES ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:$<TARGET_FILE_DIR:libc>")
    endif()
endif()
