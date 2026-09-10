# Rebuild the existing MIT ImGui dependency with item hooks for this executable
# only. Never enable vcpkg's test-engine feature (it adds a separate dependency).
if(NOT SPECFORGE_IMGUI_VERSION STREQUAL "1.92.8")
    message(FATAL_ERROR "Widget harness source pin must match the installed ImGui version")
endif()
include(FetchContent)
FetchContent_Declare(specforge_widget_imgui_source
    URL https://github.com/ocornut/imgui/archive/refs/tags/v1.92.8-docking.tar.gz
    URL_HASH SHA512=927ecf72f00a228e0899d5b8008575b44748c49b083b9425b5f2a6b4490a9900eae111afad23f2bf0a1c9c62cf1fea80c903eb3076d7e7ea901a5625f09df78e
    SOURCE_SUBDIR specforge-no-upstream-cmake
)
FetchContent_MakeAvailable(specforge_widget_imgui_source)
add_library(specforge_widget_imgui STATIC
    ${specforge_widget_imgui_source_SOURCE_DIR}/imgui.cpp
    ${specforge_widget_imgui_source_SOURCE_DIR}/imgui_draw.cpp
    ${specforge_widget_imgui_source_SOURCE_DIR}/imgui_tables.cpp
    ${specforge_widget_imgui_source_SOURCE_DIR}/imgui_widgets.cpp
)
target_include_directories(specforge_widget_imgui PUBLIC ${specforge_widget_imgui_source_SOURCE_DIR})
target_compile_definitions(specforge_widget_imgui PRIVATE IMGUI_ENABLE_TEST_ENGINE)
add_library(specforge_widget_harness STATIC imgui_widget_harness.cpp)
target_include_directories(specforge_widget_harness PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(specforge_widget_harness PUBLIC IMGUI_ENABLE_TEST_ENGINE)
target_link_libraries(specforge_widget_harness PUBLIC specforge_widget_imgui)
specforge_enable_warnings(specforge_widget_harness)

# Keep the instrumented implementation ahead of the transitive production ImGui
# dependency. Only opt-in test executables may use this helper.
function(specforge_add_widget_suite target source)
    add_executable(${target} ${source})
    target_link_libraries(${target} PRIVATE specforge_widget_harness specforge_desktop_ui dwmapi)
    specforge_enable_warnings(${target})
endfunction()

specforge_add_widget_suite(specforge_widget_tests settings_widget_tests.cpp)
specforge_enable_warnings(specforge_widget_tests)
add_test(NAME specforge_widget_failures_tests COMMAND specforge_widget_tests failures)
add_test(NAME specforge_widget_input_tests COMMAND specforge_widget_tests input)
add_test(NAME specforge_widget_theme_tests COMMAND specforge_widget_tests theme)
add_test(NAME specforge_widget_scale_tests COMMAND specforge_widget_tests scale)
add_test(NAME specforge_widget_recording_tests COMMAND specforge_widget_tests recording)
specforge_set_test_tier(specforge_widget_failures_tests fast)
specforge_set_test_tier(specforge_widget_input_tests fast)
specforge_set_test_tier(specforge_widget_theme_tests fast)
specforge_set_test_tier(specforge_widget_scale_tests fast)
specforge_set_test_tier(specforge_widget_recording_tests fast)
set_tests_properties(
    specforge_widget_failures_tests specforge_widget_input_tests
    specforge_widget_theme_tests specforge_widget_scale_tests
    specforge_widget_recording_tests
    PROPERTIES TIMEOUT 30
)
set_property(TEST
    specforge_widget_failures_tests specforge_widget_input_tests
    specforge_widget_theme_tests specforge_widget_scale_tests
    specforge_widget_recording_tests
    APPEND PROPERTY LABELS widget
)
