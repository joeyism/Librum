if(NOT PATCH_FILE OR NOT WORK_DIR)
    message(FATAL_ERROR "PATCH_FILE and WORK_DIR must be set")
endif()

execute_process(
    COMMAND patch -N -p1 --forward -r -
    INPUT_FILE "${PATCH_FILE}"
    WORKING_DIRECTORY "${WORK_DIR}"
    RESULT_VARIABLE rv
    OUTPUT_VARIABLE out
    ERROR_VARIABLE err
)
# 0 = applied, 1 = already applied (-N)
if(NOT rv EQUAL 0 AND NOT rv EQUAL 1)
    message(FATAL_ERROR "Failed to apply MuPDF wrap patch (${rv}):\n${out}\n${err}")
endif()
