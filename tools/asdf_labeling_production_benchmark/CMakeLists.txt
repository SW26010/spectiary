# The hardening option exposes this target, but default builds must not compile
# or run the benchmark. CI and local evidence runs request it explicitly.
add_executable(specforge_asdf_labeling_production_benchmark EXCLUDE_FROM_ALL
    benchmark_document_factory.cpp
    benchmark_main.cpp
)

target_include_directories(
    specforge_asdf_labeling_production_benchmark
    PRIVATE
        "${PROJECT_SOURCE_DIR}/src"
        "${CMAKE_CURRENT_SOURCE_DIR}"
)
target_compile_definitions(
    specforge_asdf_labeling_production_benchmark
    PRIVATE
        NOMINMAX
        UNICODE
        _UNICODE
        WIN32_LEAN_AND_MEAN
        SPECFORGE_BENCHMARK_BUILD_CONFIGURATION="$<CONFIG>"
        SPECFORGE_BENCHMARK_COMPILER="${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}"
)
target_link_libraries(
    specforge_asdf_labeling_production_benchmark
    PRIVATE
        specforge_core
        Psapi
)
specforge_enable_warnings(specforge_asdf_labeling_production_benchmark)

set_target_properties(
    specforge_asdf_labeling_production_benchmark
    PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tools"
)
