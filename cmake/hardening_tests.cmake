if(SPECTIARY_ENABLE_STATIC_ANALYSIS)
    add_custom_target(spectiary_static_analysis_targets DEPENDS
        spectiary_core spectiary_sessions spectiary_automation
        spectiary_desktop_ui spectiary_renderer spectiary_imgui_layout)
endif()

if(SPECTIARY_ENABLE_ASAN)
    # One explicit list owns both the build closure and CTest selection. Include
    # parsers, persistence, asynchronous loading and native/headless automation.
    set(spectiary_asan_tests
        spectiary_atomic_file_tests
        spectiary_loader_tests
        spectiary_fits_file_reader_tests
        spectiary_sample_annotation_csv_tests
        spectiary_sample_labeling_asdf_codec_tests
        spectiary_sample_labeling_asdf_store_tests
        spectiary_sample_labeling_asdf_mutation_tests
        spectiary_sample_labeling_asdf_store_property_tests
        spectiary_sample_navigation_controller_tests
        spectiary_sample_navigation_sequence_tests
        spectiary_sample_workflow_rules_tests
        spectiary_source_collection_load_queue_tests
        spectiary_source_collection_preparation_tests
        spectiary_automation_control_tests
        spectiary_automation_execution_tests
        spectiary_automation_panel_command_coordinator_tests
        spectiary_automation_state_isolation_tests
    )
    set_property(TEST ${spectiary_asan_tests} APPEND PROPERTY LABELS asan)
    add_executable(spectiary_asan_probe tests/fixtures/asan_probe.cpp)
    add_test(NAME spectiary_asan_detection_tests
        COMMAND "${CMAKE_COMMAND}" "-DPROGRAM=$<TARGET_FILE:spectiary_asan_probe>"
            -P "${CMAKE_SOURCE_DIR}/tests/asan_detection_tests.cmake")
    set_tests_properties(spectiary_asan_detection_tests PROPERTIES LABELS asan TIMEOUT 30)
    spectiary_set_test_tier(spectiary_asan_detection_tests extended)
    add_custom_target(spectiary_asan_targets DEPENDS
        ${spectiary_asan_tests} spectiary_asan_probe)
endif()
