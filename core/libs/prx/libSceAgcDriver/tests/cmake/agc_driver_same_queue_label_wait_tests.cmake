if(BUILD_TESTING)
    agc_add_draw_pipeline_test(agc_driver_same_queue_label_wait)
    add_test(NAME agc_driver_same_queue_label_wait_trace COMMAND agc_driver_same_queue_label_wait_tests wait-edge-trace)
    set_tests_properties(agc_driver_same_queue_label_wait_trace PROPERTIES
        ENVIRONMENT "ANYPS5_NO_SHADER_CACHE=1;APS5_TRACE_FRAMES=0:1;APS5_TRACE_DIR=${CMAKE_CURRENT_BINARY_DIR}/wait-edge-trace"
        SKIP_RETURN_CODE 77 TIMEOUT 30 WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
    if(WIN32)
        set_tests_properties(agc_driver_same_queue_label_wait_trace PROPERTIES ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:$<TARGET_FILE_DIR:libc>")
    endif()
endif()
