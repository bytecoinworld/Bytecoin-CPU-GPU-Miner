execute_process(
    COMMAND "${MINER}" ${ARGS}
    RESULT_VARIABLE actual_result
    OUTPUT_VARIABLE stdout
    ERROR_VARIABLE stderr
)

if(NOT "${actual_result}" STREQUAL "${EXPECTED_RESULT}")
    message(FATAL_ERROR
        "${ARG} returned ${actual_result}; expected ${EXPECTED_RESULT}.\n"
        "stdout: ${stdout}\n"
        "stderr: ${stderr}"
    )
endif()

string(CONCAT output "${stdout}" "${stderr}")
string(FIND "${output}" "${EXPECTED_OUTPUT}" output_position)
if(output_position EQUAL -1)
    message(FATAL_ERROR
        "${ARG} output did not contain '${EXPECTED_OUTPUT}'.\n"
        "stdout: ${stdout}\n"
        "stderr: ${stderr}"
    )
endif()
