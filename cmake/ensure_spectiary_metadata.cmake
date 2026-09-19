if(NOT DEFINED SPECTIARY_EXECUTABLE OR
   NOT DEFINED SPECTIARY_METADATA OR
   NOT DEFINED SPECTIARY_FINALIZER)
    message(FATAL_ERROR
        "Spectiary metadata freshness check requires executable, metadata, and finalizer paths.")
endif()

if(NOT EXISTS "${SPECTIARY_EXECUTABLE}")
    message(FATAL_ERROR
        "Spectiary executable is missing: ${SPECTIARY_EXECUTABLE}")
endif()
if(NOT EXISTS "${SPECTIARY_FINALIZER}")
    message(FATAL_ERROR
        "Spectiary metadata finalizer is missing: ${SPECTIARY_FINALIZER}")
endif()

file(READ "${CMAKE_CURRENT_LIST_DIR}/../config/project_identity.json" identity_json)
string(JSON application_id GET "${identity_json}" founding_identity)
string(JSON artifact_basename GET "${identity_json}" artifact_basename)
file(SHA256 "${SPECTIARY_EXECUTABLE}" executable_sha256)
set(metadata_current FALSE)
if(EXISTS "${SPECTIARY_METADATA}")
    file(READ "${SPECTIARY_METADATA}" metadata_json)
    string(JSON schema_version ERROR_VARIABLE schema_error
        GET "${metadata_json}" schema_version)
    string(JSON metadata_application_id ERROR_VARIABLE identity_error
        GET "${metadata_json}" application_id)
    string(JSON completed_at_utc ERROR_VARIABLE completed_at_error
        GET "${metadata_json}" build completed_at_utc)
    string(JSON artifact_file ERROR_VARIABLE artifact_file_error
        GET "${metadata_json}" artifact file)
    string(JSON artifact_sha256 ERROR_VARIABLE artifact_sha256_error
        GET "${metadata_json}" artifact sha256)
    if(NOT schema_error AND NOT identity_error AND
       metadata_application_id STREQUAL application_id AND
       NOT completed_at_error AND
       NOT artifact_file_error AND
       NOT artifact_sha256_error AND
       schema_version STREQUAL "6" AND
       completed_at_utc AND
       artifact_file STREQUAL "${artifact_basename}.exe" AND
       artifact_sha256 STREQUAL "${executable_sha256}")
        set(metadata_current TRUE)
    endif()
endif()

if(NOT metadata_current)
    execute_process(
        COMMAND "${SPECTIARY_FINALIZER}"
            "${SPECTIARY_EXECUTABLE}"
            "${SPECTIARY_METADATA}"
        RESULT_VARIABLE finalizer_result
        OUTPUT_VARIABLE finalizer_output
        ERROR_VARIABLE finalizer_error)
    if(NOT finalizer_result EQUAL 0)
        message(FATAL_ERROR
            "Spectiary metadata finalization failed with exit code ${finalizer_result}.\n"
            "stdout: ${finalizer_output}\n"
            "stderr: ${finalizer_error}")
    endif()
endif()
