if(SPECTIARY_BUILD_SOURCE_MODE STREQUAL "working_tree")
    if(NOT SPECTIARY_BUILD_SOURCE_REVISION STREQUAL "")
        message(FATAL_ERROR
            "SPECTIARY_BUILD_SOURCE_REVISION must be empty when "
            "SPECTIARY_BUILD_SOURCE_MODE is working_tree."
        )
    endif()
    set(SPECTIARY_BUILD_SOURCE_REVISION_JSON "null")
elseif(SPECTIARY_BUILD_SOURCE_MODE STREQUAL "head")
    if(SPECTIARY_BUILD_SOURCE_REVISION STREQUAL "")
        message(FATAL_ERROR
            "SPECTIARY_BUILD_SOURCE_REVISION is required when "
            "SPECTIARY_BUILD_SOURCE_MODE is head."
        )
    endif()

    string(LENGTH "${SPECTIARY_BUILD_SOURCE_REVISION}" source_revision_length)
    if(NOT source_revision_length EQUAL 40 OR
       NOT SPECTIARY_BUILD_SOURCE_REVISION MATCHES "^[0-9a-f]+$")
        message(FATAL_ERROR
            "SPECTIARY_BUILD_SOURCE_REVISION must be a full 40-character "
            "lowercase hexadecimal Git object ID."
        )
    endif()
    set(SPECTIARY_BUILD_SOURCE_REVISION_JSON
        "\"${SPECTIARY_BUILD_SOURCE_REVISION}\""
    )
else()
    message(FATAL_ERROR
        "SPECTIARY_BUILD_SOURCE_MODE must be exactly working_tree or head."
    )
endif()
