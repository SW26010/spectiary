foreach(required_variable IN ITEMS
    SOURCE_ROOT
    OUTPUT
    EXPECTED_MODE
    EXPECTED_REVISION
)
    if(NOT DEFINED ${required_variable})
        message(FATAL_ERROR "${required_variable} is required.")
    endif()
endforeach()

set(SPECFORGE_BUILD_SOURCE_MODE "${EXPECTED_MODE}")
set(SPECFORGE_BUILD_SOURCE_REVISION "${EXPECTED_REVISION}")
include("${SOURCE_ROOT}/cmake/specforge_build_source.cmake")

configure_file(
    "${SOURCE_ROOT}/cmake/specforge_build_identity.h.in"
    "${OUTPUT}"
    @ONLY
)

file(READ "${OUTPUT}" generated_header)
foreach(expected_text IN ITEMS
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
