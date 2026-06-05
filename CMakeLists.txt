cmake_minimum_required(VERSION 3.24)

project(SpecForge LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(imgui CONFIG REQUIRED)
find_package(implot CONFIG REQUIRED)
find_package(ZLIB REQUIRED)

set(SPECFORGE_MANIFEST "${CMAKE_CURRENT_SOURCE_DIR}/src/platform/specforge.exe.manifest")

add_executable(specforge_native WIN32
    src/app/specforge_app.cpp
    src/app/specforge_app.h
    src/domain/spectrum_fixture.cpp
    src/domain/spectrum_fixture.h
    src/domain/spectrum_loader.cpp
    src/domain/spectrum_loader.h
    src/domain/spectrum_snapshot.h
    src/main.cpp
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

if(MSVC)
    target_compile_options(specforge_native PRIVATE /W4 /permissive-)
else()
    target_compile_options(specforge_native PRIVATE -Wall -Wextra -Wpedantic)
endif()

set_target_properties(specforge_native PROPERTIES
    OUTPUT_NAME SpecForge
    VS_DEBUGGER_WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)

enable_testing()

add_executable(specforge_loader_tests
    tests/spectrum_loader_tests.cpp
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
