if(SPECFORGE_BUILD_SOURCE_MODE STREQUAL "working_tree")
    if(NOT SPECFORGE_BUILD_SOURCE_REVISION STREQUAL "")
        message(FATAL_ERROR
            "SPECFORGE_BUILD_SOURCE_REVISION must be empty when "
            "SPECFORGE_BUILD_SOURCE_MODE is working_tree."
        )
    endif()
    set(SPECFORGE_BUILD_SOURCE_REVISION_JSON "null")
elseif(SPECFORGE_BUILD_SOURCE_MODE STREQUAL "head")
    if(SPECFORGE_BUILD_SOURCE_REVISION STREQUAL "")
        message(FATAL_ERROR
            "SPECFORGE_BUILD_SOURCE_REVISION is required when "
            "SPECFORGE_BUILD_SOURCE_MODE is head."
        )
    endif()

    string(LENGTH "${SPECFORGE_BUILD_SOURCE_REVISION}" source_revision_length)
    if(NOT source_revision_length EQUAL 40 OR
       NOT SPECFORGE_BUILD_SOURCE_REVISION MATCHES "^[0-9a-f]+$")
        message(FATAL_ERROR
            "SPECFORGE_BUILD_SOURCE_REVISION must be a full 40-character "
            "lowercase hexadecimal Git object ID."
        )
    endif()
    set(SPECFORGE_BUILD_SOURCE_REVISION_JSON
        "\"${SPECFORGE_BUILD_SOURCE_REVISION}\""
    )
else()
    message(FATAL_ERROR
        "SPECFORGE_BUILD_SOURCE_MODE must be exactly working_tree or head."
    )
endif()
