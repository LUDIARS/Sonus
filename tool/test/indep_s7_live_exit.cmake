# Independent tests of group P: runs a command and checks its exit status (sonus-replay: 0 ok,
# 1 mismatch, 2 usage / input error). -DCMD=<program> -DARGS=<;-list> -DEXPECT=<status>
execute_process(COMMAND ${CMD} ${ARGS} RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
if(NOT "${rc}" STREQUAL "${EXPECT}")
  message(FATAL_ERROR "exit status ${rc}, expected ${EXPECT}\n${out}\n${err}")
endif()
