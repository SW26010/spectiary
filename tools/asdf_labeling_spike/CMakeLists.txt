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
        LABELS "asdf;interoperability;required"
        TIMEOUT 120
)
