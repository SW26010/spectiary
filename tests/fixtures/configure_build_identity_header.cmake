foreach(required_variable IN ITEMS
    SOURCE_ROOT
    OUTPUT
    EXPECTED_MODE
    EXPECTED_REVISION
    EXPECTED_VERSION
    EXPECTED_RELEASE_PROFILE
    EXPECTED_CONFIGURATION
    EXPECTED_ARCHITECTURE
)
    if(NOT DEFINED ${required_variable})
        message(FATAL_ERROR "${required_variable} is required.")
    endif()
endforeach()

set(SPECFORGE_BUILD_SOURCE_MODE "${EXPECTED_MODE}")
set(SPECFORGE_BUILD_SOURCE_REVISION "${EXPECTED_REVISION}")
set(PROJECT_VERSION "${EXPECTED_VERSION}")
set(SPECFORGE_RELEASE_PROFILE "${EXPECTED_RELEASE_PROFILE}")
set(SPECFORGE_BUILD_TARGET_ARCHITECTURE "${EXPECTED_ARCHITECTURE}")
include("${SOURCE_ROOT}/cmake/specforge_build_source.cmake")

configure_file(
    "${SOURCE_ROOT}/cmake/specforge_build_identity.h.in"
    "${OUTPUT}"
    @ONLY
)
file(READ "${OUTPUT}" generated_header)
string(REPLACE
    "$<CONFIG>"
    "${EXPECTED_CONFIGURATION}"
    generated_header
    "${generated_header}"
)
file(WRITE "${OUTPUT}" "${generated_header}")

file(READ "${OUTPUT}" generated_header)
foreach(expected_text IN ITEMS
    "kSpecForgeVersion[] = \"${EXPECTED_VERSION}\""
    "kReleaseProfile[] = \"${EXPECTED_RELEASE_PROFILE}\""
    "kBuildConfiguration[] = \"${EXPECTED_CONFIGURATION}\""
    "kTargetArchitecture[] = \"${EXPECTED_ARCHITECTURE}\""
    "kBuildSourceMode[] = \"${EXPECTED_MODE}\""
    "kBuildSourceRevision[] = \"${EXPECTED_REVISION}\""
)
    string(FIND "${generated_header}" "${expected_text}" expected_text_index)
    if(expected_text_index EQUAL -1)
        message(FATAL_ERROR
            "Generated build identity header is missing: ${expected_text}"
        )
    endif()
endforeach()
