find_package(yaml-cpp CONFIG REQUIRED)

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
