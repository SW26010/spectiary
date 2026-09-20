# Rebuild the existing MIT ImGui dependency with item hooks for this executable
# only. Never enable vcpkg's test-engine feature (it adds a separate dependency).
if(NOT SPECTIARY_IMGUI_VERSION STREQUAL "1.92.8")
    message(FATAL_ERROR "Widget harness source pin must match the installed ImGui version")
endif()
include(FetchContent)
FetchContent_Declare(spectiary_widget_imgui_source
    URL https://github.com/ocornut/imgui/archive/refs/tags/v1.92.8-docking.tar.gz
    URL_HASH SHA512=927ecf72f00a228e0899d5b8008575b44748c49b083b9425b5f2a6b4490a9900eae111afad23f2bf0a1c9c62cf1fea80c903eb3076d7e7ea901a5625f09df78e
    SOURCE_SUBDIR spectiary-no-upstream-cmake
)
FetchContent_MakeAvailable(spectiary_widget_imgui_source)
add_library(spectiary_widget_imgui STATIC
    ${spectiary_widget_imgui_source_SOURCE_DIR}/imgui.cpp
    ${spectiary_widget_imgui_source_SOURCE_DIR}/imgui_draw.cpp
    ${spectiary_widget_imgui_source_SOURCE_DIR}/imgui_tables.cpp
    ${spectiary_widget_imgui_source_SOURCE_DIR}/imgui_widgets.cpp
)
target_include_directories(spectiary_widget_imgui PUBLIC ${spectiary_widget_imgui_source_SOURCE_DIR})
target_compile_definitions(spectiary_widget_imgui PRIVATE IMGUI_ENABLE_TEST_ENGINE)
add_library(spectiary_widget_harness STATIC imgui_widget_harness.cpp)
target_include_directories(spectiary_widget_harness PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(spectiary_widget_harness PUBLIC IMGUI_ENABLE_TEST_ENGINE)
target_link_libraries(spectiary_widget_harness PUBLIC spectiary_widget_imgui)
spectiary_enable_warnings(spectiary_widget_harness)

# Keep the instrumented implementation ahead of the transitive production ImGui
# dependency. Only opt-in test executables may use this helper.
function(spectiary_add_widget_suite target source)
    add_executable(${target} ${source})
    target_link_libraries(${target} PRIVATE spectiary_widget_harness spectiary_desktop_ui dwmapi)
    spectiary_enable_warnings(${target})
endfunction()

spectiary_add_widget_suite(spectiary_widget_tests settings_widget_tests.cpp)
spectiary_add_widget_suite(spectiary_source_annotation_widget_tests source_annotation_widget_tests.cpp)
add_test(NAME spectiary_source_annotation_widget_tests COMMAND spectiary_source_annotation_widget_tests)
spectiary_set_test_tier(spectiary_source_annotation_widget_tests fast)
set_tests_properties(spectiary_source_annotation_widget_tests PROPERTIES TIMEOUT 30)
set_property(TEST spectiary_source_annotation_widget_tests APPEND PROPERTY LABELS widget)
spectiary_add_widget_suite(spectiary_source_files_widget_tests source_files_widget_tests.cpp)
add_test(NAME spectiary_source_files_widget_tests COMMAND spectiary_source_files_widget_tests)
spectiary_set_test_tier(spectiary_source_files_widget_tests fast)
set_tests_properties(spectiary_source_files_widget_tests PROPERTIES TIMEOUT 30)
set_property(TEST spectiary_source_files_widget_tests APPEND PROPERTY LABELS widget)
spectiary_enable_warnings(spectiary_widget_tests)
add_test(NAME spectiary_widget_failures_tests COMMAND spectiary_widget_tests failures)
add_test(NAME spectiary_widget_input_tests COMMAND spectiary_widget_tests input)
add_test(NAME spectiary_widget_theme_tests COMMAND spectiary_widget_tests theme)
add_test(NAME spectiary_widget_scale_tests COMMAND spectiary_widget_tests scale)
add_test(NAME spectiary_widget_recording_tests COMMAND spectiary_widget_tests recording)
spectiary_set_test_tier(spectiary_widget_failures_tests fast)
spectiary_set_test_tier(spectiary_widget_input_tests fast)
spectiary_set_test_tier(spectiary_widget_theme_tests fast)
spectiary_set_test_tier(spectiary_widget_scale_tests fast)
spectiary_set_test_tier(spectiary_widget_recording_tests fast)
set_tests_properties(
    spectiary_widget_failures_tests spectiary_widget_input_tests
    spectiary_widget_theme_tests spectiary_widget_scale_tests
    spectiary_widget_recording_tests
    PROPERTIES TIMEOUT 30
)
set_property(TEST
    spectiary_widget_failures_tests spectiary_widget_input_tests
    spectiary_widget_theme_tests spectiary_widget_scale_tests
    spectiary_widget_recording_tests
    APPEND PROPERTY LABELS widget
)

get_target_property(workflow_libraries spectiary_sample_workflow_shortcut_tests LINK_LIBRARIES)
set_property(TARGET spectiary_sample_workflow_shortcut_tests PROPERTY LINK_LIBRARIES
    spectiary_widget_harness ${workflow_libraries})

get_target_property(shell_libraries spectiary_shell_source_load_activation_tests LINK_LIBRARIES)
set_property(TARGET spectiary_shell_source_load_activation_tests PROPERTY LINK_LIBRARIES
    spectiary_widget_harness ${shell_libraries})

spectiary_add_widget_suite(spectiary_spectral_lines_widget_tests spectral_lines_widget_tests.cpp)
add_test(NAME spectiary_spectral_lines_widget_tests COMMAND spectiary_spectral_lines_widget_tests)
spectiary_set_test_tier(spectiary_spectral_lines_widget_tests fast)
set_tests_properties(spectiary_spectral_lines_widget_tests PROPERTIES TIMEOUT 30)
set_property(TEST spectiary_spectral_lines_widget_tests APPEND PROPERTY LABELS widget)
