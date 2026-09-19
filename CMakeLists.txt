cmake_minimum_required(VERSION 3.24)

project(Spectiary VERSION 0.10.0 LANGUAGES CXX RC)
include("${CMAKE_SOURCE_DIR}/cmake/project_identity.cmake")

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(SPECTIARY_EMBED_PUBLIC_SPECTRAL_LINES "Embed the public spectral line catalog in the executable." OFF)
option(
    SPECTIARY_BUILD_ASDF_LABELING_SPIKE
    "Build the ASDF labeling research spike."
    OFF
)
option(
    SPECTIARY_BUILD_ASDF_LABELING_HARDENING
    "Build explicit production ASDF labeling hardening tools."
    OFF
)
set(SPECTIARY_BUILD_SOURCE_MODE "" CACHE STRING "Spectiary build source mode.")
set_property(CACHE SPECTIARY_BUILD_SOURCE_MODE PROPERTY STRINGS working_tree head)
set(SPECTIARY_BUILD_SOURCE_REVISION "" CACHE STRING "Full Git object ID for head source mode.")

include("${CMAKE_SOURCE_DIR}/cmake/spectiary_build_source.cmake")

find_package(imgui CONFIG REQUIRED)
find_package(implot CONFIG REQUIRED)
find_package(cfitsio CONFIG REQUIRED)
find_package(yaml-cpp CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
find_package(ZLIB REQUIRED)

set(SPECTIARY_BUILD_COMPILER_ID "${CMAKE_CXX_COMPILER_ID}")
set(SPECTIARY_BUILD_COMPILER_VERSION "${CMAKE_CXX_COMPILER_VERSION}")
set(SPECTIARY_BUILD_CMAKE_VERSION "${CMAKE_VERSION}")
set(SPECTIARY_BUILD_GENERATOR "${CMAKE_GENERATOR}")

set(SPECTIARY_BUILD_TARGET_ARCHITECTURE
    "${CMAKE_CXX_COMPILER_ARCHITECTURE_ID}"
)
if(NOT SPECTIARY_BUILD_TARGET_ARCHITECTURE)
    set(SPECTIARY_BUILD_TARGET_ARCHITECTURE "${CMAKE_GENERATOR_PLATFORM}")
endif()
string(TOLOWER
    "${SPECTIARY_BUILD_TARGET_ARCHITECTURE}"
    SPECTIARY_BUILD_TARGET_ARCHITECTURE
)
if(SPECTIARY_BUILD_TARGET_ARCHITECTURE MATCHES "^(amd64|x64|x86_64)$")
    set(SPECTIARY_BUILD_TARGET_ARCHITECTURE "amd64")
elseif(SPECTIARY_BUILD_TARGET_ARCHITECTURE MATCHES "^(win32|i[3-6]86)$")
    set(SPECTIARY_BUILD_TARGET_ARCHITECTURE "x86")
elseif(SPECTIARY_BUILD_TARGET_ARCHITECTURE STREQUAL "aarch64")
    set(SPECTIARY_BUILD_TARGET_ARCHITECTURE "arm64")
endif()

set(SPECTIARY_BUILD_WINDOWS_SDK_VERSION
    "${CMAKE_VS_WINDOWS_TARGET_PLATFORM_VERSION}"
)
if(SPECTIARY_BUILD_WINDOWS_SDK_VERSION)
    set(SPECTIARY_BUILD_WINDOWS_SDK_VERSION_JSON
        "\"${SPECTIARY_BUILD_WINDOWS_SDK_VERSION}\""
    )
    set(SPECTIARY_BUILD_WINDOWS_SDK_VERSION_DISPLAY
        "${SPECTIARY_BUILD_WINDOWS_SDK_VERSION}"
    )
else()
    set(SPECTIARY_BUILD_WINDOWS_SDK_VERSION_JSON "null")
    set(SPECTIARY_BUILD_WINDOWS_SDK_VERSION_DISPLAY "unavailable")
endif()

foreach(spectiary_toolchain_field IN ITEMS
    SPECTIARY_BUILD_COMPILER_ID
    SPECTIARY_BUILD_COMPILER_VERSION
    SPECTIARY_BUILD_CMAKE_VERSION
    SPECTIARY_BUILD_GENERATOR
    SPECTIARY_BUILD_TARGET_ARCHITECTURE
)
    if("${${spectiary_toolchain_field}}" STREQUAL "")
        message(FATAL_ERROR
            "Could not resolve required build-toolchain metadata: "
            "${spectiary_toolchain_field}."
        )
    endif()
endforeach()
message(STATUS
    "Resolved build toolchain: "
    "${SPECTIARY_BUILD_COMPILER_ID} "
    "${SPECTIARY_BUILD_COMPILER_VERSION}, "
    "CMake ${SPECTIARY_BUILD_CMAKE_VERSION}, "
    "${SPECTIARY_BUILD_GENERATOR}, "
    "${SPECTIARY_BUILD_TARGET_ARCHITECTURE}, "
    "Windows SDK ${SPECTIARY_BUILD_WINDOWS_SDK_VERSION_DISPLAY}"
)

set(SPECTIARY_MANIFEST_TEMPLATE
    "${CMAKE_SOURCE_DIR}/src/platform/spectiary.exe.manifest.in"
)
set(SPECTIARY_MANIFEST
    "${CMAKE_BINARY_DIR}/generated/spectiary/spectiary.exe.manifest"
)
set(SPECTIARY_PUBLIC_SPECTRAL_LINES_TSV "${CMAKE_SOURCE_DIR}/config/spectral_lines.public.tsv")
set(SPECTIARY_THIRD_PARTY_NOTICES "${CMAKE_SOURCE_DIR}/legal/THIRD_PARTY_NOTICES.txt")
set(SPECTIARY_LEGAL_DOCUMENTS
    "${SPECTIARY_THIRD_PARTY_NOTICES}"
    "${CMAKE_SOURCE_DIR}/legal/DATA_SOURCES.txt"
)
set(SPECTIARY_WINDOWS_RESOURCES
    "${CMAKE_BINARY_DIR}/generated/spectiary/spectiary_resources.rc"
)
set(SPECTIARY_RESOURCE_HEADER
    "${CMAKE_SOURCE_DIR}/src/platform/spectiary_resource.h"
)
set(SPECTIARY_APPLICATION_ICON
    "${CMAKE_SOURCE_DIR}/resources/branding/spectiary.ico"
)

include("${CMAKE_SOURCE_DIR}/cmake/third_party_versions.cmake")
spectiary_read_vcpkg_package_version(imgui SPECTIARY_IMGUI_VERSION)
spectiary_read_vcpkg_package_version(implot SPECTIARY_IMPLOT_VERSION)
spectiary_read_vcpkg_package_version(cfitsio SPECTIARY_CFITSIO_VERSION)
spectiary_read_vcpkg_package_version(pthreads SPECTIARY_PTHREADS_VERSION)
spectiary_read_vcpkg_package_version(yaml-cpp SPECTIARY_YAML_CPP_VERSION)
spectiary_read_vcpkg_package_version(nlohmann-json SPECTIARY_NLOHMANN_JSON_VERSION)
spectiary_require_notice_heading(
    "${SPECTIARY_THIRD_PARTY_NOTICES}"
    "nlohmann/json"
    "nlohmann/json ${SPECTIARY_NLOHMANN_JSON_VERSION}"
)
spectiary_read_vcpkg_package_version(zlib SPECTIARY_ZLIB_VERSION)
spectiary_require_notice_heading(
    "${SPECTIARY_THIRD_PARTY_NOTICES}"
    "Dear ImGui"
    "Dear ImGui ${SPECTIARY_IMGUI_VERSION}, including docking, Win32, and DirectX 11 backends"
)
spectiary_require_notice_heading(
    "${SPECTIARY_THIRD_PARTY_NOTICES}"
    "ImPlot"
    "ImPlot ${SPECTIARY_IMPLOT_VERSION}"
)
spectiary_require_notice_heading(
    "${SPECTIARY_THIRD_PARTY_NOTICES}"
    "CFITSIO"
    "CFITSIO ${SPECTIARY_CFITSIO_VERSION}"
)
spectiary_require_notice_heading(
    "${SPECTIARY_THIRD_PARTY_NOTICES}"
    "PThreads4W"
    "PThreads4W ${SPECTIARY_PTHREADS_VERSION}"
)
spectiary_require_notice_heading(
    "${SPECTIARY_THIRD_PARTY_NOTICES}"
    "yaml-cpp"
    "yaml-cpp ${SPECTIARY_YAML_CPP_VERSION}"
)
spectiary_require_notice_heading(
    "${SPECTIARY_THIRD_PARTY_NOTICES}"
    "zlib"
    "zlib ${SPECTIARY_ZLIB_VERSION}"
)
message(STATUS
    "Resolved third-party versions: "
    "Dear ImGui ${SPECTIARY_IMGUI_VERSION}, "
    "ImPlot ${SPECTIARY_IMPLOT_VERSION}, "
    "CFITSIO ${SPECTIARY_CFITSIO_VERSION}, "
    "PThreads4W ${SPECTIARY_PTHREADS_VERSION}, "
    "yaml-cpp ${SPECTIARY_YAML_CPP_VERSION}, "
    "zlib ${SPECTIARY_ZLIB_VERSION}"
)

file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/generated/spectiary")
configure_file(
    "${SPECTIARY_MANIFEST_TEMPLATE}"
    "${SPECTIARY_MANIFEST}"
    @ONLY
)
configure_file(
    "${CMAKE_SOURCE_DIR}/src/platform/spectiary_resources.rc.in"
    "${SPECTIARY_WINDOWS_RESOURCES}"
    @ONLY
)
set_source_files_properties(
    "${SPECTIARY_WINDOWS_RESOURCES}"
    PROPERTIES
        OBJECT_DEPENDS
            "${SPECTIARY_LEGAL_DOCUMENTS};${SPECTIARY_RESOURCE_HEADER};${SPECTIARY_APPLICATION_ICON}"
)
configure_file(
    "${CMAKE_SOURCE_DIR}/cmake/spectiary_build_identity.h.in"
    "${CMAKE_BINARY_DIR}/generated/spectiary/spectiary_build_identity.h.in"
    @ONLY
)
file(GENERATE
    OUTPUT
        "${CMAKE_BINARY_DIR}/generated/$<CONFIG>/spectiary/spectiary_build_identity.h"
    INPUT
        "${CMAKE_BINARY_DIR}/generated/spectiary/spectiary_build_identity.h.in"
)
if(SPECTIARY_EMBED_PUBLIC_SPECTRAL_LINES)
    set(SPECTIARY_EMBEDDED_PUBLIC_SPECTRAL_LINES_HEADER
        "${CMAKE_BINARY_DIR}/generated/spectiary/public_spectral_lines_embedded.h"
    )
    add_custom_command(
        OUTPUT "${SPECTIARY_EMBEDDED_PUBLIC_SPECTRAL_LINES_HEADER}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${CMAKE_BINARY_DIR}/generated/spectiary"
        COMMAND
            "${CMAKE_COMMAND}"
            "-DINPUT=${SPECTIARY_PUBLIC_SPECTRAL_LINES_TSV}"
            "-DOUTPUT=${SPECTIARY_EMBEDDED_PUBLIC_SPECTRAL_LINES_HEADER}"
            -P "${CMAKE_SOURCE_DIR}/cmake/embed_public_spectral_lines.cmake"
        DEPENDS
            "${SPECTIARY_PUBLIC_SPECTRAL_LINES_TSV}"
            "${CMAKE_SOURCE_DIR}/cmake/embed_public_spectral_lines.cmake"
        VERBATIM
    )
    add_custom_target(
        spectiary_embedded_public_spectral_lines
        DEPENDS "${SPECTIARY_EMBEDDED_PUBLIC_SPECTRAL_LINES_HEADER}"
    )

    add_library(spectiary_embedded_resources INTERFACE)
    target_include_directories(spectiary_embedded_resources INTERFACE "${CMAKE_BINARY_DIR}/generated")
    target_compile_definitions(spectiary_embedded_resources INTERFACE SPECTIARY_EMBED_PUBLIC_SPECTRAL_LINES)
endif()

function(spectiary_use_embedded_resources target_name)
    if(SPECTIARY_EMBED_PUBLIC_SPECTRAL_LINES)
        target_link_libraries(${target_name} PRIVATE spectiary_embedded_resources)
        add_dependencies(${target_name} spectiary_embedded_public_spectral_lines)
    endif()
endfunction()

function(spectiary_use_runtime_paths target_name)
    if(WIN32)
        target_link_libraries(${target_name} PRIVATE ole32 shell32)
    endif()
endfunction()

function(spectiary_enable_warnings target_name)
    if(MSVC)
        target_compile_options(${target_name} PRIVATE /EHsc /W4 /permissive- /utf-8)
    else()
        target_compile_options(${target_name} PRIVATE -Wall -Wextra -Wpedantic)
    endif()
endfunction()

function(spectiary_configure_production_target target_name)
    target_include_directories(${target_name} PUBLIC "${CMAKE_BINARY_DIR}/generated")
    target_include_directories(${target_name} PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
    target_compile_definitions(${target_name} PRIVATE
        NOMINMAX
        UNICODE
        _UNICODE
        WIN32_LEAN_AND_MEAN
    )
    spectiary_enable_warnings(${target_name})
endfunction()

add_library(spectiary_core STATIC
    src/app/embedded_legal_documents.cpp
    src/app/local_user_state.cpp
    src/app/local_user_state_json.cpp
    src/app/on_demand_frame_capture.cpp
    src/app/pan_pacing.cpp
    src/app/render_wake_scheduler.cpp
    src/app/runtime_paths.cpp
    src/app/spectiary_metadata.cpp
    src/app/spectiary_metadata_finalizer.cpp
    src/app/spectiary_metadata_validation.cpp
    src/domain/canonical_timestamp.cpp
    src/domain/csv_record_codec.cpp
    src/domain/fits_file_reader.cpp
    src/domain/fits_spectrum_loader.cpp
    src/domain/npy_array_io.cpp
    src/domain/sample_annotation_io.cpp
    src/domain/sample_filter.cpp
    src/domain/sample_label_export.cpp
    src/domain/sample_labeling_asdf_codec.cpp
    src/domain/sample_labeling_asdf_store.cpp
    src/domain/sample_labeling.cpp
    src/domain/sample_labeling_document.cpp
    src/domain/sample_labeling_source_compatibility.cpp
    src/domain/source_collection_identity_digest.cpp
    src/domain/source_collection_manifest.cpp
    src/domain/source_open_resolution.cpp
    src/domain/source_path_identity.cpp
    src/domain/stable_sha256.cpp
    src/domain/spectrum_fixture.cpp
    src/domain/spectrum_format_adapters.cpp
    src/domain/spectrum_loader_support.cpp
    src/domain/spectrum_loader.cpp
    src/domain/spectrum_smoothing.cpp
    src/domain/utf8.cpp
    src/domain/uuid_v4.cpp
    src/overlays/spectral_line_catalog.cpp
    src/overlays/spectral_line_list.cpp
    src/overlays/spectral_line_list_json.cpp
    src/overlays/built_in_spectral_line_overlay.cpp
    src/overlays/built_in_spectral_line_state_io.cpp
    src/overlays/built_in_spectral_line_reconciliation.cpp
    src/overlays/spectral_line_session_state.cpp
    src/overlays/catalog_user_state_reconciliation.cpp
    src/overlays/spectral_line_user_state_cache_io.cpp
    src/overlays/spectral_line_user_state.cpp
    src/platform/atomic_file.cpp
    src/platform/exclusive_file_lease.cpp
    src/platform/file_sha256.cpp
    src/platform/secure_random_bytes.cpp
    src/platform/win32_directory_change_generation.cpp
    src/platform/win32_compositor_clock.cpp
    src/platform/win32_message_render_observer.cpp
    src/platform/win32_message_wait.cpp
    src/platform/win32_process_launcher.cpp
    src/profile/load_latency_trace_lifecycle.cpp
    src/profile/navigation_latency_trace.cpp
    src/profile/navigation_prefetch_trace.cpp
    src/profile/profile_recording_status.cpp
    src/profile/profile_settings.cpp
    src/profile/profile_sink.cpp
    src/profile/source_load_latency_trace.cpp
)
spectiary_configure_production_target(spectiary_core)
target_include_directories(spectiary_core PRIVATE
    "${CMAKE_BINARY_DIR}/generated/$<CONFIG>"
)
spectiary_use_runtime_paths(spectiary_core)
spectiary_use_embedded_resources(spectiary_core)
target_link_libraries(spectiary_core PRIVATE yaml-cpp::yaml-cpp CFITSIO::cfitsio ZLIB::ZLIB bcrypt)
target_link_libraries(spectiary_core PUBLIC nlohmann_json::nlohmann_json)
if(MSVC)
    # Cancellation-stage tests resolve optimized and inlined source locations
    # at runtime, so retain line tables for the production code they exercise.
    set_property(
        SOURCE
            src/domain/fits_file_reader.cpp
            src/domain/fits_spectrum_loader.cpp
            src/domain/spectrum_loader_support.cpp
        APPEND
        PROPERTY COMPILE_OPTIONS
            "$<$<CONFIG:Release>:/Z7>"
            "$<$<CONFIG:Release>:/Zo>"
    )
endif()

add_library(spectiary_imgui_layout STATIC
    src/app/imgui_layout_persistence.cpp
)
spectiary_configure_production_target(spectiary_imgui_layout)
target_link_libraries(
    spectiary_imgui_layout
    PUBLIC
        spectiary_core
        imgui::imgui
)

add_library(spectiary_automation STATIC
    src/automation/automation_named_pipe.cpp
    src/automation/automation_protocol.cpp
    src/automation/automation_startup.cpp
    src/automation/automation_state.cpp
)
spectiary_configure_production_target(spectiary_automation)
target_link_libraries(spectiary_automation
    PUBLIC
        spectiary_core
    PRIVATE
        advapi32
        ntdll
        shell32
)

add_library(spectiary_sessions STATIC
    src/ui/panel_visibility_state_cache_io.cpp
    src/ui/sample_annotation_labeling_rules.cpp
    src/ui/sample_labeling_controller.cpp
    src/ui/sample_labeling_state_cache_io.cpp
    src/ui/sample_labeling_persistence_owners.cpp
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
spectiary_configure_production_target(spectiary_sessions)
target_link_libraries(spectiary_sessions PUBLIC spectiary_core)

add_library(spectiary_desktop_ui STATIC
    src/app/application_settings.cpp
    src/app/initial_source.cpp
    src/app/native_window_title.cpp
    src/plot/plot_touchpad_gesture.cpp
    src/plot/scientific_label.cpp
    src/plot/series_color.cpp
    src/plot/spectral_line_label_layout.cpp
    src/plot/spectrum_plot.cpp
    src/ui/immersive_context_overlay.cpp
    src/ui/panel_session_interaction.cpp
    src/ui/sample_label_presentation.cpp
    src/ui/sample_labeling_filename.cpp
    src/ui/sample_workflow_shortcut.cpp
    src/ui/sample_workflow_panel.cpp
    src/ui/settings_panel.cpp
    src/ui/shell_ui.cpp
    src/ui/source_collection_panel.cpp
    src/ui/spectral_lines_grouping_view.cpp
    src/ui/spectral_lines_name_localization.cpp
    src/ui/spectral_lines_panel.cpp
    src/ui/spectrum_plot_preferences_io.cpp
    src/ui/spectrum_viewport_state_io.cpp
    src/ui/legacy_spectrum_view_state_io.cpp
    src/ui/spectrum_persistence_migration.cpp
    src/ui/spectrum_view_session.cpp
    src/ui/theme.cpp
    src/ui/ui_font.cpp
    src/ui/ui_scale_settings.cpp
    src/ui/ui_text.cpp
)
spectiary_configure_production_target(spectiary_desktop_ui)
target_include_directories(spectiary_desktop_ui BEFORE PRIVATE
    "${CMAKE_BINARY_DIR}/generated/$<CONFIG>"
)
target_link_libraries(spectiary_desktop_ui
    PUBLIC
        spectiary_sessions
        imgui::imgui
        implot::implot
    PRIVATE
        shell32
)

add_library(spectiary_renderer STATIC
    src/renderer/d3d11_composition_swap_chain.cpp
    src/renderer/d3d11_frame_capture.cpp
    src/renderer/d3d11_imgui_viewport_renderer.cpp
    src/renderer/d3d11_renderer.cpp
    src/renderer/d3d11_sdr_swap_chain.cpp
    src/renderer/d3d11_window_presentation.cpp
    src/renderer/win32_display_refresh.cpp
)
spectiary_configure_production_target(spectiary_renderer)
target_link_libraries(spectiary_renderer
    PRIVATE
        imgui::imgui
        dcomp
        d3d11
        dxgi
        ntdll
        ole32
        windowscodecs
)

add_executable(spectiary_native WIN32
    src/app/automation_execution.cpp
    src/app/automation_panel_command_coordinator.cpp
    src/app/runtime_resource_workload.cpp
    src/app/spectiary_app.cpp
    src/main.cpp
    src/platform/win32_application_icon.cpp
    src/platform/win32_system_theme.cpp
    src/platform/win32_touchpad_gesture_source.cpp
    src/platform/win32_window.cpp
    ${SPECTIARY_MANIFEST}
    ${SPECTIARY_WINDOWS_RESOURCES}
)
spectiary_configure_production_target(spectiary_native)
target_link_libraries(spectiary_native PRIVATE
    spectiary_automation
    spectiary_desktop_ui
    spectiary_imgui_layout
    spectiary_renderer
    advapi32
    dwmapi
    comctl32
    imm32
    ole32
    uuid
)
if(MSVC)
    target_link_options(
        spectiary_native
        PRIVATE
            /Brepro
            /INCREMENTAL:NO
            "$<$<CONFIG:Debug>:/DEBUG:FASTLINK>"
    )
endif()

add_executable(spectiary_metadata_finalizer_tool
    src/app/spectiary_metadata_finalizer_main.cpp
)
spectiary_configure_production_target(spectiary_metadata_finalizer_tool)
target_include_directories(spectiary_metadata_finalizer_tool PRIVATE
    "${CMAKE_BINARY_DIR}/generated/$<CONFIG>"
)
target_link_libraries(spectiary_metadata_finalizer_tool PRIVATE
    spectiary_core
)
add_dependencies(spectiary_native spectiary_metadata_finalizer_tool)

set_target_properties(spectiary_native PROPERTIES
    OUTPUT_NAME "${PROJECT_artifact_basename}"
    VS_DEBUGGER_WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)

add_custom_command(TARGET spectiary_native PRE_LINK
    COMMAND "${CMAKE_COMMAND}" -E rm -f
        "$<TARGET_FILE_DIR:spectiary_native>/${PROJECT_metadata_filename}"
    COMMENT "Invalidating stale Spectiary metadata before linking"
    VERBATIM
)

if(NOT SPECTIARY_EMBED_PUBLIC_SPECTRAL_LINES)
    add_custom_command(TARGET spectiary_native POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E make_directory "$<TARGET_FILE_DIR:spectiary_native>/config"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${SPECTIARY_PUBLIC_SPECTRAL_LINES_TSV}"
            "$<TARGET_FILE_DIR:spectiary_native>/config/spectral_lines.public.tsv"
    )
endif()

add_executable(spectiary_automation_launcher
    src/automation/automation_launcher_main.cpp
)
spectiary_configure_production_target(
    spectiary_automation_launcher
)
target_link_libraries(
    spectiary_automation_launcher
    PRIVATE
        spectiary_automation
        spectiary_imgui_layout
        spectiary_sessions
        bcrypt
)
set_target_properties(
    spectiary_automation_launcher
    PROPERTIES
        OUTPUT_NAME "${PROJECT_artifact_basename}Automation"
        VS_DEBUGGER_WORKING_DIRECTORY
            "${CMAKE_SOURCE_DIR}"
)
add_executable(spectiary_automation_launcher_timeout_fixture
    tests/automation_launcher_timeout_fixture.cpp
)
spectiary_configure_production_target(
    spectiary_automation_launcher_timeout_fixture
)
target_link_libraries(
    spectiary_automation_launcher_timeout_fixture
    PRIVATE
        spectiary_automation
)
add_executable(spectiary_win32_window_peer
    tests/win32_window_peer_main.cpp
)
spectiary_configure_production_target(
    spectiary_win32_window_peer
)
target_link_libraries(
    spectiary_win32_window_peer
    PRIVATE
        user32
)
add_custom_command(TARGET spectiary_native POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E rm -rf
        "$<TARGET_FILE_DIR:spectiary_native>/Legal"
    COMMAND "${CMAKE_COMMAND}" -E rm -f
        "$<TARGET_FILE_DIR:spectiary_native>/EULA.txt"
        "$<TARGET_FILE_DIR:spectiary_native>/THIRD_PARTY_NOTICES.txt"
        "$<TARGET_FILE_DIR:spectiary_native>/DATA_SOURCES.txt"
    COMMENT "Removing obsolete executable-adjacent legal documents"
    VERBATIM
)
add_custom_command(TARGET spectiary_native POST_BUILD
    COMMAND "$<TARGET_FILE:spectiary_metadata_finalizer_tool>"
        "$<TARGET_FILE:spectiary_native>"
        "$<TARGET_FILE_DIR:spectiary_native>/${PROJECT_metadata_filename}"
    COMMENT "Finalizing schema 6 Spectiary metadata"
    VERBATIM
)
set(SPECTIARY_METADATA_BYPRODUCT
    "${CMAKE_CURRENT_BINARY_DIR}/${CMAKE_CFG_INTDIR}/${PROJECT_metadata_filename}")
add_custom_target(spectiary_metadata
    COMMAND "${CMAKE_COMMAND}"
        "-DSPECTIARY_EXECUTABLE=$<TARGET_FILE:spectiary_native>"
        "-DSPECTIARY_METADATA=$<TARGET_FILE_DIR:spectiary_native>/${PROJECT_metadata_filename}"
        "-DSPECTIARY_FINALIZER=$<TARGET_FILE:spectiary_metadata_finalizer_tool>"
        -P "${CMAKE_SOURCE_DIR}/cmake/ensure_spectiary_metadata.cmake"
    DEPENDS spectiary_native
        spectiary_metadata_finalizer_tool
        "${CMAKE_SOURCE_DIR}/cmake/ensure_spectiary_metadata.cmake"
    BYPRODUCTS
        "${SPECTIARY_METADATA_BYPRODUCT}"
    COMMENT "Building and finalizing Spectiary metadata"
    VERBATIM
)

enable_testing()
option(SPECTIARY_BUILD_WIDGET_TESTS "Build the isolated ImGui widget regression harness" OFF)

function(spectiary_set_test_tier test_name tier)
    if(NOT tier STREQUAL "fast" AND NOT tier STREQUAL "extended")
        message(FATAL_ERROR "Unknown CTest tier '${tier}' for ${test_name}")
    endif()
    set_property(TEST "${test_name}" APPEND PROPERTY LABELS "${tier}")
endfunction()

if(SPECTIARY_BUILD_ASDF_LABELING_SPIKE)
    add_subdirectory(tools/asdf_labeling_spike)
endif()
if(SPECTIARY_BUILD_ASDF_LABELING_HARDENING)
    add_subdirectory(tools/asdf_labeling_production_benchmark)
endif()

set(SPECTIARY_PORTABLE_TEST_RUNTIME_DIRECTORY
    "${CMAKE_BINARY_DIR}/portable-tests/$<CONFIG>"
)
set(SPECTIARY_PORTABLE_TEST_METADATA
    "${SPECTIARY_PORTABLE_TEST_RUNTIME_DIRECTORY}/${PROJECT_metadata_filename}"
)
add_custom_target(spectiary_portable_test_metadata
    COMMAND "${CMAKE_COMMAND}" -E make_directory
        "${SPECTIARY_PORTABLE_TEST_RUNTIME_DIRECTORY}"
    COMMAND "${CMAKE_COMMAND}"
        "-DINPUT=$<TARGET_FILE_DIR:spectiary_native>/${PROJECT_metadata_filename}"
        "-DOUTPUT=${SPECTIARY_PORTABLE_TEST_METADATA}"
        -P "${CMAKE_SOURCE_DIR}/tests/fixtures/create_portable_runtime_metadata.cmake"
    DEPENDS
        spectiary_metadata
        "${CMAKE_SOURCE_DIR}/tests/fixtures/create_portable_runtime_metadata.cmake"
    COMMENT "Preparing schema 6 Portable runtime metadata for deterministic tests"
    VERBATIM
)

function(spectiary_add_test target_name source_file production_target)
    add_executable(${target_name} ${source_file})
    target_link_libraries(${target_name} PRIVATE ${production_target})
    spectiary_enable_warnings(${target_name})
    add_test(NAME ${target_name} COMMAND ${target_name})
endfunction()

function(spectiary_add_portable_only_test target_name source_file production_target)
    add_executable(${target_name} ${source_file})
    target_link_libraries(${target_name} PRIVATE ${production_target})
    spectiary_enable_warnings(${target_name})
    set(portable_fixture_target
        "${target_name}_portable_fixture"
    )
    set(portable_executable
        "${SPECTIARY_PORTABLE_TEST_RUNTIME_DIRECTORY}/$<TARGET_FILE_NAME:${target_name}>"
    )
    add_custom_target(${portable_fixture_target} ALL
        COMMAND "${CMAKE_COMMAND}" -E make_directory
            "${SPECTIARY_PORTABLE_TEST_RUNTIME_DIRECTORY}"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "$<TARGET_FILE:${target_name}>"
            "${portable_executable}"
        DEPENDS ${target_name}
        COMMENT "Preparing Portable runtime fixture for ${target_name}"
        VERBATIM
    )
    add_dependencies(
        ${portable_fixture_target}
        spectiary_portable_test_metadata
    )
    add_test(
        NAME "${target_name}"
        COMMAND "${portable_executable}"
    )
    set_tests_properties(
        "${target_name}"
        PROPERTIES
            ENVIRONMENT_MODIFICATION
                "PATH=path_list_prepend:$<TARGET_FILE_DIR:${target_name}>"
            WORKING_DIRECTORY "$<TARGET_FILE_DIR:${target_name}>"
    )
endfunction()

spectiary_add_test(
    spectiary_sdr_swap_chain_tests
    tests/d3d11_sdr_swap_chain_tests.cpp
    spectiary_renderer
)
target_link_libraries(spectiary_sdr_swap_chain_tests PRIVATE imgui::imgui)

add_executable(spectiary_win32_application_icon_tests
    tests/win32_application_icon_tests.cpp
    src/platform/win32_application_icon.cpp
    src/platform/win32_window.cpp
    ${SPECTIARY_WINDOWS_RESOURCES}
)
spectiary_configure_production_target(
    spectiary_win32_application_icon_tests
)
target_link_libraries(
    spectiary_win32_application_icon_tests
    PRIVATE
        imgui::imgui
        imm32
)
add_test(
    NAME spectiary_win32_application_icon_tests
    COMMAND spectiary_win32_application_icon_tests
)

spectiary_add_test(
    spectiary_local_user_state_tests
    tests/local_user_state_tests.cpp
    spectiary_sessions
)
spectiary_add_test(
    spectiary_sample_labeling_persistence_owner_tests
    tests/sample_labeling_persistence_owner_tests.cpp
    spectiary_sessions
)
spectiary_add_portable_only_test(
    spectiary_portable_runtime_state_tests
    tests/portable_runtime_state_tests.cpp
    spectiary_sessions
)
spectiary_add_test(
    spectiary_runtime_storage_context_tests
    tests/runtime_storage_context_tests.cpp
    spectiary_desktop_ui
)
spectiary_add_test(
    spectiary_render_wake_scheduler_tests
    tests/render_wake_scheduler_tests.cpp
    spectiary_core
)
target_link_libraries(spectiary_render_wake_scheduler_tests PRIVATE imgui::imgui)
spectiary_add_test(
    spectiary_imgui_layout_persistence_tests
    tests/imgui_layout_persistence_tests.cpp
    spectiary_imgui_layout
)
target_compile_definitions(
    spectiary_imgui_layout_persistence_tests
    PRIVATE
        SPECTIARY_VIEWPORT_OWNERSHIP_LAYOUT_FIXTURE="${CMAKE_SOURCE_DIR}/tests/fixtures/imgui/no-auto-merge-floating.ini"
)
spectiary_add_test(
    spectiary_automation_control_tests
    tests/automation_control_tests.cpp
    spectiary_automation
)
target_link_libraries(
    spectiary_automation_control_tests
    PRIVATE
        advapi32
        spectiary_desktop_ui
)
set_tests_properties(
    spectiary_automation_control_tests
    PROPERTIES
        LABELS "automation;automation-headless;ci-headless;required"
)
spectiary_add_test(
    spectiary_automation_panel_command_coordinator_tests
    tests/automation_panel_command_coordinator_tests.cpp
    spectiary_automation
)
target_sources(
    spectiary_automation_panel_command_coordinator_tests
    PRIVATE
        src/app/automation_panel_command_coordinator.cpp
)
target_link_libraries(
    spectiary_automation_panel_command_coordinator_tests
    PRIVATE
        spectiary_desktop_ui
)
set_tests_properties(
    spectiary_automation_panel_command_coordinator_tests
    PROPERTIES
        LABELS "automation;automation-headless;ci-headless;required"
)
spectiary_add_test(
    spectiary_automation_state_isolation_tests
    tests/automation_state_isolation_tests.cpp
    spectiary_desktop_ui
)
target_link_libraries(
    spectiary_automation_state_isolation_tests
    PRIVATE
        dwmapi
)
add_dependencies(
    spectiary_automation_state_isolation_tests
    spectiary_metadata
)
set_tests_properties(
    spectiary_automation_state_isolation_tests
    PROPERTIES
        LABELS "automation;automation-headless;ci-headless;required"
)
add_test(
    NAME spectiary_automation_samples_contract_tests
    COMMAND powershell -NoProfile -ExecutionPolicy Bypass
        -File "${CMAKE_SOURCE_DIR}/tests/automation_samples_contract_tests.ps1"
        -FixtureDirectory
            "${CMAKE_SOURCE_DIR}/tests/fixtures/automation/samples"
        -Documentation
            "${CMAKE_SOURCE_DIR}/docs/development/automation/automation_samples.md"
        -Runner
            "${CMAKE_SOURCE_DIR}/tests/automation_samples_tests.ps1"
        -CMakeLists
            "${CMAKE_SOURCE_DIR}/CMakeLists.txt"
)
set_tests_properties(
    spectiary_automation_samples_contract_tests
    PROPERTIES
        LABELS "automation;automation-headless;ci-headless;required"
        TIMEOUT 30
)
add_test(
    NAME spectiary_automation_ci_contract_tests
    COMMAND powershell -NoProfile -ExecutionPolicy Bypass
        -File "${CMAKE_SOURCE_DIR}/tests/automation_ci_contract_tests.ps1"
        -Workflow
            "${CMAKE_SOURCE_DIR}/.github/workflows/automation.yml"
        -CMakeLists
            "${CMAKE_SOURCE_DIR}/CMakeLists.txt"
        -RunnerScript
            "${CMAKE_SOURCE_DIR}/scripts/run-automation-ci.ps1"
)
set_tests_properties(
    spectiary_automation_ci_contract_tests
    PROPERTIES
        LABELS "automation;automation-headless;ci-headless;required"
        TIMEOUT 30
)
add_test(
    NAME spectiary_repository_verification_ci_contract_tests
    COMMAND powershell -NoProfile -ExecutionPolicy Bypass
        -File
            "${CMAKE_SOURCE_DIR}/tests/repository_verification_ci_contract_tests.ps1"
        -Workflow
            "${CMAKE_SOURCE_DIR}/.github/workflows/repository-verification.yml"
        -AutomationWorkflow
            "${CMAKE_SOURCE_DIR}/.github/workflows/automation.yml"
        -PresetsFile
            "${CMAKE_SOURCE_DIR}/CMakePresets.json"
        -RootCMakeLists
            "${CMAKE_SOURCE_DIR}/CMakeLists.txt"
        -AsdfCMakeLists
            "${CMAKE_SOURCE_DIR}/tools/asdf_labeling_spike/CMakeLists.txt"
)
set_tests_properties(
    spectiary_repository_verification_ci_contract_tests
    PROPERTIES
        TIMEOUT 30
)
add_test(
    NAME spectiary_release_artifact_process_runner_tests
    COMMAND powershell -NoProfile -ExecutionPolicy Bypass
        -File
            "${CMAKE_SOURCE_DIR}/tests/release_artifact_process_runner_tests.ps1"
        -RepoRoot "${CMAKE_SOURCE_DIR}"
)
set_tests_properties(
    spectiary_release_artifact_process_runner_tests
    PROPERTIES
        LABELS "ci-headless;required"
        TIMEOUT 30
)
add_test(
    NAME spectiary_automation_launcher_integration_tests
    COMMAND powershell -NoProfile -ExecutionPolicy Bypass
        -File "${CMAKE_SOURCE_DIR}/tests/automation_launcher_integration_tests.ps1"
        -Launcher "$<TARGET_FILE:spectiary_automation_launcher>"
        -Executable "$<TARGET_FILE:spectiary_native>"
        -CleanupFixture "$<TARGET_FILE:spectiary_automation_control_tests>"
        -TimeoutFixture "$<TARGET_FILE:spectiary_automation_launcher_timeout_fixture>"
        -StateFixture "$<TARGET_FILE:spectiary_automation_state_isolation_tests>"
)
set_tests_properties(
    spectiary_automation_launcher_integration_tests
    PROPERTIES
        RUN_SERIAL TRUE
        # This test includes the deterministic fake-GUI deadline fixtures as
        # well as the real-GUI launcher workflow; retain margin over the
        # current ~94-second run on the managed desktop.
        TIMEOUT 180
        LABELS "automation;real-gui"
)
add_test(
    NAME spectiary_imgui_viewport_ownership_integration_tests
    COMMAND powershell -NoProfile -ExecutionPolicy Bypass
        -File "${CMAKE_SOURCE_DIR}/tests/imgui_viewport_ownership_integration_tests.ps1"
        -Launcher "$<TARGET_FILE:spectiary_automation_launcher>"
        -Executable "$<TARGET_FILE:spectiary_native>"
        -WindowPeer "$<TARGET_FILE:spectiary_win32_window_peer>"
        -LayoutSeed
            "${CMAKE_SOURCE_DIR}/tests/fixtures/imgui/no-auto-merge-floating.ini"
        -ArtifactsDirectory
            "${CMAKE_BINARY_DIR}/test-artifacts/theme-smoke"
)
set_tests_properties(
    spectiary_imgui_viewport_ownership_integration_tests
    PROPERTIES
        RUN_SERIAL TRUE
        TIMEOUT 90
        LABELS "automation;gui-integration"
)
add_test(
    NAME spectiary_automation_samples_tests
    COMMAND powershell -NoProfile -ExecutionPolicy Bypass
        -File "${CMAKE_SOURCE_DIR}/tests/automation_samples_tests.ps1"
        -Launcher "$<TARGET_FILE:spectiary_automation_launcher>"
        -Executable "$<TARGET_FILE:spectiary_native>"
        -StateFixture "$<TARGET_FILE:spectiary_automation_state_isolation_tests>"
        -FixtureDirectory
            "${CMAKE_SOURCE_DIR}/tests/fixtures/automation/samples"
        -ArtifactsDirectory
            "${CMAKE_BINARY_DIR}/test-artifacts/automation-samples"
)
set_tests_properties(
    spectiary_automation_samples_tests
    PROPERTIES
        RUN_SERIAL TRUE
        LABELS "automation;real-gui"
        # Two 60s GUI sequence budgets, two 30s helper budgets, bounded
        # launcher/output/process cleanup, and evidence preservation must all
        # complete before CTest terminates the runner's finally block.
        TIMEOUT 300
)
add_test(
    NAME spectiary_multi_instance_labeling_smoke_tests
    COMMAND powershell -NoProfile -ExecutionPolicy Bypass
        -File "${CMAKE_SOURCE_DIR}/tests/multi_instance_labeling_smoke_tests.ps1"
        -Executable "$<TARGET_FILE:spectiary_native>"
        -StateFixture "$<TARGET_FILE:spectiary_automation_state_isolation_tests>"
)
set_tests_properties(
    spectiary_multi_instance_labeling_smoke_tests
    PROPERTIES
        RUN_SERIAL TRUE
        # Runner main flow is capped at 180s; 30s shared GUI cleanup, two
        # 1s GUI Kill waits, and a shared 10s helper/fallback fixture and
        # alias-directory cleanup budget stay below 240s.
        TIMEOUT 240
        LABELS "automation;real-gui"
)

spectiary_add_test(
    spectiary_automation_execution_tests
    tests/automation_execution_tests.cpp
    spectiary_automation
)
target_sources(spectiary_automation_execution_tests PRIVATE src/app/automation_execution.cpp)
target_link_libraries(spectiary_automation_execution_tests PRIVATE spectiary_desktop_ui)
set_tests_properties(spectiary_automation_execution_tests PROPERTIES
    LABELS "automation;automation-headless;ci-headless;required")

add_custom_target(spectiary_automation_headless_targets)
add_dependencies(
    spectiary_automation_headless_targets
    spectiary_automation_control_tests
    spectiary_automation_execution_tests
    spectiary_automation_panel_command_coordinator_tests
    spectiary_automation_state_isolation_tests
)

add_custom_target(spectiary_automation_real_gui_targets)
add_dependencies(
    spectiary_automation_real_gui_targets
    spectiary_automation_launcher
    spectiary_native
    spectiary_automation_control_tests
    spectiary_automation_launcher_timeout_fixture
    spectiary_automation_state_isolation_tests
)
spectiary_add_test(
    spectiary_on_demand_frame_capture_tests
    tests/on_demand_frame_capture_tests.cpp
    spectiary_core
)
spectiary_add_test(
    spectiary_profile_recording_ui_state_tests
    tests/profile_recording_ui_state_tests.cpp
    spectiary_core
)
spectiary_add_test(
    spectiary_top_bar_status_layout_tests
    tests/top_bar_status_layout_tests.cpp
    spectiary_core
)
spectiary_add_test(
    spectiary_top_bar_status_hover_tests
    tests/top_bar_status_hover_tests.cpp
    spectiary_core
)
target_link_libraries(spectiary_top_bar_status_hover_tests PRIVATE imgui::imgui)
spectiary_add_test(
    spectiary_profile_sink_tests
    tests/profile_sink_tests.cpp
    spectiary_core
)
spectiary_add_test(
    spectiary_presentation_trace_tests
    tests/presentation_trace_tests.cpp
    spectiary_core
)
spectiary_add_test(
    spectiary_profile_settings_tests
    tests/profile_settings_tests.cpp
    spectiary_core
)
spectiary_add_test(
    spectiary_navigation_latency_trace_tests
    tests/navigation_latency_trace_tests.cpp
    spectiary_core
)
spectiary_add_test(
    spectiary_source_load_latency_trace_tests
    tests/source_load_latency_trace_tests.cpp
    spectiary_core
)
spectiary_add_test(
    spectiary_metadata_tests
    tests/spectiary_metadata_tests.cpp
    spectiary_core
)
target_compile_definitions(spectiary_metadata_tests PRIVATE
    SPECTIARY_METADATA_FIXTURE_DIR="${CMAKE_SOURCE_DIR}/tests/fixtures/build_metadata"
    SPECTIARY_EXPECTED_CFITSIO_VERSION="${SPECTIARY_CFITSIO_VERSION}"
)
spectiary_add_test(
    spectiary_metadata_finalizer_tests
    tests/spectiary_metadata_finalizer_tests.cpp
    spectiary_core
)
spectiary_add_test(
    spectiary_atomic_file_tests
    tests/atomic_file_tests.cpp
    spectiary_core
)
spectiary_add_test(
    spectiary_embedded_legal_documents_tests
    tests/embedded_legal_documents_tests.cpp
    spectiary_core
)
target_sources(
    spectiary_embedded_legal_documents_tests
    PRIVATE
        ${SPECTIARY_WINDOWS_RESOURCES}
)
target_compile_definitions(
    spectiary_embedded_legal_documents_tests
    PRIVATE
        SPECTIARY_LEGAL_SOURCE_DIRECTORY="${CMAKE_SOURCE_DIR}/legal"
)
spectiary_add_test(
    spectiary_settings_panel_tests
    tests/settings_panel_tests.cpp
    spectiary_desktop_ui
)
target_sources(
    spectiary_settings_panel_tests
    PRIVATE
        ${SPECTIARY_WINDOWS_RESOURCES}
)
target_compile_definitions(spectiary_settings_panel_tests PRIVATE
    SPECTIARY_EXPECTED_VERSION="${PROJECT_VERSION}"
    SPECTIARY_EXPECTED_CONFIGURATION="$<CONFIG>"
    SPECTIARY_EXPECTED_ARCHITECTURE="${SPECTIARY_BUILD_TARGET_ARCHITECTURE}"
    SPECTIARY_EXPECTED_SOURCE_MODE="${SPECTIARY_BUILD_SOURCE_MODE}"
    SPECTIARY_EXPECTED_SOURCE_REVISION="${SPECTIARY_BUILD_SOURCE_REVISION}"
)
target_link_libraries(spectiary_settings_panel_tests PRIVATE dwmapi)
add_test(
    NAME spectiary_analyze_profile_tests
    COMMAND powershell -NoProfile -ExecutionPolicy Bypass
        -File "${CMAKE_SOURCE_DIR}/tests/analyze_profile_tests.ps1"
        -Analyzer "${CMAKE_SOURCE_DIR}/scripts/analyze-profile.ps1"
)
add_test(
    NAME spectiary_analyze_navigation_profile_tests
    COMMAND powershell -NoProfile -ExecutionPolicy Bypass
        -File "${CMAKE_SOURCE_DIR}/tests/analyze_navigation_profile_tests.ps1"
        -Analyzer "${CMAKE_SOURCE_DIR}/scripts/analyze-navigation-profile.ps1"
)
add_test(
    NAME spectiary_analyze_source_load_profile_tests
    COMMAND powershell -NoProfile -ExecutionPolicy Bypass
        -File "${CMAKE_SOURCE_DIR}/tests/analyze_source_load_profile_tests.ps1"
        -Analyzer "${CMAKE_SOURCE_DIR}/scripts/analyze-source-load-profile.ps1"
)
add_test(
    NAME spectiary_build_ninja_msvc_debug_watchdog_tests
    COMMAND powershell -NoProfile -ExecutionPolicy Bypass
        -File "${CMAKE_SOURCE_DIR}/tests/build_ninja_msvc_debug_watchdog_tests.ps1"
        -Wrapper "${CMAKE_SOURCE_DIR}/scripts/build-ninja-msvc-debug.ps1"
)
add_executable(
    spectiary_runtime_resource_workload_tests
    tests/runtime_resource_workload_tests.cpp
    src/app/runtime_resource_workload.cpp
)
spectiary_configure_production_target(
    spectiary_runtime_resource_workload_tests
)
target_link_libraries(
    spectiary_runtime_resource_workload_tests
    PRIVATE
        spectiary_desktop_ui
        dwmapi
)
add_test(
    NAME spectiary_runtime_resource_workload_tests
    COMMAND spectiary_runtime_resource_workload_tests
)
add_test(
    NAME spectiary_runtime_resource_stability_runner_tests
    COMMAND powershell -NoProfile -ExecutionPolicy Bypass
        -File "${CMAKE_SOURCE_DIR}/tests/runtime_resource_stability_runner_tests.ps1"
        -Runner "${CMAKE_SOURCE_DIR}/scripts/test-runtime-resource-stability.ps1"
        -Executable "$<TARGET_FILE:spectiary_native>"
)
add_test(
    NAME spectiary_runtime_resource_stability
    COMMAND powershell -NoProfile -ExecutionPolicy Bypass
        -File "${CMAKE_SOURCE_DIR}/scripts/test-runtime-resource-stability.ps1"
        -Executable "$<TARGET_FILE:spectiary_native>"
        -OutputDirectory "${CMAKE_BINARY_DIR}/runtime-resource-stability"
        -CTestIntegration
)
set_tests_properties(
    spectiary_runtime_resource_stability
    PROPERTIES
        LABELS "resource-stability;periodic"
        RUN_SERIAL TRUE
        SKIP_RETURN_CODE 125
        TIMEOUT 1900
)
if(MSVC AND CMAKE_GENERATOR STREQUAL "Ninja")
    get_filename_component(
        SPECTIARY_METADATA_BUILD_REGRESSION_PRESET
        "${CMAKE_BINARY_DIR}"
        NAME
    )
    add_test(
        NAME spectiary_metadata_build_regression_tests
        COMMAND powershell -NoProfile -ExecutionPolicy Bypass
            -File "${CMAKE_SOURCE_DIR}/tests/spectiary_metadata_build_regression_tests.ps1"
            -CMakeCommand "${CMAKE_COMMAND}"
            -BuildDirectory "${CMAKE_BINARY_DIR}"
            -BuiltExecutable "$<TARGET_FILE:spectiary_native>"
            -BuildTarget spectiary_metadata
            -Configuration "$<CONFIG>"
            -BuildWrapper
                "${CMAKE_SOURCE_DIR}/scripts/build-ninja-msvc-debug.ps1"
            -BuildPreset
                "${SPECTIARY_METADATA_BUILD_REGRESSION_PRESET}"
            -BuildTimeoutSec 180
    )
else()
    add_test(
        NAME spectiary_metadata_build_regression_tests
        COMMAND powershell -NoProfile -ExecutionPolicy Bypass
            -File "${CMAKE_SOURCE_DIR}/tests/spectiary_metadata_build_regression_tests.ps1"
            -CMakeCommand "${CMAKE_COMMAND}"
            -BuildDirectory "${CMAKE_BINARY_DIR}"
            -BuiltExecutable "$<TARGET_FILE:spectiary_native>"
            -BuildTarget spectiary_metadata
            -Configuration "$<CONFIG>"
    )
endif()
set_tests_properties(
    spectiary_metadata_build_regression_tests
    PROPERTIES
        LABELS "release;required"
        RUN_SERIAL TRUE
        TIMEOUT 900
)
add_test(
    NAME spectiary_release_artifacts_tests
    COMMAND powershell -NoProfile -ExecutionPolicy Bypass
        -File "${CMAKE_SOURCE_DIR}/tests/release_artifacts_tests.ps1"
        -RepoRoot "${CMAKE_SOURCE_DIR}"
        -DearImGuiVersion "${SPECTIARY_IMGUI_VERSION}"
        -ImPlotVersion "${SPECTIARY_IMPLOT_VERSION}"
        -CfitsioVersion "${SPECTIARY_CFITSIO_VERSION}"
        -PthreadsVersion "${SPECTIARY_PTHREADS_VERSION}"
        -YamlCppVersion "${SPECTIARY_YAML_CPP_VERSION}"
        -ZlibVersion "${SPECTIARY_ZLIB_VERSION}"
        -BuiltExecutable "$<TARGET_FILE:spectiary_native>"
        -GeneratedManifest "${SPECTIARY_MANIFEST}"
        -GeneratedBuildIdentity
            "${CMAKE_BINARY_DIR}/generated/$<CONFIG>/spectiary/spectiary_build_identity.h"
        -SpectiaryVersion "${PROJECT_VERSION}"
        -Configuration "$<CONFIG>"
        -SourceMode "${SPECTIARY_BUILD_SOURCE_MODE}"
        -SourceRevision "${SPECTIARY_BUILD_SOURCE_REVISION}"
        -CompilerId "${SPECTIARY_BUILD_COMPILER_ID}"
        -CompilerVersion "${SPECTIARY_BUILD_COMPILER_VERSION}"
        -CMakeVersion "${SPECTIARY_BUILD_CMAKE_VERSION}"
        -Generator "${SPECTIARY_BUILD_GENERATOR}"
        -TargetArchitecture "${SPECTIARY_BUILD_TARGET_ARCHITECTURE}"
        -WindowsSdkVersion "${SPECTIARY_BUILD_WINDOWS_SDK_VERSION}"
)
set_tests_properties(
    spectiary_release_artifacts_tests
    PROPERTIES
        LABELS "release;ci-headless;required"
        RUN_SERIAL TRUE
        # Each external verification case has its own 30-120 second process-
        # tree deadline. Retain a larger aggregate budget for hosted Windows
        # ZIP/PE scanning variability while staying below the Release job's
        # 900-second CTest step deadline.
        TIMEOUT 600
)
add_test(
    NAME spectiary_portable_pe_contract_tests
    COMMAND powershell -NoProfile -ExecutionPolicy Bypass
        -File "${CMAKE_SOURCE_DIR}/tests/portable_pe_contract_tests.ps1"
        -RepoRoot "${CMAKE_SOURCE_DIR}"
        -BuildDirectory "${CMAKE_BINARY_DIR}"
)
set_tests_properties(
    spectiary_portable_pe_contract_tests
    PROPERTIES
        LABELS "release;ci-headless;required"
        RUN_SERIAL TRUE
        TIMEOUT 30
)
add_test(
    NAME spectiary_release_workflow_structure_contract_tests
    COMMAND powershell -NoProfile -ExecutionPolicy Bypass
        -File "${CMAKE_SOURCE_DIR}/tests/release_workflow_structure_contract_tests.ps1"
        -RepoRoot "${CMAKE_SOURCE_DIR}"
)
set_tests_properties(
    spectiary_release_workflow_structure_contract_tests
    PROPERTIES
        LABELS "release;ci-headless;required"
        TIMEOUT 30
)
add_test(
    NAME spectiary_release_configured_build_contract_tests
    COMMAND powershell -NoProfile -ExecutionPolicy Bypass
        -File "${CMAKE_SOURCE_DIR}/tests/release_configured_build_contract_tests.ps1"
        -RepoRoot "${CMAKE_SOURCE_DIR}"
        -DebugBuildDirectory
            "${CMAKE_SOURCE_DIR}/build/ninja-msvc-debug"
        -ReleaseBuildDirectory
            "${CMAKE_SOURCE_DIR}/build/ninja-msvc-release-static"
)
set_tests_properties(
    spectiary_release_configured_build_contract_tests
    PROPERTIES
        LABELS "release;ci-headless;required"
        RUN_SERIAL TRUE
        TIMEOUT 60
)
add_test(
    NAME spectiary_application_icon_tests
    COMMAND powershell -NoProfile -ExecutionPolicy Bypass
        -File "${CMAKE_SOURCE_DIR}/tests/application_icon_tests.ps1"
        -RepoRoot "${CMAKE_SOURCE_DIR}"
        -BuiltExecutable "$<TARGET_FILE:spectiary_native>"
)
set_tests_properties(
    spectiary_application_icon_tests
    PROPERTIES
        LABELS "release;required"
)
add_test(
    NAME spectiary_head_snapshot_build_tests
    COMMAND
        powershell
        -NoProfile
        -ExecutionPolicy Bypass
        -File "${CMAKE_CURRENT_SOURCE_DIR}/tests/head_snapshot_build_tests.ps1"
        -RepoRoot "${CMAKE_CURRENT_SOURCE_DIR}"
)
add_test(
    NAME spectiary_panel_session_render_architecture_tests
    COMMAND powershell -NoProfile -ExecutionPolicy Bypass
        -File "${CMAKE_SOURCE_DIR}/tests/panel_session_render_architecture_tests.ps1"
        -RepoRoot "${CMAKE_SOURCE_DIR}"
)
add_test(
    NAME spectiary_production_target_dependency_architecture_tests
    COMMAND powershell -NoProfile -ExecutionPolicy Bypass
        -File "${CMAKE_SOURCE_DIR}/tests/production_target_dependency_architecture_tests.ps1"
        -RepoRoot "${CMAKE_SOURCE_DIR}"
)
add_test(
    NAME spectiary_imgui_viewport_policy_architecture_tests
    COMMAND powershell -NoProfile -ExecutionPolicy Bypass
        -File "${CMAKE_SOURCE_DIR}/tests/imgui_viewport_policy_architecture_tests.ps1"
        -RepoRoot "${CMAKE_SOURCE_DIR}"
)
spectiary_add_test(
    spectiary_win32_compositor_clock_tests
    tests/win32_compositor_clock_tests.cpp
    spectiary_core
)
spectiary_add_test(
    spectiary_win32_message_wait_tests
    tests/win32_message_wait_tests.cpp
    spectiary_core
)
spectiary_add_test(
    spectiary_win32_process_launcher_tests
    tests/win32_process_launcher_tests.cpp
    spectiary_core
)
target_link_libraries(
    spectiary_win32_process_launcher_tests
    PRIVATE
        spectiary_automation
        spectiary_desktop_ui
        dwmapi
        shell32
)
add_dependencies(
    spectiary_win32_process_launcher_tests
    spectiary_native
)
spectiary_add_test(
    spectiary_loader_tests
    tests/spectrum_loader_tests.cpp
    spectiary_core
)
target_link_libraries(spectiary_loader_tests PRIVATE ZLIB::ZLIB)
target_compile_definitions(
    spectiary_loader_tests
    PRIVATE
        SPECTIARY_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
)
target_link_libraries(spectiary_loader_tests PRIVATE dbghelp)
spectiary_add_test(
    spectiary_fits_file_reader_tests
    tests/fits_file_reader_tests.cpp
    spectiary_core
)
target_link_libraries(
    spectiary_fits_file_reader_tests
    PRIVATE
        CFITSIO::cfitsio
        dbghelp
        ZLIB::ZLIB
)
target_compile_definitions(
    spectiary_fits_file_reader_tests
    PRIVATE
        SPECTIARY_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
)
set_tests_properties(
    spectiary_loader_tests
    spectiary_fits_file_reader_tests
    PROPERTIES
        LABELS "fits;release;ci-headless;required"
)
if(MSVC)
    target_link_options(
        spectiary_loader_tests
        PRIVATE
            "$<$<CONFIG:Release>:/DEBUG:FULL>"
    )
    target_link_options(
        spectiary_fits_file_reader_tests
        PRIVATE
            "$<$<CONFIG:Release>:/DEBUG:FULL>"
    )
endif()
spectiary_add_test(
    spectiary_source_open_resolution_tests
    tests/source_open_resolution_tests.cpp
    spectiary_core
)
spectiary_add_test(
    spectiary_sample_navigation_controller_tests
    tests/sample_navigation_controller_tests.cpp
    spectiary_sessions
)
spectiary_add_test(
    spectiary_sample_navigation_sequence_tests
    tests/sample_navigation_sequence_tests.cpp
    spectiary_sessions
)
spectiary_add_test(
    spectiary_sample_workflow_shortcut_tests
    tests/sample_workflow_shortcut_tests.cpp
    spectiary_desktop_ui
)
spectiary_add_test(
    spectiary_sample_labeling_filename_tests
    tests/sample_labeling_filename_tests.cpp
    spectiary_desktop_ui
)
set_tests_properties(
    spectiary_sample_labeling_filename_tests
    PROPERTIES
        LABELS "labeling;ci-headless;required"
)
spectiary_add_test(
    spectiary_source_collection_panel_tests
    tests/source_collection_panel_tests.cpp
    spectiary_desktop_ui
)
spectiary_add_test(
    spectiary_panel_session_interaction_tests
    tests/panel_session_interaction_tests.cpp
    spectiary_desktop_ui
)
spectiary_add_test(
    spectiary_sample_workflow_rules_tests
    tests/sample_workflow_rules_tests.cpp
    spectiary_sessions
)
spectiary_add_test(
    spectiary_sample_name_autocomplete_tests
    tests/sample_name_autocomplete_tests.cpp
    spectiary_sessions
)
spectiary_add_test(
    spectiary_sample_labeling_filter_tests
    tests/sample_labeling_filter_tests.cpp
    spectiary_sessions
)
spectiary_add_test(
    spectiary_sample_label_export_tests
    tests/sample_label_export_tests.cpp
    spectiary_core
)
spectiary_add_test(
    spectiary_sample_annotation_csv_tests
    tests/sample_annotation_csv_tests.cpp
    spectiary_core
)
spectiary_add_test(
    spectiary_csv_record_codec_tests
    tests/csv_record_codec_tests.cpp
    spectiary_core
)
spectiary_add_test(
    spectiary_labeling_primitives_tests
    tests/labeling_primitives_tests.cpp
    spectiary_core
)
set_tests_properties(
    spectiary_sample_label_export_tests
    PROPERTIES
        LABELS "labeling;ci-headless;required"
)
set_tests_properties(
    spectiary_sample_annotation_csv_tests
    PROPERTIES
        LABELS "data;labeling;ci-headless;required"
)
set_tests_properties(
    spectiary_csv_record_codec_tests
    PROPERTIES
        LABELS "data;ci-headless;required"
)
set_tests_properties(
    spectiary_labeling_primitives_tests
    PROPERTIES
        LABELS "labeling;ci-headless;required"
)
spectiary_add_test(
    spectiary_sample_labeling_document_tests
    tests/sample_labeling_document_tests.cpp
    spectiary_core
)
spectiary_add_test(
    spectiary_sample_labeling_asdf_codec_tests
    tests/sample_labeling_asdf_codec_tests.cpp
    spectiary_core
)
spectiary_add_test(
    spectiary_sample_labeling_asdf_store_tests
    tests/sample_labeling_asdf_store_tests.cpp
    spectiary_core
)
spectiary_add_test(
    spectiary_sample_labeling_asdf_store_property_tests
    tests/sample_labeling_asdf_store_property_tests.cpp
    spectiary_core
)
add_executable(
    spectiary_sample_labeling_asdf_mutation_tests
    tests/sample_labeling_asdf_mutation_tests.cpp
)
target_link_libraries(
    spectiary_sample_labeling_asdf_mutation_tests
    PRIVATE
        spectiary_core
        yaml-cpp::yaml-cpp
        ZLIB::ZLIB
)
spectiary_enable_warnings(spectiary_sample_labeling_asdf_mutation_tests)
add_test(
    NAME spectiary_sample_labeling_asdf_mutation_tests
    COMMAND
        spectiary_sample_labeling_asdf_mutation_tests
        --seed 0x78c0de2026
        --case-count 73
        --max-input-bytes 1048576
)
target_compile_definitions(
    spectiary_sample_labeling_asdf_codec_tests
    PRIVATE
        SPECTIARY_ASDF_LABELING_FIXTURE_DIR="${CMAKE_SOURCE_DIR}/tests/fixtures/asdf_labeling"
        SPECTIARY_EXPECTED_VERSION="${PROJECT_VERSION}"
)
target_compile_definitions(
    spectiary_sample_labeling_asdf_store_tests
    PRIVATE
        SPECTIARY_ASDF_LABELING_FIXTURE_DIR="${CMAKE_SOURCE_DIR}/tests/fixtures/asdf_labeling"
)
target_compile_definitions(
    spectiary_sample_labeling_asdf_store_property_tests
    PRIVATE
        SPECTIARY_ASDF_LABELING_FIXTURE_DIR="${CMAKE_SOURCE_DIR}/tests/fixtures/asdf_labeling"
)
target_compile_definitions(
    spectiary_sample_labeling_asdf_mutation_tests
    PRIVATE
        SPECTIARY_ASDF_LABELING_FIXTURE_DIR="${CMAKE_SOURCE_DIR}/tests/fixtures/asdf_labeling"
)
if(SPECTIARY_BUILD_ASDF_LABELING_SPIKE)
    add_dependencies(
        spectiary_asdf_labeling_interoperability_targets
        spectiary_sample_labeling_asdf_codec_tests
    )
endif()
spectiary_add_test(
    spectiary_source_collection_session_tests
    tests/source_collection_session_tests.cpp
    spectiary_sessions
)
set_tests_properties(
    spectiary_sample_workflow_rules_tests
    spectiary_sample_labeling_document_tests
    spectiary_sample_labeling_asdf_codec_tests
    spectiary_sample_labeling_asdf_store_tests
    spectiary_sample_labeling_asdf_store_property_tests
    spectiary_sample_labeling_asdf_mutation_tests
    spectiary_sample_labeling_filter_tests
    spectiary_source_collection_session_tests
    PROPERTIES
        LABELS "labeling;asdf-lifecycle;ci-headless;required"
)
set_tests_properties(
    spectiary_sample_labeling_asdf_store_property_tests
    spectiary_sample_labeling_asdf_mutation_tests
    PROPERTIES
        LABELS "labeling;asdf-lifecycle;asdf-hardening;ci-headless;required"
        TIMEOUT 120
)
spectiary_add_test(
    spectiary_source_collection_load_queue_tests
    tests/source_collection_load_queue_tests.cpp
    spectiary_sessions
)
spectiary_add_test(
    spectiary_source_collection_preparation_tests
    tests/source_collection_preparation_tests.cpp
    spectiary_sessions
)
spectiary_add_test(
    spectiary_source_collection_activation_transaction_tests
    tests/source_collection_activation_transaction_tests.cpp
    spectiary_sessions
)
spectiary_add_test(
    spectiary_shell_source_load_activation_tests
    tests/shell_source_load_activation_tests.cpp
    spectiary_desktop_ui
)
target_link_libraries(spectiary_shell_source_load_activation_tests PRIVATE dwmapi)
set_tests_properties(
    spectiary_shell_source_load_activation_tests
    PROPERTIES TIMEOUT 120
)
spectiary_add_test(
    spectiary_spectrum_smoothing_tests
    tests/spectrum_smoothing_tests.cpp
    spectiary_core
)
spectiary_add_test(
    spectiary_plot_touchpad_gesture_tests
    tests/plot_touchpad_gesture_tests.cpp
    spectiary_desktop_ui
)
spectiary_add_test(
    spectiary_series_color_tests
    tests/series_color_tests.cpp
    spectiary_desktop_ui
)
spectiary_add_test(
    spectiary_spectral_line_label_layout_tests
    tests/spectral_line_label_layout_tests.cpp
    spectiary_desktop_ui
)
spectiary_add_test(
    spectiary_scientific_label_tests
    tests/scientific_label_tests.cpp
    spectiary_desktop_ui
)
spectiary_add_test(
    spectiary_application_settings_tests
    tests/application_settings_tests.cpp
    spectiary_desktop_ui
)
spectiary_add_test(
    spectiary_native_window_title_tests
    tests/native_window_title_tests.cpp
    spectiary_desktop_ui
)
target_compile_definitions(
    spectiary_native_window_title_tests
    PRIVATE
        SPECTIARY_EXPECTED_VERSION="${PROJECT_VERSION}"
)
spectiary_add_test(
    spectiary_ui_font_glyph_ranges_tests
    tests/ui_font_glyph_ranges_tests.cpp
    spectiary_desktop_ui
)
spectiary_add_test(
    spectiary_ui_text_tests
    tests/ui_text_tests.cpp
    spectiary_desktop_ui
)
spectiary_add_test(
    spectiary_ui_scale_settings_tests
    tests/ui_scale_settings_tests.cpp
    spectiary_desktop_ui
)
spectiary_add_test(
    spectiary_theme_tests
    tests/theme_tests.cpp
    spectiary_desktop_ui
)
add_test(
    NAME spectiary_ui_scale_runtime_architecture_tests
    COMMAND powershell -NoProfile -ExecutionPolicy Bypass
        -File "${CMAKE_SOURCE_DIR}/tests/ui_scale_runtime_architecture_tests.ps1"
        -RepoRoot "${CMAKE_SOURCE_DIR}"
)
add_test(
    NAME spectiary_native_window_title_architecture_tests
    COMMAND powershell -NoProfile -ExecutionPolicy Bypass
        -File "${CMAKE_SOURCE_DIR}/tests/native_window_title_architecture_tests.ps1"
        -RepoRoot "${CMAKE_SOURCE_DIR}"
)
spectiary_add_test(
    spectiary_spectrum_view_session_tests
    tests/spectrum_view_session_tests.cpp
    spectiary_desktop_ui
)
spectiary_add_test(
    spectiary_spectral_line_catalog_tests
    tests/spectral_line_catalog_tests.cpp
    spectiary_core
)
spectiary_add_test(
    spectiary_spectral_line_list_tests
    tests/spectral_line_list_tests.cpp
    spectiary_core
)
target_compile_definitions(spectiary_spectral_line_list_tests
    PRIVATE SPECTIARY_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
spectiary_add_test(
    spectiary_built_in_spectral_line_state_tests
    tests/built_in_spectral_line_state_tests.cpp
    spectiary_core
)
target_compile_definitions(
    spectiary_spectral_line_catalog_tests
    PRIVATE SPECTIARY_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
)
if(SPECTIARY_EMBED_PUBLIC_SPECTRAL_LINES)
    target_compile_definitions(
        spectiary_spectral_line_catalog_tests
        PRIVATE SPECTIARY_EXPECT_EMBEDDED_PUBLIC_SPECTRAL_LINES
    )
endif()

if(NOT SPECTIARY_EMBED_PUBLIC_SPECTRAL_LINES)
    add_custom_command(TARGET spectiary_spectral_line_catalog_tests POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E make_directory "$<TARGET_FILE_DIR:spectiary_spectral_line_catalog_tests>/config"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${SPECTIARY_PUBLIC_SPECTRAL_LINES_TSV}"
            "$<TARGET_FILE_DIR:spectiary_spectral_line_catalog_tests>/config/spectral_lines.public.tsv"
    )
endif()

spectiary_add_test(
    spectiary_spectral_line_user_state_tests
    tests/spectral_line_user_state_tests.cpp
    spectiary_core
)
spectiary_add_test(
    spectiary_spectral_lines_panel_controller_tests
    tests/spectral_lines_panel_controller_tests.cpp
    spectiary_sessions
)

add_test(
    NAME spectiary_ctest_tier_contract_tests
    COMMAND powershell -NoProfile -ExecutionPolicy Bypass
        -File "${CMAKE_SOURCE_DIR}/tests/ctest_tier_contract_tests.ps1"
        -BuildDirectory "${CMAKE_BINARY_DIR}"
        -PresetsFile "${CMAKE_SOURCE_DIR}/CMakePresets.json"
        -SourceDirectory "${CMAKE_SOURCE_DIR}"
)
set_tests_properties(
    spectiary_ctest_tier_contract_tests
    PROPERTIES
        TIMEOUT 30
)

# Keep the developer feedback model intentionally small: every root test must
# belong to exactly one of these two explicit lists. The contract test validates
# the configured CTest model so a new unclassified test fails visibly.
add_test(
    NAME spectiary_presentation_profile_schema_tests
    COMMAND powershell -NoProfile -ExecutionPolicy Bypass
        -File "${CMAKE_SOURCE_DIR}/tests/presentation_profile_schema_tests.ps1"
        -Analyzer "${CMAKE_SOURCE_DIR}/scripts/analyze-presentation-profile.ps1"
)
add_test(
    NAME spectiary_live_resize_resource_observation_tests
    COMMAND powershell -NoProfile -ExecutionPolicy Bypass
        -File "${CMAKE_SOURCE_DIR}/tests/live_resize_resource_observation_tests.ps1"
        -RepoRoot "${CMAKE_SOURCE_DIR}"
)
set_tests_properties(spectiary_live_resize_resource_observation_tests spectiary_presentation_profile_schema_tests spectiary_presentation_trace_tests
    PROPERTIES TIMEOUT 15)
set(SPECTIARY_FAST_TESTS
    spectiary_sample_labeling_persistence_owner_tests
    spectiary_live_resize_resource_observation_tests
    spectiary_presentation_trace_tests
    spectiary_presentation_profile_schema_tests
    spectiary_sdr_swap_chain_tests
    spectiary_win32_application_icon_tests
    spectiary_local_user_state_tests
    spectiary_runtime_storage_context_tests
    spectiary_portable_runtime_state_tests
    spectiary_render_wake_scheduler_tests
    spectiary_imgui_layout_persistence_tests
    spectiary_automation_control_tests
    spectiary_automation_execution_tests
    spectiary_automation_panel_command_coordinator_tests
    spectiary_automation_state_isolation_tests
    spectiary_automation_samples_contract_tests
    spectiary_automation_ci_contract_tests
    spectiary_repository_verification_ci_contract_tests
    spectiary_release_artifact_process_runner_tests
    spectiary_on_demand_frame_capture_tests
    spectiary_profile_recording_ui_state_tests
    spectiary_top_bar_status_layout_tests
    spectiary_top_bar_status_hover_tests
    spectiary_profile_sink_tests
    spectiary_profile_settings_tests
    spectiary_navigation_latency_trace_tests
    spectiary_source_load_latency_trace_tests
    spectiary_metadata_tests
    spectiary_metadata_finalizer_tests
    spectiary_atomic_file_tests
    spectiary_embedded_legal_documents_tests
    spectiary_build_ninja_msvc_debug_watchdog_tests
    spectiary_runtime_resource_workload_tests
    spectiary_runtime_resource_stability_runner_tests
    spectiary_panel_session_render_architecture_tests
    spectiary_production_target_dependency_architecture_tests
    spectiary_imgui_viewport_policy_architecture_tests
    spectiary_win32_compositor_clock_tests
    spectiary_win32_message_wait_tests
    spectiary_win32_process_launcher_tests
    spectiary_loader_tests
    spectiary_fits_file_reader_tests
    spectiary_source_open_resolution_tests
    spectiary_sample_navigation_controller_tests
    spectiary_sample_navigation_sequence_tests
    spectiary_sample_workflow_shortcut_tests
    spectiary_sample_labeling_filename_tests
    spectiary_source_collection_panel_tests
    spectiary_panel_session_interaction_tests
    spectiary_sample_workflow_rules_tests
    spectiary_sample_name_autocomplete_tests
    spectiary_sample_label_export_tests
    spectiary_sample_annotation_csv_tests
    spectiary_csv_record_codec_tests
    spectiary_labeling_primitives_tests
    spectiary_sample_labeling_document_tests
    spectiary_sample_labeling_asdf_codec_tests
    spectiary_sample_labeling_asdf_store_tests
    spectiary_sample_labeling_asdf_mutation_tests
    spectiary_source_collection_session_tests
    spectiary_source_collection_load_queue_tests
    spectiary_source_collection_preparation_tests
    spectiary_source_collection_activation_transaction_tests
    spectiary_spectrum_smoothing_tests
    spectiary_plot_touchpad_gesture_tests
    spectiary_series_color_tests
    spectiary_spectral_line_label_layout_tests
    spectiary_scientific_label_tests
    spectiary_application_settings_tests
    spectiary_native_window_title_tests
    spectiary_ui_font_glyph_ranges_tests
    spectiary_ui_text_tests
    spectiary_ui_scale_settings_tests
    spectiary_theme_tests
    spectiary_ui_scale_runtime_architecture_tests
    spectiary_native_window_title_architecture_tests
    spectiary_spectrum_view_session_tests
    spectiary_spectral_line_catalog_tests
    spectiary_spectral_line_list_tests
    spectiary_built_in_spectral_line_state_tests
    spectiary_spectral_line_user_state_tests
    spectiary_ctest_tier_contract_tests
)
set(SPECTIARY_EXTENDED_TESTS
    spectiary_automation_launcher_integration_tests
    spectiary_imgui_viewport_ownership_integration_tests
    spectiary_automation_samples_tests
    spectiary_multi_instance_labeling_smoke_tests
    spectiary_runtime_resource_stability
    spectiary_metadata_build_regression_tests
    spectiary_release_artifacts_tests
    spectiary_portable_pe_contract_tests
    spectiary_release_workflow_structure_contract_tests
    spectiary_release_configured_build_contract_tests
    spectiary_application_icon_tests
    spectiary_head_snapshot_build_tests
    spectiary_settings_panel_tests
    spectiary_analyze_profile_tests
    spectiary_analyze_navigation_profile_tests
    spectiary_analyze_source_load_profile_tests
    spectiary_sample_labeling_filter_tests
    spectiary_sample_labeling_asdf_store_property_tests
    spectiary_shell_source_load_activation_tests
    spectiary_spectral_lines_panel_controller_tests
)
set_property(
    TEST ${SPECTIARY_FAST_TESTS}
    APPEND PROPERTY LABELS fast
)
set_property(
    TEST ${SPECTIARY_EXTENDED_TESTS}
    APPEND PROPERTY LABELS extended
)

if(SPECTIARY_BUILD_WIDGET_TESTS)
    add_subdirectory(tests/widget)
    include(cmake/redirection_experiment.cmake)
    set_property(TEST spectiary_sample_workflow_shortcut_tests spectiary_shell_source_load_activation_tests
        APPEND PROPERTY LABELS widget)
endif()
