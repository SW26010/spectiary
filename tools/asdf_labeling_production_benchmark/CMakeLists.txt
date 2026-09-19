# The hardening option exposes this target, but default builds must not compile
# or run the benchmark. CI and local evidence runs request it explicitly.
add_executable(spectiary_asdf_labeling_production_benchmark EXCLUDE_FROM_ALL
    benchmark_document_factory.cpp
    benchmark_main.cpp
)

target_include_directories(
    spectiary_asdf_labeling_production_benchmark
    PRIVATE
        "${PROJECT_SOURCE_DIR}/src"
        "${CMAKE_CURRENT_SOURCE_DIR}"
)
target_compile_definitions(
    spectiary_asdf_labeling_production_benchmark
    PRIVATE
        NOMINMAX
        UNICODE
        _UNICODE
        WIN32_LEAN_AND_MEAN
        SPECTIARY_BENCHMARK_BUILD_CONFIGURATION="$<CONFIG>"
        SPECTIARY_BENCHMARK_COMPILER="${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}"
)
target_link_libraries(
    spectiary_asdf_labeling_production_benchmark
    PRIVATE
        spectiary_core
        Psapi
)
spectiary_enable_warnings(spectiary_asdf_labeling_production_benchmark)

set_target_properties(
    spectiary_asdf_labeling_production_benchmark
    PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tools"
)
