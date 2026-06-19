cmake_minimum_required(VERSION 3.24)

project(SpecForge LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(SPECFORGE_EMBED_PUBLIC_SPECTRAL_LINES "Embed the public spectral line catalog in the executable." OFF)

find_package(imgui CONFIG REQUIRED)
find_package(implot CONFIG REQUIRED)
find_package(ZLIB REQUIRED)

set(SPECFORGE_MANIFEST "${CMAKE_CURRENT_SOURCE_DIR}/src/platform/specforge.exe.manifest")
set(SPECFORGE_PUBLIC_SPECTRAL_LINES_TSV "${CMAKE_SOURCE_DIR}/config/spectral_lines.public.tsv")

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

add_executable(specforge_native WIN32
    src/app/specforge_app.cpp
    src/app/specforge_app.h
    src/domain/sample_filter.cpp
    src/domain/sample_filter.h
    src/domain/npy_array_io.cpp
    src/domain/npy_array_io.h
    src/domain/sample_labeling.cpp
    src/domain/sample_labeling.h
    src/domain/spectrum_fixture.cpp
    src/domain/spectrum_fixture.h
    src/domain/sample_annotation_io.cpp
    src/domain/sample_annotation_io.h
    src/domain/spectrum_format_adapters.cpp
    src/domain/spectrum_format_adapters.h
    src/domain/spectrum_loader.cpp
    src/domain/spectrum_loader.h
    src/domain/spectrum_snapshot.h
    src/domain/spectrum_smoothing.cpp
    src/domain/spectrum_smoothing.h
    src/main.cpp
    src/overlays/spectral_line_catalog.cpp
    src/overlays/spectral_line_catalog.h
    src/overlays/spectral_line_user_state_cache_io.cpp
    src/overlays/spectral_line_user_state.cpp
    src/overlays/spectral_line_user_state.h
    src/platform/win32_window.cpp
    src/platform/win32_window.h
    ${SPECFORGE_MANIFEST}
    src/plot/spectrum_plot.cpp
    src/plot/spectrum_plot.h
    src/profile/profile_sink.cpp
    src/profile/profile_sink.h
    src/renderer/d3d11_renderer.cpp
    src/renderer/d3d11_renderer.h
    src/ui/shell_ui.cpp
    src/ui/shell_ui.h
    src/ui/sample_name_autocomplete.cpp
    src/ui/sample_name_autocomplete.h
    src/ui/sample_labeling_controller.cpp
    src/ui/sample_labeling_controller.h
    src/ui/sample_navigation_controller.cpp
    src/ui/sample_navigation_controller.h
    src/ui/sample_workflow_panel.cpp
    src/ui/sample_workflow_panel.h
    src/ui/spectral_lines_grouping_view.cpp
    src/ui/spectral_lines_grouping_view.h
    src/ui/spectral_lines_panel.cpp
    src/ui/spectral_lines_panel.h
    src/ui/spectral_lines_panel_controller.cpp
    src/ui/spectral_lines_panel_controller.h
)

target_include_directories(specforge_native PRIVATE src)

target_compile_definitions(specforge_native PRIVATE
    NOMINMAX
    UNICODE
    _UNICODE
    WIN32_LEAN_AND_MEAN
)

target_link_libraries(specforge_native PRIVATE
    imgui::imgui
    implot::implot
    ZLIB::ZLIB
    d3d11
    dxgi
    dwmapi
    imm32
    ole32
    shell32
)
specforge_use_embedded_resources(specforge_native)

if(MSVC)
    target_compile_options(specforge_native PRIVATE /W4 /permissive-)
else()
    target_compile_options(specforge_native PRIVATE -Wall -Wextra -Wpedantic)
endif()

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

enable_testing()

add_executable(specforge_loader_tests
    tests/spectrum_loader_tests.cpp
    src/domain/npy_array_io.cpp
    src/domain/npy_array_io.h
    src/domain/sample_annotation_io.cpp
    src/domain/sample_annotation_io.h
    src/domain/spectrum_format_adapters.cpp
    src/domain/spectrum_format_adapters.h
    src/domain/spectrum_loader.cpp
    src/domain/spectrum_loader.h
    src/domain/spectrum_snapshot.h
)

target_include_directories(specforge_loader_tests PRIVATE src)
target_link_libraries(specforge_loader_tests PRIVATE ZLIB::ZLIB)

if(MSVC)
    target_compile_options(specforge_loader_tests PRIVATE /W4 /permissive-)
else()
    target_compile_options(specforge_loader_tests PRIVATE -Wall -Wextra -Wpedantic)
endif()

add_test(NAME specforge_loader_tests COMMAND specforge_loader_tests)

add_executable(specforge_sample_navigation_controller_tests
    tests/sample_navigation_controller_tests.cpp
    src/domain/npy_array_io.cpp
    src/domain/npy_array_io.h
    src/domain/sample_annotation_io.cpp
    src/domain/sample_annotation_io.h
    src/ui/sample_navigation_controller.cpp
    src/ui/sample_navigation_controller.h
    src/domain/spectrum_snapshot.h
)

target_include_directories(specforge_sample_navigation_controller_tests PRIVATE src)

if(MSVC)
    target_compile_options(specforge_sample_navigation_controller_tests PRIVATE /W4 /permissive-)
else()
    target_compile_options(specforge_sample_navigation_controller_tests PRIVATE -Wall -Wextra -Wpedantic)
endif()

add_test(NAME specforge_sample_navigation_controller_tests COMMAND specforge_sample_navigation_controller_tests)

add_executable(specforge_sample_name_autocomplete_tests
    tests/sample_name_autocomplete_tests.cpp
    src/ui/sample_name_autocomplete.cpp
    src/ui/sample_name_autocomplete.h
)

target_include_directories(specforge_sample_name_autocomplete_tests PRIVATE src)

if(MSVC)
    target_compile_options(specforge_sample_name_autocomplete_tests PRIVATE /W4 /permissive-)
else()
    target_compile_options(specforge_sample_name_autocomplete_tests PRIVATE -Wall -Wextra -Wpedantic)
endif()

add_test(NAME specforge_sample_name_autocomplete_tests COMMAND specforge_sample_name_autocomplete_tests)

add_executable(specforge_sample_labeling_filter_tests
    tests/sample_labeling_filter_tests.cpp
    src/domain/npy_array_io.cpp
    src/domain/npy_array_io.h
    src/domain/sample_filter.cpp
    src/domain/sample_filter.h
    src/domain/sample_labeling.cpp
    src/domain/sample_labeling.h
    src/domain/sample_annotation_io.h
    src/domain/spectrum_snapshot.h
    src/ui/sample_labeling_controller.cpp
    src/ui/sample_labeling_controller.h
)

target_include_directories(specforge_sample_labeling_filter_tests PRIVATE src)

if(MSVC)
    target_compile_options(specforge_sample_labeling_filter_tests PRIVATE /W4 /permissive-)
else()
    target_compile_options(specforge_sample_labeling_filter_tests PRIVATE -Wall -Wextra -Wpedantic)
endif()

add_test(NAME specforge_sample_labeling_filter_tests COMMAND specforge_sample_labeling_filter_tests)

add_executable(specforge_spectrum_smoothing_tests
    tests/spectrum_smoothing_tests.cpp
    src/domain/spectrum_smoothing.cpp
    src/domain/spectrum_smoothing.h
)

target_include_directories(specforge_spectrum_smoothing_tests PRIVATE src)

if(MSVC)
    target_compile_options(specforge_spectrum_smoothing_tests PRIVATE /W4 /permissive-)
else()
    target_compile_options(specforge_spectrum_smoothing_tests PRIVATE -Wall -Wextra -Wpedantic)
endif()

add_test(NAME specforge_spectrum_smoothing_tests COMMAND specforge_spectrum_smoothing_tests)

add_executable(specforge_spectral_line_catalog_tests
    tests/spectral_line_catalog_tests.cpp
    src/overlays/spectral_line_catalog.cpp
    src/overlays/spectral_line_catalog.h
)

target_include_directories(specforge_spectral_line_catalog_tests PRIVATE src)
target_compile_definitions(
    specforge_spectral_line_catalog_tests
    PRIVATE SPECFORGE_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
)
specforge_use_embedded_resources(specforge_spectral_line_catalog_tests)

if(NOT SPECFORGE_EMBED_PUBLIC_SPECTRAL_LINES)
    add_custom_command(TARGET specforge_spectral_line_catalog_tests POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E make_directory "$<TARGET_FILE_DIR:specforge_spectral_line_catalog_tests>/config"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${SPECFORGE_PUBLIC_SPECTRAL_LINES_TSV}"
            "$<TARGET_FILE_DIR:specforge_spectral_line_catalog_tests>/config/spectral_lines.public.tsv"
    )
endif()

if(MSVC)
    target_compile_options(specforge_spectral_line_catalog_tests PRIVATE /W4 /permissive-)
else()
    target_compile_options(specforge_spectral_line_catalog_tests PRIVATE -Wall -Wextra -Wpedantic)
endif()

add_test(NAME specforge_spectral_line_catalog_tests COMMAND specforge_spectral_line_catalog_tests)

add_executable(specforge_spectral_line_user_state_tests
    tests/spectral_line_user_state_tests.cpp
    src/overlays/spectral_line_catalog.cpp
    src/overlays/spectral_line_catalog.h
    src/overlays/spectral_line_user_state_cache_io.cpp
    src/overlays/spectral_line_user_state.cpp
    src/overlays/spectral_line_user_state.h
)

target_include_directories(specforge_spectral_line_user_state_tests PRIVATE src)

if(MSVC)
    target_compile_options(specforge_spectral_line_user_state_tests PRIVATE /W4 /permissive-)
else()
    target_compile_options(specforge_spectral_line_user_state_tests PRIVATE -Wall -Wextra -Wpedantic)
endif()

add_test(NAME specforge_spectral_line_user_state_tests COMMAND specforge_spectral_line_user_state_tests)
