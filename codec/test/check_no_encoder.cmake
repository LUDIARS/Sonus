# Fails if the runtime-only test executable contains the build-time encoder.
#   cmake -DNM=<nm> -DEXE=<executable> -P check_no_encoder.cmake
execute_process(COMMAND "${NM}" -C "${EXE}" OUTPUT_VARIABLE syms RESULT_VARIABLE rc)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "nm failed on ${EXE}")
endif()
if(NOT syms MATCHES "snd::codec::BuiltinCodec")
  message(FATAL_ERROR "symbol check is broken: runtime decoder symbols not found in ${EXE}")
endif()
if(syms MATCHES "snd::codec::encode::")
  message(FATAL_ERROR "encoder symbols are linked into a runtime-only executable")
endif()
message(STATUS "no encoder symbols in ${EXE}")
