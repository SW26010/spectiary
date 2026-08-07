cmake_minimum_required(VERSION 3.24)

project(SpecForge VERSION 0.7.8 LANGUAGES CXX RC)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(SPECFORGE_EMBED_PUBLIC_SPECTRAL_LINES "Embed the public spectral line catalog in the executable." OFF)
set(SPECFORGE_BUILD_SOURCE_MODE "" CACHE STRING "SpecForge build source mode.")
set_property(CACHE SPECFORGE_BUILD_SOURCE_MODE PROPERTY STRINGS working_tree head)
set(SPECFORGE_BUILD_SOURCE_REVISION "" CACHE STRING "Full Git object ID for head source mode.")

include("${CMAKE_SOURCE_DIR}/cmake/specforge_build_source.cmake")

find_package(imgui CONFIG REQUIRED)
find_package(implot CONFIG REQUIRED)
find_package(ZLIB REQUIRED)

set(SPECFORGE_BUILD_COMPILER_ID "${CMAKE_CXX_COMPILER_ID}")
set(SPECFORGE_BUILD_COMPILER_VERSION "${CMAKE_CXX_COMPILER_VERSION}")
set(SPECFORGE_BUILD_CMAKE_VERSION "${CMAKE_VERSION}")
set(SPECFORGE_BUILD_GENERATOR "${CMAKE_GENERATOR}")

set(SPECFORGE_BUILD_TARGET_ARCHITECTURE
    "${CMAKE_CXX_COMPILER_ARCHITECTURE_ID}"
)
if(NOT SPECFORGE_BUILD_TARGET_ARCHITECTURE)
    set(SPECFORGE_BUILD_TARGET_ARCHITECTURE "${CMAKE_GENERATOR_PLATFORM}")
endif()
string(TOLOWER
    "${SPECFORGE_BUILD_TARGET_ARCHITECTURE}"
    SPECFORGE_BUILD_TARGET_ARCHITECTURE
)
if(SPECFORGE_BUILD_TARGET_ARCHITECTURE MATCHES "^(amd64|x64|x86_64)$")
    set(SPECFORGE_BUILD_TARGET_ARCHITECTURE "amd64")
elseif(SPECFORGE_BUILD_TARGET_ARCHITECTURE MATCHES "^(win32|i[3-6]86)$")
    set(SPECFORGE_BUILD_TARGET_ARCHITECTURE "x86")
elseif(SPECFORGE_BUILD_TARGET_ARCHITECTURE STREQUAL "aarch64")
    set(SPECFORGE_BUILD_TARGET_ARCHITECTURE "arm64")
endif()

set(SPECFORGE_BUILD_WINDOWS_SDK_VERSION
    "${CMAKE_VS_WINDOWS_TARGET_PLATFORM_VERSION}"
)
if(SPECFORGE_BUILD_WINDOWS_SDK_VERSION)
    set(SPECFORGE_BUILD_WINDOWS_SDK_VERSION_JSON
        "\"${SPECFORGE_BUILD_WINDOWS_SDK_VERSION}\""
    )
    set(SPECFORGE_BUILD_WINDOWS_SDK_VERSION_DISPLAY
        "${SPECFORGE_BUILD_WINDOWS_SDK_VERSION}"
    )
else()
    set(SPECFORGE_BUILD_WINDOWS_SDK_VERSION_JSON "null")
    set(SPECFORGE_BUILD_WINDOWS_SDK_VERSION_DISPLAY "unavailable")
endif()

foreach(specforge_toolchain_field IN ITEMS
    SPECFORGE_BUILD_COMPILER_ID
    SPECFORGE_BUILD_COMPILER_VERSION
    SPECFORGE_BUILD_CMAKE_VERSION
    SPECFORGE_BUILD_GENERATOR
    SPECFORGE_BUILD_TARGET_ARCHITECTURE
)
    if("${${specforge_toolchain_field}}" STREQUAL "")
        message(FATAL_ERROR
            "Could not resolve required build-toolchain metadata: "
            "${specforge_toolchain_field}."
        )
    endif()
endforeach()
message(STATUS
    "Resolved build toolchain: "
    "${SPECFORGE_BUILD_COMPILER_ID} "
    "${SPECFORGE_BUILD_COMPILER_VERSION}, "
    "CMake ${SPECFORGE_BUILD_CMAKE_VERSION}, "
    "${SPECFORGE_BUILD_GENERATOR}, "
    "${SPECFORGE_BUILD_TARGET_ARCHITECTURE}, "
    "Windows SDK ${SPECFORGE_BUILD_WINDOWS_SDK_VERSION_DISPLAY}"
)

set(SPECFORGE_MANIFEST_TEMPLATE
    "${CMAKE_SOURCE_DIR}/src/platform/specforge.exe.manifest.in"
)
set(SPECFORGE_MANIFEST
    "${CMAKE_BINARY_DIR}/generated/specforge/specforge.exe.manifest"
)
set(SPECFORGE_PUBLIC_SPECTRAL_LINES_TSV "${CMAKE_SOURCE_DIR}/config/spectral_lines.public.tsv")
set(SPECFORGE_THIRD_PARTY_NOTICES "${CMAKE_SOURCE_DIR}/legal/THIRD_PARTY_NOTICES.txt")
set(SPECFORGE_LEGAL_DOCUMENTS
    "${SPECFORGE_THIRD_PARTY_NOTICES}"
    "${CMAKE_SOURCE_DIR}/legal/DATA_SOURCES.txt"
)
set(SPECFORGE_WINDOWS_RESOURCES
    "${CMAKE_BINARY_DIR}/generated/specforge/specforge_resources.rc"
)
set(SPECFORGE_RESOURCE_HEADER
    "${CMAKE_SOURCE_DIR}/src/platform/specforge_resource.h"
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
    "${SPECFORGE_MANIFEST_TEMPLATE}"
    "${SPECFORGE_MANIFEST}"
    @ONLY
)
configure_file(
    "${CMAKE_SOURCE_DIR}/src/platform/specforge_resources.rc.in"
    "${SPECFORGE_WINDOWS_RESOURCES}"
    @ONLY
)
set_source_files_properties(
    "${SPECFORGE_WINDOWS_RESOURCES}"
    PROPERTIES
        OBJECT_DEPENDS
            "${SPECFORGE_LEGAL_DOCUMENTS};${SPECFORGE_RESOURCE_HEADER}"
)
configure_file(
    "${CMAKE_SOURCE_DIR}/cmake/specforge_build_identity.h.in"
    "${CMAKE_BINARY_DIR}/generated/specforge/specforge_build_identity.h.in"
    @ONLY
)
file(GENERATE
    OUTPUT
        "${CMAKE_BINARY_DIR}/generated/$<CONFIG>/specforge/specforge_build_identity.h"
    INPUT
        "${CMAKE_BINARY_DIR}/generated/specforge/specforge_build_identity.h.in"
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

function(specforge_use_runtime_paths target_name)
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
    src/app/embedded_legal_documents.cpp
    src/app/local_user_state.cpp
    src/app/local_user_state_json.cpp
    src/app/on_demand_frame_capture.cpp
    src/app/pan_pacing.cpp
    src/app/render_wake_scheduler.cpp
    src/app/runtime_paths.cpp
    src/app/specforge_metadata.cpp
    src/app/specforge_metadata_finalizer.cpp
    src/app/specforge_metadata_validation.cpp
    src/domain/fits_file_reader.cpp
    src/domain/fits_spectrum_loader.cpp
    src/domain/npy_array_io.cpp
    src/domain/sample_annotation_io.cpp
    src/domain/sample_filter.cpp
    src/domain/sample_labeling.cpp
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
    src/overlays/spectral_line_catalog.cpp
    src/overlays/catalog_user_state_reconciliation.cpp
    src/overlays/spectral_line_user_state_cache_io.cpp
    src/overlays/spectral_line_user_state.cpp
    src/platform/atomic_file.cpp
    src/platform/exclusive_file_lease.cpp
    src/platform/file_sha256.cpp
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
specforge_configure_production_target(specforge_core)
target_include_directories(specforge_core PRIVATE
    "${CMAKE_BINARY_DIR}/generated/$<CONFIG>"
)
specforge_use_runtime_paths(specforge_core)
specforge_use_embedded_resources(specforge_core)
target_link_libraries(specforge_core PRIVATE ZLIB::ZLIB bcrypt)

add_library(specforge_imgui_layout STATIC
    src/app/imgui_layout_persistence.cpp
)
specforge_configure_production_target(specforge_imgui_layout)
target_link_libraries(
    specforge_imgui_layout
    PUBLIC
        specforge_core
        imgui::imgui
)

add_library(specforge_automation STATIC
    src/automation/automation_named_pipe.cpp
    src/automation/automation_protocol.cpp
    src/automation/automation_startup.cpp
    src/automation/automation_state.cpp
)
specforge_configure_production_target(specforge_automation)
target_link_libraries(specforge_automation
    PUBLIC
        specforge_core
    PRIVATE
        advapi32
        ntdll
        shell32
)

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
    src/app/initial_source.cpp
    src/app/native_window_title.cpp
    src/plot/plot_touchpad_gesture.cpp
    src/plot/scientific_label.cpp
    src/plot/spectral_line_label_layout.cpp
    src/plot/spectrum_plot.cpp
    src/ui/input_settings.cpp
    src/ui/external_source_settings.cpp
    src/ui/panel_session_interaction.cpp
    src/ui/sample_workflow_shortcut.cpp
    src/ui/sample_workflow_panel.cpp
    src/ui/settings_panel.cpp
    src/ui/shell_ui.cpp
    src/ui/source_collection_panel.cpp
    src/ui/spectral_lines_grouping_view.cpp
    src/ui/spectral_lines_name_localization.cpp
    src/ui/spectral_lines_panel.cpp
    src/ui/spectrum_view_session.cpp
    src/ui/ui_font.cpp
    src/ui/ui_language_settings.cpp
    src/ui/ui_scale_settings.cpp
    src/ui/ui_text.cpp
)
specforge_configure_production_target(specforge_desktop_ui)
target_include_directories(specforge_desktop_ui BEFORE PRIVATE
    "${CMAKE_BINARY_DIR}/generated/$<CONFIG>"
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
    src/renderer/d3d11_frame_capture.cpp
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
        ntdll
        ole32
        windowscodecs
)

add_executable(specforge_native WIN32
    src/app/automation_panel_command_coordinator.cpp
    src/app/runtime_resource_workload.cpp
    src/app/specforge_app.cpp
    src/main.cpp
    src/platform/win32_touchpad_gesture_source.cpp
    src/platform/win32_window.cpp
    ${SPECFORGE_MANIFEST}
    ${SPECFORGE_WINDOWS_RESOURCES}
)
specforge_configure_production_target(specforge_native)
target_link_libraries(specforge_native PRIVATE
    specforge_automation
    specforge_desktop_ui
    specforge_imgui_layout
    specforge_renderer
    dwmapi
    comctl32
    imm32
    ole32
    uuid
)
if(MSVC)
    target_link_options(
        specforge_native
        PRIVATE
            /Brepro
            /INCREMENTAL:NO
            "$<$<CONFIG:Debug>:/DEBUG:FASTLINK>"
    )
endif()

add_executable(specforge_metadata_finalizer_tool
    src/app/specforge_metadata_finalizer_main.cpp
)
specforge_configure_production_target(specforge_metadata_finalizer_tool)
target_include_directories(specforge_metadata_finalizer_tool PRIVATE
    "${CMAKE_BINARY_DIR}/generated/$<CONFIG>"
)
target_link_libraries(specforge_metadata_finalizer_tool PRIVATE
    specforge_core
)
add_dependencies(specforge_native specforge_metadata_finalizer_tool)

set_target_properties(specforge_native PROPERTIES
    OUTPUT_NAME SpecForge
    VS_DEBUGGER_WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)

add_custom_command(TARGET specforge_native PRE_LINK
    COMMAND "${CMAKE_COMMAND}" -E rm -f
        "$<TARGET_FILE_DIR:specforge_native>/specforge_metadata.json"
    COMMENT "Invalidating stale SpecForge metadata before linking"
    VERBATIM
)

if(NOT SPECFORGE_EMBED_PUBLIC_SPECTRAL_LINES)
    add_custom_command(TARGET specforge_native POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E make_directory "$<TARGET_FILE_DIR:specforge_native>/config"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${SPECFORGE_PUBLIC_SPECTRAL_LINES_TSV}"
            "$<TARGET_FILE_DIR:specforge_native>/config/spectral_lines.public.tsv"
    )
endif()

add_executable(specforge_automation_launcher
    src/automation/automation_launcher_main.cpp
)
specforge_configure_production_target(
    specforge_automation_launcher
)
target_link_libraries(
    specforge_automation_launcher
    PRIVATE
        specforge_automation
        specforge_imgui_layout
        specforge_sessions
        bcrypt
)
set_target_properties(
    specforge_automation_launcher
    PROPERTIES
        OUTPUT_NAME SpecForgeAutomation
        VS_DEBUGGER_WORKING_DIRECTORY
            "${CMAKE_SOURCE_DIR}"
)
add_executable(specforge_automation_launcher_timeout_fixture
    tests/automation_launcher_timeout_fixture.cpp
)
specforge_configure_production_target(
    specforge_automation_launcher_timeout_fixture
)
target_link_libraries(
    specforge_automation_launcher_timeout_fixture
    PRIVATE
        specforge_automation
)
add_custom_command(TARGET specforge_native POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E rm -rf
        "$<TARGET_FILE_DIR:specforge_native>/Legal"
    COMMAND "${CMAKE_COMMAND}" -E rm -f
        "$<TARGET_FILE_DIR:specforge_native>/EULA.txt"
        "$<TARGET_FILE_DIR:specforge_native>/THIRD_PARTY_NOTICES.txt"
        "$<TARGET_FILE_DIR:specforge_native>/DATA_SOURCES.txt"
    COMMENT "Removing obsolete executable-adjacent legal documents"
    VERBATIM
)
add_custom_command(TARGET specforge_native POST_BUILD
    COMMAND "$<TARGET_FILE:specforge_metadata_finalizer_tool>"
        "$<TARGET_FILE:specforge_native>"
        "$<TARGET_FILE_DIR:specforge_native>/specforge_metadata.json"
    COMMENT "Finalizing schema 5 SpecForge metadata"
    VERBATIM
)
set(SPECFORGE_METADATA_BYPRODUCT
    "${CMAKE_CURRENT_BINARY_DIR}/${CMAKE_CFG_INTDIR}/specforge_metadata.json")
add_custom_target(specforge_metadata
    COMMAND "${CMAKE_COMMAND}"
        "-DSPECFORGE_EXECUTABLE=$<TARGET_FILE:specforge_native>"
        "-DSPECFORGE_METADATA=$<TARGET_FILE_DIR:specforge_native>/specforge_metadata.json"
        "-DSPECFORGE_FINALIZER=$<TARGET_FILE:specforge_metadata_finalizer_tool>"
        -P "${CMAKE_SOURCE_DIR}/cmake/ensure_specforge_metadata.cmake"
    DEPENDS specforge_native
        specforge_metadata_finalizer_tool
        "${CMAKE_SOURCE_DIR}/cmake/ensure_specforge_metadata.cmake"
    BYPRODUCTS
        "${SPECFORGE_METADATA_BYPRODUCT}"
    COMMENT "Building and finalizing SpecForge metadata"
    VERBATIM
)

enable_testing()

set(SPECFORGE_PORTABLE_TEST_RUNTIME_DIRECTORY
    "${CMAKE_BINARY_DIR}/portable-tests/$<CONFIG>"
)
set(SPECFORGE_PORTABLE_TEST_METADATA
    "${SPECFORGE_PORTABLE_TEST_RUNTIME_DIRECTORY}/specforge_metadata.json"
)
add_custom_target(specforge_portable_test_metadata
    COMMAND "${CMAKE_COMMAND}" -E make_directory
        "${SPECFORGE_PORTABLE_TEST_RUNTIME_DIRECTORY}"
    COMMAND "${CMAKE_COMMAND}"
        "-DINPUT=$<TARGET_FILE_DIR:specforge_native>/specforge_metadata.json"
        "-DOUTPUT=${SPECFORGE_PORTABLE_TEST_METADATA}"
        -P "${CMAKE_SOURCE_DIR}/tests/fixtures/create_portable_runtime_metadata.cmake"
    DEPENDS
        specforge_metadata
        "${CMAKE_SOURCE_DIR}/tests/fixtures/create_portable_runtime_metadata.cmake"
    COMMENT "Preparing schema 5 Portable runtime metadata for deterministic tests"
    VERBATIM
)

function(specforge_add_test target_name source_file production_target)
    add_executable(${target_name} ${source_file})
    target_link_libraries(${target_name} PRIVATE ${production_target})
    specforge_enable_warnings(${target_name})
    add_test(NAME ${target_name} COMMAND ${target_name})
endfunction()

function(specforge_add_portable_only_test target_name source_file production_target)
    add_executable(${target_name} ${source_file})
    target_link_libraries(${target_name} PRIVATE ${production_target})
    specforge_enable_warnings(${target_name})
    set(portable_fixture_target
        "${target_name}_portable_fixture"
    )
    set(portable_executable
        "${SPECFORGE_PORTABLE_TEST_RUNTIME_DIRECTORY}/$<TARGET_FILE_NAME:${target_name}>"
    )
    add_custom_target(${portable_fixture_target} ALL
        COMMAND "${CMAKE_COMMAND}" -E make_directory
            "${SPECFORGE_PORTABLE_TEST_RUNTIME_DIRECTORY}"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "$<TARGET_FILE:${target_name}>"
            "${portable_executable}"
        DEPENDS ${target_name}
        COMMENT "Preparing Portable runtime fixture for ${target_name}"
        VERBATIM
    )
    add_dependencies(
        ${portable_fixture_target}
        specforge_portable_test_metadata
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
specforge_add_portable_only_test(
    specforge_portable_runtime_state_tests
    tests/portable_runtime_state_tests.cpp
    specforge_sessions
)
specforge_add_test(
    specforge_render_wake_scheduler_tests
    tests/render_wake_scheduler_tests.cpp
    specforge_core
)
target_link_libraries(specforge_render_wake_scheduler_tests PRIVATE imgui::imgui)
specforge_add_test(
    specforge_imgui_layout_persistence_tests
    tests/imgui_layout_persistence_tests.cpp
    specforge_imgui_layout
)
target_compile_definitions(
    specforge_imgui_layout_persistence_tests
    PRIVATE
        SPECFORGE_VIEWPORT_OWNERSHIP_LAYOUT_FIXTURE="${CMAKE_SOURCE_DIR}/tests/fixtures/imgui/no-auto-merge-floating.ini"
)
specforge_add_test(
    specforge_automation_control_tests
    tests/automation_control_tests.cpp
    specforge_automation
)
target_link_libraries(
    specforge_automation_control_tests
    PRIVATE
        advapi32
        specforge_desktop_ui
)
set_tests_properties(
    specforge_automation_control_tests
    PROPERTIES
        LABELS "automation;ci-headless;required"
)
specforge_add_test(
    specforge_automation_panel_command_coordinator_tests
    tests/automation_panel_command_coordinator_tests.cpp
    specforge_automation
)
target_sources(
    specforge_automation_panel_command_coordinator_tests
    PRIVATE
        src/app/automation_panel_command_coordinator.cpp
)
target_link_libraries(
    specforge_automation_panel_command_coordinator_tests
    PRIVATE
        specforge_desktop_ui
)
set_tests_properties(
    specforge_automation_panel_command_coordinator_tests
    PROPERTIES
        LABELS "automation;ci-headless;required"
)
specforge_add_test(
    specforge_automation_state_isolation_tests
    tests/automation_state_isolation_tests.cpp
    specforge_desktop_ui
)
target_link_libraries(
    specforge_automation_state_isolation_tests
    PRIVATE
        dwmapi
)
add_dependencies(
    specforge_automation_state_isolation_tests
    specforge_metadata
)
set_tests_properties(
    specforge_automation_state_isolation_tests
    PROPERTIES
        LABELS "automation;ci-headless;required"
)
add_test(
    NAME specforge_automation_samples_contract_tests
    COMMAND powershell -NoProfile -ExecutionPolicy Bypass
        -File "${CMAKE_SOURCE_DIR}/tests/automation_samples_contract_tests.ps1"
        -FixtureDirectory
            "${CMAKE_SOURCE_DIR}/tests/fixtures/automation/samples"
        -Documentation
            "${CMAKE_SOURCE_DIR}/docs/automation_samples.md"
        -Runner
            "${CMAKE_SOURCE_DIR}/tests/automation_samples_tests.ps1"
        -CMakeLists
            "${CMAKE_SOURCE_DIR}/CMakeLists.txt"
)
set_tests_properties(
    specforge_automation_samples_contract_tests
    PROPERTIES
        LABELS "automation;ci-headless;required"
        TIMEOUT 30
)
add_test(
    NAME specforge_automation_ci_contract_tests
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
    specforge_automation_ci_contract_tests
    PROPERTIES
        LABELS "automation;ci-headless;required"
        TIMEOUT 30
)
add_test(
    NAME specforge_automation_launcher_integration_tests
    COMMAND powershell -NoProfile -ExecutionPolicy Bypass
        -File "${CMAKE_SOURCE_DIR}/tests/automation_launcher_integration_tests.ps1"
        -Launcher "$<TARGET_FILE:specforge_automation_launcher>"
        -Executable "$<TARGET_FILE:specforge_native>"
        -CleanupFixture "$<TARGET_FILE:specforge_automation_control_tests>"
        -TimeoutFixture "$<TARGET_FILE:specforge_automation_launcher_timeout_fixture>"
        -StateFixture "$<TARGET_FILE:specforge_automation_state_isolation_tests>"
)
set_tests_properties(
    specforge_automation_launcher_integration_tests
    PROPERTIES
        RUN_SERIAL TRUE
        # This test includes the deterministic fake-GUI deadline fixtures as
        # well as the real-GUI launcher workflow; retain margin over the
        # current ~94-second run on the managed desktop.
        TIMEOUT 180
        LABELS "automation;real-gui"
)
add_test(
    NAME specforge_imgui_viewport_ownership_integration_tests
    COMMAND powershell -NoProfile -ExecutionPolicy Bypass
        -File "${CMAKE_SOURCE_DIR}/tests/imgui_viewport_ownership_integration_tests.ps1"
        -Launcher "$<TARGET_FILE:specforge_automation_launcher>"
        -Executable "$<TARGET_FILE:specforge_native>"
        -LayoutSeed
            "${CMAKE_SOURCE_DIR}/tests/fixtures/imgui/no-auto-merge-floating.ini"
)
set_tests_properties(
    specforge_imgui_viewport_ownership_integration_tests
    PROPERTIES
        RUN_SERIAL TRUE
        TIMEOUT 90
        LABELS "automation;gui-integration"
)
add_test(
    NAME specforge_automation_samples_tests
    COMMAND powershell -NoProfile -ExecutionPolicy Bypass
        -File "${CMAKE_SOURCE_DIR}/tests/automation_samples_tests.ps1"
        -Launcher "$<TARGET_FILE:specforge_automation_launcher>"
        -Executable "$<TARGET_FILE:specforge_native>"
        -StateFixture "$<TARGET_FILE:specforge_automation_state_isolation_tests>"
        -FixtureDirectory
            "${CMAKE_SOURCE_DIR}/tests/fixtures/automation/samples"
        -ArtifactsDirectory
            "${CMAKE_BINARY_DIR}/test-artifacts/automation-samples"
)
set_tests_properties(
    specforge_automation_samples_tests
    PROPERTIES
        RUN_SERIAL TRUE
        LABELS "automation;real-gui"
        # Two 60s GUI sequence budgets, two 30s helper budgets, bounded
        # launcher/output/process cleanup, and evidence preservation must all
        # complete before CTest terminates the runner's finally block.
        TIMEOUT 300
)
add_test(
    NAME specforge_multi_instance_labeling_smoke_tests
    COMMAND powershell -NoProfile -ExecutionPolicy Bypass
        -File "${CMAKE_SOURCE_DIR}/tests/multi_instance_labeling_smoke_tests.ps1"
        -Executable "$<TARGET_FILE:specforge_native>"
        -StateFixture "$<TARGET_FILE:specforge_automation_state_isolation_tests>"
)
set_tests_properties(
    specforge_multi_instance_labeling_smoke_tests
    PROPERTIES
        RUN_SERIAL TRUE
        # Runner main flow is capped at 180s; 30s shared GUI cleanup, two
        # 1s GUI Kill waits, and a shared 10s helper/fallback fixture and
        # alias-directory cleanup budget stay below 240s.
        TIMEOUT 240
        LABELS "automation;real-gui"
)
specforge_add_test(
    specforge_on_demand_frame_capture_tests
    tests/on_demand_frame_capture_tests.cpp
    specforge_core
)
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
    specforge_metadata_tests
    tests/specforge_metadata_tests.cpp
    specforge_core
)
target_compile_definitions(specforge_metadata_tests PRIVATE
    SPECFORGE_METADATA_FIXTURE_DIR="${CMAKE_SOURCE_DIR}/tests/fixtures/build_metadata"
)
specforge_add_test(
    specforge_metadata_finalizer_tests
    tests/specforge_metadata_finalizer_tests.cpp
    specforge_core
)
specforge_add_test(
    specforge_atomic_file_tests
    tests/atomic_file_tests.cpp
    specforge_core
)
specforge_add_test(
    specforge_embedded_legal_documents_tests
    tests/embedded_legal_documents_tests.cpp
    specforge_core
)
target_sources(
    specforge_embedded_legal_documents_tests
    PRIVATE
        ${SPECFORGE_WINDOWS_RESOURCES}
)
target_compile_definitions(
    specforge_embedded_legal_documents_tests
    PRIVATE
        SPECFORGE_LEGAL_SOURCE_DIRECTORY="${CMAKE_SOURCE_DIR}/legal"
)
specforge_add_test(
    specforge_settings_panel_tests
    tests/settings_panel_tests.cpp
    specforge_desktop_ui
)
target_sources(
    specforge_settings_panel_tests
    PRIVATE
        ${SPECFORGE_WINDOWS_RESOURCES}
)
target_compile_definitions(specforge_settings_panel_tests PRIVATE
    SPECFORGE_EXPECTED_VERSION="${PROJECT_VERSION}"
    SPECFORGE_EXPECTED_CONFIGURATION="$<CONFIG>"
    SPECFORGE_EXPECTED_ARCHITECTURE="${SPECFORGE_BUILD_TARGET_ARCHITECTURE}"
    SPECFORGE_EXPECTED_SOURCE_MODE="${SPECFORGE_BUILD_SOURCE_MODE}"
    SPECFORGE_EXPECTED_SOURCE_REVISION="${SPECFORGE_BUILD_SOURCE_REVISION}"
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
add_executable(
    specforge_runtime_resource_workload_tests
    tests/runtime_resource_workload_tests.cpp
    src/app/runtime_resource_workload.cpp
)
specforge_configure_production_target(
    specforge_runtime_resource_workload_tests
)
target_link_libraries(
    specforge_runtime_resource_workload_tests
    PRIVATE
        specforge_desktop_ui
        dwmapi
)
add_test(
    NAME specforge_runtime_resource_workload_tests
    COMMAND specforge_runtime_resource_workload_tests
)
add_test(
    NAME specforge_runtime_resource_stability_runner_tests
    COMMAND powershell -NoProfile -ExecutionPolicy Bypass
        -File "${CMAKE_SOURCE_DIR}/tests/runtime_resource_stability_runner_tests.ps1"
        -Runner "${CMAKE_SOURCE_DIR}/scripts/test-runtime-resource-stability.ps1"
        -Executable "$<TARGET_FILE:specforge_native>"
)
add_test(
    NAME specforge_runtime_resource_stability
    COMMAND powershell -NoProfile -ExecutionPolicy Bypass
        -File "${CMAKE_SOURCE_DIR}/scripts/test-runtime-resource-stability.ps1"
        -Executable "$<TARGET_FILE:specforge_native>"
        -OutputDirectory "${CMAKE_BINARY_DIR}/runtime-resource-stability"
        -CTestIntegration
)
set_tests_properties(
    specforge_runtime_resource_stability
    PROPERTIES
        LABELS "resource-stability;periodic"
        RUN_SERIAL TRUE
        SKIP_RETURN_CODE 125
        TIMEOUT 1900
)
if(MSVC AND CMAKE_GENERATOR STREQUAL "Ninja")
    get_filename_component(
        SPECFORGE_METADATA_BUILD_REGRESSION_PRESET
        "${CMAKE_BINARY_DIR}"
        NAME
    )
    add_test(
        NAME specforge_metadata_build_regression_tests
        COMMAND powershell -NoProfile -ExecutionPolicy Bypass
            -File "${CMAKE_SOURCE_DIR}/tests/specforge_metadata_build_regression_tests.ps1"
            -CMakeCommand "${CMAKE_COMMAND}"
            -BuildDirectory "${CMAKE_BINARY_DIR}"
            -BuiltExecutable "$<TARGET_FILE:specforge_native>"
            -BuildTarget specforge_metadata
            -Configuration "$<CONFIG>"
            -BuildWrapper
                "${CMAKE_SOURCE_DIR}/scripts/build-ninja-msvc-debug.ps1"
            -BuildPreset
                "${SPECFORGE_METADATA_BUILD_REGRESSION_PRESET}"
            -BuildTimeoutSec 180
    )
else()
    add_test(
        NAME specforge_metadata_build_regression_tests
        COMMAND powershell -NoProfile -ExecutionPolicy Bypass
            -File "${CMAKE_SOURCE_DIR}/tests/specforge_metadata_build_regression_tests.ps1"
            -CMakeCommand "${CMAKE_COMMAND}"
            -BuildDirectory "${CMAKE_BINARY_DIR}"
            -BuiltExecutable "$<TARGET_FILE:specforge_native>"
            -BuildTarget specforge_metadata
            -Configuration "$<CONFIG>"
    )
endif()
set_tests_properties(
    specforge_metadata_build_regression_tests
    PROPERTIES
        LABELS "release;required"
        RUN_SERIAL TRUE
        TIMEOUT 900
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
        -GeneratedManifest "${SPECFORGE_MANIFEST}"
        -GeneratedBuildIdentity
            "${CMAKE_BINARY_DIR}/generated/$<CONFIG>/specforge/specforge_build_identity.h"
        -SpecForgeVersion "${PROJECT_VERSION}"
        -Configuration "$<CONFIG>"
        -SourceMode "${SPECFORGE_BUILD_SOURCE_MODE}"
        -SourceRevision "${SPECFORGE_BUILD_SOURCE_REVISION}"
        -CompilerId "${SPECFORGE_BUILD_COMPILER_ID}"
        -CompilerVersion "${SPECFORGE_BUILD_COMPILER_VERSION}"
        -CMakeVersion "${SPECFORGE_BUILD_CMAKE_VERSION}"
        -Generator "${SPECFORGE_BUILD_GENERATOR}"
        -TargetArchitecture "${SPECFORGE_BUILD_TARGET_ARCHITECTURE}"
        -WindowsSdkVersion "${SPECFORGE_BUILD_WINDOWS_SDK_VERSION}"
)
set_tests_properties(
    specforge_release_artifacts_tests
    PROPERTIES
        RUN_SERIAL TRUE
)
add_test(
    NAME specforge_head_snapshot_build_tests
    COMMAND
        powershell
        -NoProfile
        -ExecutionPolicy Bypass
        -File "${CMAKE_CURRENT_SOURCE_DIR}/tests/head_snapshot_build_tests.ps1"
        -RepoRoot "${CMAKE_CURRENT_SOURCE_DIR}"
)
add_test(
    NAME specforge_panel_session_render_architecture_tests
    COMMAND powershell -NoProfile -ExecutionPolicy Bypass
        -File "${CMAKE_SOURCE_DIR}/tests/panel_session_render_architecture_tests.ps1"
        -RepoRoot "${CMAKE_SOURCE_DIR}"
)
add_test(
    NAME specforge_production_target_dependency_architecture_tests
    COMMAND powershell -NoProfile -ExecutionPolicy Bypass
        -File "${CMAKE_SOURCE_DIR}/tests/production_target_dependency_architecture_tests.ps1"
        -RepoRoot "${CMAKE_SOURCE_DIR}"
)
add_test(
    NAME specforge_imgui_viewport_policy_architecture_tests
    COMMAND powershell -NoProfile -ExecutionPolicy Bypass
        -File "${CMAKE_SOURCE_DIR}/tests/imgui_viewport_policy_architecture_tests.ps1"
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
    specforge_win32_process_launcher_tests
    tests/win32_process_launcher_tests.cpp
    specforge_core
)
target_link_libraries(
    specforge_win32_process_launcher_tests
    PRIVATE
        specforge_automation
        specforge_desktop_ui
        dwmapi
        shell32
)
add_dependencies(
    specforge_win32_process_launcher_tests
    specforge_native
)
specforge_add_test(
    specforge_loader_tests
    tests/spectrum_loader_tests.cpp
    specforge_core
)
target_link_libraries(specforge_loader_tests PRIVATE ZLIB::ZLIB)
specforge_add_test(
    specforge_source_open_resolution_tests
    tests/source_open_resolution_tests.cpp
    specforge_core
)
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
    specforge_source_collection_panel_tests
    tests/source_collection_panel_tests.cpp
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
    specforge_native_window_title_tests
    tests/native_window_title_tests.cpp
    specforge_desktop_ui
)
target_compile_definitions(
    specforge_native_window_title_tests
    PRIVATE
        SPECFORGE_EXPECTED_VERSION="${PROJECT_VERSION}"
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
    specforge_ui_scale_settings_tests
    tests/ui_scale_settings_tests.cpp
    specforge_desktop_ui
)
specforge_add_test(
    specforge_input_settings_tests
    tests/input_settings_tests.cpp
    specforge_desktop_ui
)
specforge_add_test(
    specforge_external_source_settings_tests
    tests/external_source_settings_tests.cpp
    specforge_desktop_ui
)
add_test(
    NAME specforge_ui_scale_runtime_architecture_tests
    COMMAND powershell -NoProfile -ExecutionPolicy Bypass
        -File "${CMAKE_SOURCE_DIR}/tests/ui_scale_runtime_architecture_tests.ps1"
        -RepoRoot "${CMAKE_SOURCE_DIR}"
)
add_test(
    NAME specforge_native_window_title_architecture_tests
    COMMAND powershell -NoProfile -ExecutionPolicy Bypass
        -File "${CMAKE_SOURCE_DIR}/tests/native_window_title_architecture_tests.ps1"
        -RepoRoot "${CMAKE_SOURCE_DIR}"
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
