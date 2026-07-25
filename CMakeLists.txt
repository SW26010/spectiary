cmake_minimum_required(VERSION 3.24)

project(SpecForge VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(SPECFORGE_EMBED_PUBLIC_SPECTRAL_LINES "Embed the public spectral line catalog in the executable." OFF)
set(SPECFORGE_RELEASE_PROFILE "" CACHE STRING "SpecForge release profile: Portable or Installed.")
set_property(CACHE SPECFORGE_RELEASE_PROFILE PROPERTY STRINGS Portable Installed)

if(NOT SPECFORGE_RELEASE_PROFILE MATCHES "^(Portable|Installed)$")
    message(FATAL_ERROR "SPECFORGE_RELEASE_PROFILE must be exactly Portable or Installed.")
endif()

find_package(imgui CONFIG REQUIRED)
find_package(implot CONFIG REQUIRED)
find_package(ZLIB REQUIRED)

set(SPECFORGE_MANIFEST "${CMAKE_CURRENT_SOURCE_DIR}/src/platform/specforge.exe.manifest")
set(SPECFORGE_PUBLIC_SPECTRAL_LINES_TSV "${CMAKE_SOURCE_DIR}/config/spectral_lines.public.tsv")
set(SPECFORGE_THIRD_PARTY_NOTICES "${CMAKE_SOURCE_DIR}/legal/THIRD_PARTY_NOTICES.txt")
set(SPECFORGE_RELEASE_DOCUMENTS
    "${CMAKE_SOURCE_DIR}/legal/EULA.txt"
    "${SPECFORGE_THIRD_PARTY_NOTICES}"
    "${CMAKE_SOURCE_DIR}/legal/DATA_SOURCES.txt"
)

include("${CMAKE_SOURCE_DIR}/cmake/third_party_versions.cmake")
specforge_read_vcpkg_package_version(imgui SPECFORGE_IMGUI_VERSION)
specforge_read_vcpkg_package_version(implot SPECFORGE_IMPLOT_VERSION)
specforge_read_vcpkg_package_version(zlib SPECFORGE_ZLIB_VERSION)
specforge_require_notice_heading(
    "${SPECFORGE_THIRD_PARTY_NOTICES}"
    "Dear ImGui"
    "Dear ImGui ${SPECFORGE_IMGUI_VERSION}, including docking, Win32, and DirectX 11 backends"
)
specforge_require_notice_heading(
    "${SPECFORGE_THIRD_PARTY_NOTICES}"
    "ImPlot"
    "ImPlot ${SPECFORGE_IMPLOT_VERSION}"
)
specforge_require_notice_heading(
    "${SPECFORGE_THIRD_PARTY_NOTICES}"
    "zlib"
    "zlib ${SPECFORGE_ZLIB_VERSION}"
)
message(STATUS
    "Resolved third-party versions: "
    "Dear ImGui ${SPECFORGE_IMGUI_VERSION}, "
    "ImPlot ${SPECFORGE_IMPLOT_VERSION}, "
    "zlib ${SPECFORGE_ZLIB_VERSION}"
)

file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/generated/specforge")
configure_file(
    "${CMAKE_SOURCE_DIR}/cmake/third_party_versions.h.in"
    "${CMAKE_BINARY_DIR}/generated/specforge/third_party_versions.h"
    @ONLY
)
configure_file(
    "${CMAKE_SOURCE_DIR}/cmake/specforge_build_metadata.json.in"
    "${CMAKE_BINARY_DIR}/generated/specforge/specforge_build_metadata.json.in"
    @ONLY
)
file(GENERATE
    OUTPUT
        "${CMAKE_BINARY_DIR}/generated/specforge/$<CONFIG>/specforge_build_metadata.json"
    INPUT
        "${CMAKE_BINARY_DIR}/generated/specforge/specforge_build_metadata.json.in"
)

if(SPECFORGE_EMBED_PUBLIC_SPECTRAL_LINES)
    set(SPECFORGE_EMBEDDED_PUBLIC_SPECTRAL_LINES_HEADER
        "${CMAKE_BINARY_DIR}/generated/specforge/public_spectral_lines_embedded.h"
    )
    add_custom_command(
        OUTPUT "${SPECFORGE_EMBEDDED_PUBLIC_SPECTRAL_LINES_HEADER}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${CMAKE_BINARY_DIR}/generated/specforge"
        COMMAND
            "${CMAKE_COMMAND}"
            "-DINPUT=${SPECFORGE_PUBLIC_SPECTRAL_LINES_TSV}"
            "-DOUTPUT=${SPECFORGE_EMBEDDED_PUBLIC_SPECTRAL_LINES_HEADER}"
            -P "${CMAKE_SOURCE_DIR}/cmake/embed_public_spectral_lines.cmake"
        DEPENDS
            "${SPECFORGE_PUBLIC_SPECTRAL_LINES_TSV}"
            "${CMAKE_SOURCE_DIR}/cmake/embed_public_spectral_lines.cmake"
        VERBATIM
    )
    add_custom_target(
        specforge_embedded_public_spectral_lines
        DEPENDS "${SPECFORGE_EMBEDDED_PUBLIC_SPECTRAL_LINES_HEADER}"
    )

    add_library(specforge_embedded_resources INTERFACE)
    target_include_directories(specforge_embedded_resources INTERFACE "${CMAKE_BINARY_DIR}/generated")
    target_compile_definitions(specforge_embedded_resources INTERFACE SPECFORGE_EMBED_PUBLIC_SPECTRAL_LINES)
endif()

function(specforge_use_embedded_resources target_name)
    if(SPECFORGE_EMBED_PUBLIC_SPECTRAL_LINES)
        target_link_libraries(${target_name} PRIVATE specforge_embedded_resources)
        add_dependencies(${target_name} specforge_embedded_public_spectral_lines)
    endif()
endfunction()

function(specforge_apply_release_profile target_name)
    if(SPECFORGE_RELEASE_PROFILE STREQUAL "Portable")
        target_compile_definitions(${target_name} PRIVATE SPECFORGE_RELEASE_PROFILE_PORTABLE)
    elseif(SPECFORGE_RELEASE_PROFILE STREQUAL "Installed")
        target_compile_definitions(${target_name} PRIVATE SPECFORGE_RELEASE_PROFILE_INSTALLED)
    else()
        message(FATAL_ERROR "Unknown SPECFORGE_RELEASE_PROFILE: ${SPECFORGE_RELEASE_PROFILE}")
    endif()
endfunction()

function(specforge_use_runtime_paths target_name)
    specforge_apply_release_profile(${target_name})
    if(WIN32)
        target_link_libraries(${target_name} PRIVATE ole32 shell32)
    endif()
endfunction()

function(specforge_enable_warnings target_name)
    if(MSVC)
        target_compile_options(${target_name} PRIVATE /EHsc /W4 /permissive- /utf-8)
    else()
        target_compile_options(${target_name} PRIVATE -Wall -Wextra -Wpedantic)
    endif()
endfunction()

function(specforge_configure_production_target target_name)
    target_include_directories(${target_name} PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
    target_compile_definitions(${target_name} PRIVATE
        NOMINMAX
        UNICODE
        _UNICODE
        WIN32_LEAN_AND_MEAN
    )
    specforge_enable_warnings(${target_name})
endfunction()

add_library(specforge_core STATIC
    src/app/local_user_state.cpp
    src/app/local_user_state_json.cpp
    src/app/pan_pacing.cpp
    src/app/render_wake_scheduler.cpp
    src/app/runtime_paths.cpp
    src/domain/fits_file_reader.cpp
    src/domain/fits_spectrum_loader.cpp
    src/domain/npy_array_io.cpp
    src/domain/sample_annotation_io.cpp
    src/domain/sample_filter.cpp
    src/domain/sample_labeling.cpp
    src/domain/source_collection_identity_digest.cpp
    src/domain/source_collection_manifest.cpp
    src/domain/source_path_identity.cpp
    src/domain/stable_sha256.cpp
    src/domain/spectrum_fixture.cpp
    src/domain/spectrum_format_adapters.cpp
    src/domain/spectrum_loader_support.cpp
    src/domain/spectrum_loader.cpp
    src/domain/spectrum_smoothing.cpp
    src/overlays/spectral_line_catalog.cpp
    src/overlays/spectral_line_user_state_cache_io.cpp
    src/overlays/spectral_line_user_state.cpp
    src/platform/atomic_file.cpp
    src/platform/win32_directory_change_generation.cpp
    src/platform/win32_compositor_clock.cpp
    src/platform/win32_message_render_observer.cpp
    src/platform/win32_message_wait.cpp
    src/profile/navigation_latency_trace.cpp
    src/profile/navigation_prefetch_trace.cpp
    src/profile/profile_recording_status.cpp
    src/profile/profile_settings.cpp
    src/profile/profile_sink.cpp
    src/profile/source_load_latency_trace.cpp
)
specforge_configure_production_target(specforge_core)
specforge_use_runtime_paths(specforge_core)
specforge_use_embedded_resources(specforge_core)
target_link_libraries(specforge_core PRIVATE ZLIB::ZLIB bcrypt)

add_library(specforge_sessions STATIC
    src/ui/panel_visibility_state_cache_io.cpp
    src/ui/sample_annotation_labeling_rules.cpp
    src/ui/sample_labeling_controller.cpp
    src/ui/sample_labeling_state_cache_io.cpp
    src/ui/sample_name_autocomplete.cpp
    src/ui/sample_navigation_controller.cpp
    src/ui/sample_navigation_sequence.cpp
    src/ui/sample_navigation_state_cache_io.cpp
    src/ui/sample_sorting_sources.cpp
    src/ui/sample_workflow_coordinator.cpp
    src/ui/sample_workflow_preparation.cpp
    src/ui/sample_workflow_source_policy.cpp
    src/ui/sample_workflow_state_cache_io.cpp
    src/ui/source_collection_activation_transaction.cpp
    src/ui/source_collection_roster.cpp
    src/ui/source_collection_load_queue.cpp
    src/ui/source_collection_preparation.cpp
    src/ui/source_collection_session_state_cache_io.cpp
    src/ui/source_collection_session.cpp
    src/ui/spectral_lines_panel_controller.cpp
)
specforge_configure_production_target(specforge_sessions)
target_link_libraries(specforge_sessions PUBLIC specforge_core)

add_library(specforge_desktop_ui STATIC
    src/app/application_settings.cpp
    src/plot/plot_touchpad_gesture.cpp
    src/plot/scientific_label.cpp
    src/plot/spectral_line_label_layout.cpp
    src/plot/spectrum_plot.cpp
    src/ui/panel_session_interaction.cpp
    src/ui/sample_workflow_shortcut.cpp
    src/ui/sample_workflow_panel.cpp
    src/ui/settings_panel.cpp
    src/ui/shell_ui.cpp
    src/ui/source_collection_panel.cpp
    src/ui/spectral_lines_grouping_view.cpp
    src/ui/spectral_lines_panel.cpp
    src/ui/spectrum_view_session.cpp
    src/ui/ui_font.cpp
    src/ui/ui_language_settings.cpp
    src/ui/ui_text.cpp
)
specforge_configure_production_target(specforge_desktop_ui)
target_include_directories(specforge_desktop_ui PRIVATE "${CMAKE_BINARY_DIR}/generated")
target_compile_definitions(specforge_desktop_ui PRIVATE
    SPECFORGE_VERSION="${PROJECT_VERSION}"
)
target_link_libraries(specforge_desktop_ui
    PUBLIC
        specforge_sessions
        imgui::imgui
        implot::implot
    PRIVATE
        shell32
)

add_library(specforge_renderer STATIC
    src/renderer/d3d11_composition_swap_chain.cpp
    src/renderer/d3d11_imgui_viewport_renderer.cpp
    src/renderer/d3d11_renderer.cpp
    src/renderer/d3d11_sdr_swap_chain.cpp
    src/renderer/d3d11_window_presentation.cpp
    src/renderer/win32_display_refresh.cpp
)
specforge_configure_production_target(specforge_renderer)
target_link_libraries(specforge_renderer
    PRIVATE
        imgui::imgui
        dcomp
        d3d11
        dxgi
)

add_executable(specforge_native WIN32
    src/app/specforge_app.cpp
    src/main.cpp
    src/platform/win32_touchpad_gesture_source.cpp
    src/platform/win32_window.cpp
    ${SPECFORGE_MANIFEST}
)
specforge_configure_production_target(specforge_native)
specforge_apply_release_profile(specforge_native)
target_link_libraries(specforge_native PRIVATE
    specforge_desktop_ui
    specforge_renderer
    dwmapi
    comctl32
    imm32
    ole32
    uuid
)

set_target_properties(specforge_native PROPERTIES
    OUTPUT_NAME SpecForge
    VS_DEBUGGER_WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)

if(NOT SPECFORGE_EMBED_PUBLIC_SPECTRAL_LINES)
    add_custom_command(TARGET specforge_native POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E make_directory "$<TARGET_FILE_DIR:specforge_native>/config"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${SPECFORGE_PUBLIC_SPECTRAL_LINES_TSV}"
            "$<TARGET_FILE_DIR:specforge_native>/config/spectral_lines.public.tsv"
    )
endif()

add_custom_command(TARGET specforge_native POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        "${CMAKE_BINARY_DIR}/generated/specforge/$<CONFIG>/specforge_build_metadata.json"
        "$<TARGET_FILE_DIR:specforge_native>/specforge_build_metadata.json"
)

if(CMAKE_CONFIGURATION_TYPES)
    set(SPECFORGE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/$<CONFIG>")
else()
    set(SPECFORGE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
endif()

set(SPECFORGE_RELEASE_DOCUMENT_OUTPUTS)
set(SPECFORGE_RELEASE_DOCUMENT_COPY_COMMANDS)
foreach(release_document IN LISTS SPECFORGE_RELEASE_DOCUMENTS)
    get_filename_component(release_document_name "${release_document}" NAME)
    set(release_document_output
        "${SPECFORGE_RUNTIME_OUTPUT_DIRECTORY}/Legal/${release_document_name}"
    )
    list(APPEND SPECFORGE_RELEASE_DOCUMENT_OUTPUTS "${release_document_output}")
    list(APPEND SPECFORGE_RELEASE_DOCUMENT_COPY_COMMANDS
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${release_document}"
            "${release_document_output}"
    )
endforeach()

add_custom_target(specforge_release_documents
    COMMAND "${CMAKE_COMMAND}" -E rm -f
        "${SPECFORGE_RUNTIME_OUTPUT_DIRECTORY}/EULA.txt"
        "${SPECFORGE_RUNTIME_OUTPUT_DIRECTORY}/THIRD_PARTY_NOTICES.txt"
        "${SPECFORGE_RUNTIME_OUTPUT_DIRECTORY}/DATA_SOURCES.txt"
    COMMAND "${CMAKE_COMMAND}" -E make_directory
        "${SPECFORGE_RUNTIME_OUTPUT_DIRECTORY}/Legal"
    ${SPECFORGE_RELEASE_DOCUMENT_COPY_COMMANDS}
    BYPRODUCTS ${SPECFORGE_RELEASE_DOCUMENT_OUTPUTS}
    DEPENDS ${SPECFORGE_RELEASE_DOCUMENTS}
    COMMENT "Synchronizing SpecForge release documents"
    VERBATIM
)
add_dependencies(specforge_native specforge_release_documents)

enable_testing()

function(specforge_add_test target_name source_file production_target)
    add_executable(${target_name} ${source_file})
    target_link_libraries(${target_name} PRIVATE ${production_target})
    specforge_enable_warnings(${target_name})
    add_test(NAME ${target_name} COMMAND ${target_name})
endfunction()

specforge_add_test(
    specforge_sdr_swap_chain_tests
    tests/d3d11_sdr_swap_chain_tests.cpp
    specforge_renderer
)
target_link_libraries(specforge_sdr_swap_chain_tests PRIVATE imgui::imgui)

specforge_add_test(
    specforge_local_user_state_tests
    tests/local_user_state_tests.cpp
    specforge_sessions
)
specforge_add_test(
    specforge_render_wake_scheduler_tests
    tests/render_wake_scheduler_tests.cpp
    specforge_core
)
target_link_libraries(specforge_render_wake_scheduler_tests PRIVATE imgui::imgui)
specforge_add_test(
    specforge_profile_recording_ui_state_tests
    tests/profile_recording_ui_state_tests.cpp
    specforge_core
)
specforge_add_test(
    specforge_top_bar_status_layout_tests
    tests/top_bar_status_layout_tests.cpp
    specforge_core
)
specforge_add_test(
    specforge_top_bar_status_hover_tests
    tests/top_bar_status_hover_tests.cpp
    specforge_core
)
target_link_libraries(specforge_top_bar_status_hover_tests PRIVATE imgui::imgui)
specforge_add_test(
    specforge_profile_sink_tests
    tests/profile_sink_tests.cpp
    specforge_core
)
specforge_add_test(
    specforge_profile_settings_tests
    tests/profile_settings_tests.cpp
    specforge_core
)
specforge_add_test(
    specforge_navigation_latency_trace_tests
    tests/navigation_latency_trace_tests.cpp
    specforge_core
)
specforge_add_test(
    specforge_source_load_latency_trace_tests
    tests/source_load_latency_trace_tests.cpp
    specforge_core
)
specforge_add_test(
    specforge_settings_panel_tests
    tests/settings_panel_tests.cpp
    specforge_desktop_ui
)
target_compile_definitions(specforge_settings_panel_tests PRIVATE
    SPECFORGE_EXPECTED_VERSION="${PROJECT_VERSION}"
)
target_link_libraries(specforge_settings_panel_tests PRIVATE dwmapi)
add_test(
    NAME specforge_analyze_profile_tests
    COMMAND powershell -NoProfile -ExecutionPolicy Bypass
        -File "${CMAKE_SOURCE_DIR}/tests/analyze_profile_tests.ps1"
        -Analyzer "${CMAKE_SOURCE_DIR}/scripts/analyze-profile.ps1"
)
add_test(
    NAME specforge_analyze_navigation_profile_tests
    COMMAND powershell -NoProfile -ExecutionPolicy Bypass
        -File "${CMAKE_SOURCE_DIR}/tests/analyze_navigation_profile_tests.ps1"
        -Analyzer "${CMAKE_SOURCE_DIR}/scripts/analyze-navigation-profile.ps1"
)
add_test(
    NAME specforge_analyze_source_load_profile_tests
    COMMAND powershell -NoProfile -ExecutionPolicy Bypass
        -File "${CMAKE_SOURCE_DIR}/tests/analyze_source_load_profile_tests.ps1"
        -Analyzer "${CMAKE_SOURCE_DIR}/scripts/analyze-source-load-profile.ps1"
)
add_test(
    NAME specforge_release_artifacts_tests
    COMMAND powershell -NoProfile -ExecutionPolicy Bypass
        -File "${CMAKE_SOURCE_DIR}/tests/release_artifacts_tests.ps1"
        -RepoRoot "${CMAKE_SOURCE_DIR}"
        -DearImGuiVersion "${SPECFORGE_IMGUI_VERSION}"
        -ImPlotVersion "${SPECFORGE_IMPLOT_VERSION}"
        -ZlibVersion "${SPECFORGE_ZLIB_VERSION}"
        -BuiltExecutable "$<TARGET_FILE:specforge_native>"
        -SpecForgeVersion "${PROJECT_VERSION}"
        -ReleaseProfile "${SPECFORGE_RELEASE_PROFILE}"
        -Configuration "$<CONFIG>"
)
add_test(
    NAME specforge_panel_session_render_architecture_tests
    COMMAND powershell -NoProfile -ExecutionPolicy Bypass
        -File "${CMAKE_SOURCE_DIR}/tests/panel_session_render_architecture_tests.ps1"
        -RepoRoot "${CMAKE_SOURCE_DIR}"
)
specforge_add_test(
    specforge_win32_compositor_clock_tests
    tests/win32_compositor_clock_tests.cpp
    specforge_core
)
specforge_add_test(
    specforge_win32_message_wait_tests
    tests/win32_message_wait_tests.cpp
    specforge_core
)
specforge_add_test(
    specforge_loader_tests
    tests/spectrum_loader_tests.cpp
    specforge_core
)
target_link_libraries(specforge_loader_tests PRIVATE ZLIB::ZLIB)
specforge_add_test(
    specforge_sample_navigation_controller_tests
    tests/sample_navigation_controller_tests.cpp
    specforge_sessions
)
specforge_add_test(
    specforge_sample_navigation_sequence_tests
    tests/sample_navigation_sequence_tests.cpp
    specforge_sessions
)
specforge_add_test(
    specforge_sample_workflow_shortcut_tests
    tests/sample_workflow_shortcut_tests.cpp
    specforge_desktop_ui
)
specforge_add_test(
    specforge_panel_session_interaction_tests
    tests/panel_session_interaction_tests.cpp
    specforge_desktop_ui
)
specforge_add_test(
    specforge_sample_workflow_rules_tests
    tests/sample_workflow_rules_tests.cpp
    specforge_sessions
)
specforge_add_test(
    specforge_sample_name_autocomplete_tests
    tests/sample_name_autocomplete_tests.cpp
    specforge_sessions
)
specforge_add_test(
    specforge_sample_labeling_filter_tests
    tests/sample_labeling_filter_tests.cpp
    specforge_sessions
)
specforge_add_test(
    specforge_source_collection_session_tests
    tests/source_collection_session_tests.cpp
    specforge_sessions
)
specforge_add_test(
    specforge_source_collection_load_queue_tests
    tests/source_collection_load_queue_tests.cpp
    specforge_sessions
)
specforge_add_test(
    specforge_source_collection_preparation_tests
    tests/source_collection_preparation_tests.cpp
    specforge_sessions
)
specforge_add_test(
    specforge_source_collection_activation_transaction_tests
    tests/source_collection_activation_transaction_tests.cpp
    specforge_sessions
)
specforge_add_test(
    specforge_shell_source_load_activation_tests
    tests/shell_source_load_activation_tests.cpp
    specforge_desktop_ui
)
target_link_libraries(specforge_shell_source_load_activation_tests PRIVATE dwmapi)
specforge_add_test(
    specforge_spectrum_smoothing_tests
    tests/spectrum_smoothing_tests.cpp
    specforge_core
)
specforge_add_test(
    specforge_plot_touchpad_gesture_tests
    tests/plot_touchpad_gesture_tests.cpp
    specforge_desktop_ui
)
specforge_add_test(
    specforge_spectral_line_label_layout_tests
    tests/spectral_line_label_layout_tests.cpp
    specforge_desktop_ui
)
specforge_add_test(
    specforge_scientific_label_tests
    tests/scientific_label_tests.cpp
    specforge_desktop_ui
)
specforge_add_test(
    specforge_application_settings_tests
    tests/application_settings_tests.cpp
    specforge_desktop_ui
)
specforge_add_test(
    specforge_ui_font_glyph_ranges_tests
    tests/ui_font_glyph_ranges_tests.cpp
    specforge_desktop_ui
)
specforge_add_test(
    specforge_ui_text_tests
    tests/ui_text_tests.cpp
    specforge_desktop_ui
)
specforge_add_test(
    specforge_ui_language_settings_tests
    tests/ui_language_settings_tests.cpp
    specforge_desktop_ui
)
specforge_add_test(
    specforge_spectrum_view_session_tests
    tests/spectrum_view_session_tests.cpp
    specforge_desktop_ui
)
specforge_add_test(
    specforge_spectral_line_catalog_tests
    tests/spectral_line_catalog_tests.cpp
    specforge_core
)
target_compile_definitions(
    specforge_spectral_line_catalog_tests
    PRIVATE SPECFORGE_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
)
if(SPECFORGE_EMBED_PUBLIC_SPECTRAL_LINES)
    target_compile_definitions(
        specforge_spectral_line_catalog_tests
        PRIVATE SPECFORGE_EXPECT_EMBEDDED_PUBLIC_SPECTRAL_LINES
    )
endif()

if(NOT SPECFORGE_EMBED_PUBLIC_SPECTRAL_LINES)
    add_custom_command(TARGET specforge_spectral_line_catalog_tests POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E make_directory "$<TARGET_FILE_DIR:specforge_spectral_line_catalog_tests>/config"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${SPECFORGE_PUBLIC_SPECTRAL_LINES_TSV}"
            "$<TARGET_FILE_DIR:specforge_spectral_line_catalog_tests>/config/spectral_lines.public.tsv"
    )
endif()

specforge_add_test(
    specforge_spectral_line_user_state_tests
    tests/spectral_line_user_state_tests.cpp
    specforge_core
)
specforge_add_test(
    specforge_spectral_lines_panel_controller_tests
    tests/spectral_lines_panel_controller_tests.cpp
    specforge_sessions
)
