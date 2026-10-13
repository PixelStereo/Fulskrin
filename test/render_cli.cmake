# pinol --render exit codes (ctest -R render_cli): a project that cannot be loaded fails with 3 and writes nothing;
# no project renders the empty composition and succeeds.
#   cmake -DPINOL=<path of the executable> -DOUT=<folder> -P test/render_cli.cmake
file(MAKE_DIRECTORY "${OUT}")
file(REMOVE "${OUT}/bad.png" "${OUT}/empty.png")
file(WRITE "${OUT}/broken.pinol" "{ not json")

execute_process(COMMAND "${PINOL}" --render "${OUT}/bad.png" --frames 2 "${OUT}/broken.pinol" RESULT_VARIABLE rc
                OUTPUT_QUIET ERROR_QUIET)
if(NOT rc EQUAL 3 OR EXISTS "${OUT}/bad.png")
  message(FATAL_ERROR "broken project: exit code ${rc} (3 expected), output written: ${OUT}/bad.png")
endif()
execute_process(COMMAND "${PINOL}" --render "${OUT}/bad.png" --frames 2 "${OUT}/missing.pinol" RESULT_VARIABLE rc
                OUTPUT_QUIET ERROR_QUIET)
if(NOT rc EQUAL 3)
  message(FATAL_ERROR "missing project: exit code ${rc} (3 expected)")
endif()
execute_process(COMMAND "${PINOL}" --render "${OUT}/empty.png" --frames 2 RESULT_VARIABLE rc OUTPUT_QUIET ERROR_QUIET)
if(NOT rc EQUAL 0 OR NOT EXISTS "${OUT}/empty.png")
  message(FATAL_ERROR "no project: exit code ${rc} (0 expected), output written: ${OUT}/empty.png")
endif()
