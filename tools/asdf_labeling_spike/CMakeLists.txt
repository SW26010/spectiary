find_package(yaml-cpp CONFIG REQUIRED)
find_package(Python3 3.11 COMPONENTS Interpreter REQUIRED)

add_executable(specforge_asdf_labeling_spike_native
    native_codec.cpp
    native_main.cpp
)
target_include_directories(specforge_asdf_labeling_spike_native PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}"
)
target_link_libraries(specforge_asdf_labeling_spike_native PRIVATE
    yaml-cpp::yaml-cpp
    ZLIB::ZLIB
)
specforge_enable_warnings(specforge_asdf_labeling_spike_native)

add_test(
    NAME specforge_asdf_labeling_spike_native_fixture_test
    COMMAND
        specforge_asdf_labeling_spike_native
        read
        "${PROJECT_SOURCE_DIR}/tests/fixtures/asdf_labeling/profile_zlib.asdf"
)
set_tests_properties(
    specforge_asdf_labeling_spike_native_fixture_test
    PROPERTIES
        LABELS "asdf;asdf-lifecycle;asdf-pinned-oracle"
)
specforge_set_test_tier(
    specforge_asdf_labeling_spike_native_fixture_test
    extended
)

add_custom_target(specforge_asdf_labeling_interoperability_targets
    DEPENDS specforge_asdf_labeling_spike_native
)

add_test(
    NAME specforge_asdf_labeling_interoperability
    COMMAND
        "${CMAKE_COMMAND}" -E env
        "PYTHONPATH=${CMAKE_BINARY_DIR}/asdf-labeling-spike-python"
        "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/asdf_spike.py"
        interoperability
        --fixtures "${PROJECT_SOURCE_DIR}/tests/fixtures/asdf_labeling"
        --native "$<TARGET_FILE:specforge_asdf_labeling_spike_native>"
        --production-native "$<TARGET_FILE:specforge_sample_labeling_asdf_codec_tests>"
        --output "${CMAKE_BINARY_DIR}/asdf-labeling-spike-results/interoperability.json"
)
set_tests_properties(
    specforge_asdf_labeling_interoperability
    PROPERTIES
        LABELS "asdf;asdf-lifecycle;asdf-pinned-oracle;interoperability;required"
        TIMEOUT 120
)
specforge_set_test_tier(
    specforge_asdf_labeling_interoperability
    extended
)

add_test(
    NAME specforge_sample_labeling_asdf_checksum_interoperability
    COMMAND
        "${CMAKE_COMMAND}" -E env
        "PYTHONPATH=${CMAKE_BINARY_DIR}/asdf-labeling-spike-python"
        "${Python3_EXECUTABLE}"
        "${PROJECT_SOURCE_DIR}/tools/asdf_labeling_hardening/checksum_oracle.py"
        --native "$<TARGET_FILE:specforge_sample_labeling_asdf_codec_tests>"
        --output
            "${CMAKE_BINARY_DIR}/asdf-labeling-hardening-results/checksum-interoperability.json"
        --work-dir
            "${CMAKE_BINARY_DIR}/asdf-labeling-hardening-results/checksum-files"
)
set_tests_properties(
    specforge_sample_labeling_asdf_checksum_interoperability
    PROPERTIES
        LABELS "labeling;asdf-lifecycle;asdf-hardening;asdf-pinned-oracle;ci-headless;required"
        TIMEOUT 120
)
specforge_set_test_tier(
    specforge_sample_labeling_asdf_checksum_interoperability
    extended
)
