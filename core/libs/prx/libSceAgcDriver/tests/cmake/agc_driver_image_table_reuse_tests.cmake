if(BUILD_TESTING)
    agc_add_draw_pipeline_test(agc_driver_image_table_reuse)
    add_test(NAME agc_driver_image_table_reuse_overflow COMMAND agc_driver_image_table_reuse_tests overflow)
    set_tests_properties(agc_driver_image_table_reuse_overflow PROPERTIES
        ENVIRONMENT "ANYPS5_NO_SHADER_CACHE=1;APS5_NO_SAMPLED_FROM_STORAGE=1"
        SKIP_RETURN_CODE 77 WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}" TIMEOUT 60)
    if(WIN32)
        set_tests_properties(agc_driver_image_table_reuse_overflow PROPERTIES ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:$<TARGET_FILE_DIR:libc>")
    endif()
endif()
