execute_process(COMMAND "${PROGRAM}"
    RESULT_VARIABLE clean_result OUTPUT_VARIABLE clean_out ERROR_VARIABLE clean_err TIMEOUT 10)
if(NOT clean_result STREQUAL "0" OR NOT clean_out MATCHES "value=42")
    message(FATAL_ERROR "ASan runtime clean probe failed: ${clean_result}\n${clean_out}\n${clean_err}")
endif()
execute_process(COMMAND "${PROGRAM}" overflow
    RESULT_VARIABLE bad_result OUTPUT_VARIABLE bad_out ERROR_VARIABLE bad_err TIMEOUT 10)
if(bad_result STREQUAL "0" OR bad_result MATCHES "timeout" OR
    NOT "${bad_out}${bad_err}" MATCHES "AddressSanitizer: heap-buffer-overflow")
    message(FATAL_ERROR "ASan must report and fail a heap overflow: ${bad_result}\n${bad_out}\n${bad_err}")
endif()
message(STATUS "ASan runtime detects heap overflow and returns failure.")
