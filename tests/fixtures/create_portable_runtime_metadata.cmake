cmake_minimum_required(VERSION 3.24)

foreach(required_variable IN ITEMS INPUT OUTPUT)
    if(NOT DEFINED ${required_variable} OR
       "${${required_variable}}" STREQUAL "")
        message(FATAL_ERROR
            "${required_variable} is required."
        )
    endif()
endforeach()

if(NOT EXISTS "${INPUT}")
    message(FATAL_ERROR
        "Source SpecForge metadata does not exist: ${INPUT}"
    )
endif()

file(READ "${INPUT}" portable_metadata)
string(JSON schema_type
    ERROR_VARIABLE schema_error
    TYPE "${portable_metadata}" schema_version
)
if(NOT schema_error STREQUAL "NOTFOUND" OR
   NOT schema_type STREQUAL "NUMBER")
    message(FATAL_ERROR
        "Source SpecForge metadata has no numeric schema_version: "
        "${schema_error}"
    )
endif()

string(JSON portable_metadata
    SET "${portable_metadata}" deployment "{}"
)
string(JSON portable_metadata
    SET "${portable_metadata}" deployment distribution "\"portable\""
)
string(JSON portable_metadata
    SET "${portable_metadata}" deployment storage_profile "\"portable\""
)

string(JSON schema_version
    GET "${portable_metadata}" schema_version
)
if(NOT schema_version STREQUAL "5")
    message(FATAL_ERROR
        "Portable runtime metadata must use schema 5; found ${schema_version}."
    )
endif()

get_filename_component(output_directory "${OUTPUT}" DIRECTORY)
file(MAKE_DIRECTORY "${output_directory}")
file(WRITE "${OUTPUT}" "${portable_metadata}\n")
