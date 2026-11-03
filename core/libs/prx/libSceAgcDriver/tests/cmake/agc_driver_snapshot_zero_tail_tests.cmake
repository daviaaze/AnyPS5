if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    add_test_executable(agc_driver_snapshot_zero_tail_tests EXCLUDE_FROM_ALL
            tests/SnapshotZeroTail.cpp)
    target_include_directories(agc_driver_snapshot_zero_tail_tests PRIVATE
            ${LIBS_INCLUDE_DIR}
            $<TARGET_PROPERTY:shader_recompiler_agc_driver,INTERFACE_INCLUDE_DIRECTORIES>
            ${agcTextureDetileGeneratedRoot}
            ${CMAKE_SOURCE_DIR}/core/shader/recompiler
            ${CMAKE_SOURCE_DIR}/core/shader/recompiler/IntermediateRepresentation/include
            ${CMAKE_SOURCE_DIR}/core/shader/recompiler/ControlFlow/include
            ${CMAKE_SOURCE_DIR}/core/shader/recompiler/RdnaDecoder/include
            ${ANYPS5_VULKAN_HEADERS_INCLUDE_DIR}
            ${ANYPS5_SPIRV_HEADERS_INCLUDE_DIR})
    target_link_libraries(agc_driver_snapshot_zero_tail_tests PRIVATE libSceAgcDriver libc libkernel)
    set_target_properties(agc_driver_snapshot_zero_tail_tests PROPERTIES CXX_EXTENSIONS OFF)
    configure_windows_unwind(agc_driver_snapshot_zero_tail_tests)
    if(BUILD_TESTING)
        set_target_properties(agc_driver_snapshot_zero_tail_tests PROPERTIES EXCLUDE_FROM_ALL OFF)
        add_test(NAME agc_driver_snapshot_zero_tail COMMAND agc_driver_snapshot_zero_tail_tests)
        set_tests_properties(agc_driver_snapshot_zero_tail PROPERTIES
                ENVIRONMENT "ANYPS5_NO_SHADER_CACHE=1" SKIP_RETURN_CODE 77 TIMEOUT 30)
    endif()
endif()
