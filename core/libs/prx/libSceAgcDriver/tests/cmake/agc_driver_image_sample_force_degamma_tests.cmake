if(BUILD_TESTING AND TARGET agc_driver_image_sample_force_degamma_tests)
    add_test(NAME agc_driver_image_sample_force_degamma_sparse_disabled
            COMMAND agc_driver_image_sample_force_degamma_tests --segmented-sparse-off)
    set_tests_properties(agc_driver_image_sample_force_degamma_sparse_disabled PROPERTIES
            ENVIRONMENT "ANYPS5_NO_SHADER_CACHE=1;APS5_NO_PIPELINE_JOURNAL=1;APS5_NO_SPARSE_HEAP_MIRROR=1"
            SKIP_RETURN_CODE 77 TIMEOUT 60 WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
    if(WIN32)
        set_tests_properties(agc_driver_image_sample_force_degamma_sparse_disabled PROPERTIES
                ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:libc>")
    endif()
endif()
