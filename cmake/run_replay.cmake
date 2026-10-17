# Aufruf: cmake -DEXE=... -DINPUT=... -DEXPECTED=... -DOUTPUT=... -P run_replay.cmake
get_filename_component(out_dir ${OUTPUT} DIRECTORY)
file(MAKE_DIRECTORY ${out_dir})
execute_process(
    COMMAND ${EXE} ${INPUT}
    OUTPUT_FILE ${OUTPUT}
    RESULT_VARIABLE rc)
if(NOT rc EQUAL 0)
    message(FATAL_ERROR "${EXE} exited with ${rc}")
endif()
execute_process(
    COMMAND ${CMAKE_COMMAND} -E compare_files ${OUTPUT} ${EXPECTED}
    RESULT_VARIABLE diff)
if(NOT diff EQUAL 0)
    file(READ ${OUTPUT} actual)
    file(READ ${EXPECTED} expected)
    message(FATAL_ERROR "Replay mismatch for ${INPUT}\n--- expected ---\n${expected}--- actual ---\n${actual}")
endif()
