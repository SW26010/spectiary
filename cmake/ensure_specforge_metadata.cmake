if(NOT DEFINED SPECFORGE_EXECUTABLE OR
   NOT DEFINED SPECFORGE_METADATA OR
   NOT DEFINED SPECFORGE_FINALIZER)
    message(FATAL_ERROR
        "SpecForge metadata freshness check requires executable, metadata, and finalizer paths.")
endif()

if(NOT EXISTS "${SPECFORGE_EXECUTABLE}")
    message(FATAL_ERROR
        "SpecForge executable is missing: ${SPECFORGE_EXECUTABLE}")
endif()
if(NOT EXISTS "${SPECFORGE_FINALIZER}")
    message(FATAL_ERROR
        "SpecForge metadata finalizer is missing: ${SPECFORGE_FINALIZER}")
endif()

file(SHA256 "${SPECFORGE_EXECUTABLE}" executable_sha256)
set(metadata_current FALSE)
if(EXISTS "${SPECFORGE_METADATA}")
    file(READ "${SPECFORGE_METADATA}" metadata_json)
    string(JSON schema_version ERROR_VARIABLE schema_error
        GET "${metadata_json}" schema_version)
    string(JSON completed_at_utc ERROR_VARIABLE completed_at_error
        GET "${metadata_json}" build completed_at_utc)
    string(JSON artifact_file ERROR_VARIABLE artifact_file_error
        GET "${metadata_json}" artifact file)
    string(JSON artifact_sha256 ERROR_VARIABLE artifact_sha256_error
        GET "${metadata_json}" artifact sha256)
    if(NOT schema_error AND
       NOT completed_at_error AND
       NOT artifact_file_error AND
       NOT artifact_sha256_error AND
       schema_version STREQUAL "5" AND
       completed_at_utc AND
       artifact_file STREQUAL "SpecForge.exe" AND
       artifact_sha256 STREQUAL "${executable_sha256}")
        set(metadata_current TRUE)
    endif()
endif()

if(NOT metadata_current)
    execute_process(
        COMMAND "${SPECFORGE_FINALIZER}"
            "${SPECFORGE_EXECUTABLE}"
            "${SPECFORGE_METADATA}"
        RESULT_VARIABLE finalizer_result
        OUTPUT_VARIABLE finalizer_output
        ERROR_VARIABLE finalizer_error)
    if(NOT finalizer_result EQUAL 0)
        message(FATAL_ERROR
            "SpecForge metadata finalization failed with exit code ${finalizer_result}.\n"
            "stdout: ${finalizer_output}\n"
            "stderr: ${finalizer_error}")
    endif()
endif()
