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
# Opus (D-56): the libopus encoder stays out of the runtime as well; only the
# decoder is linked (when the build has Opus).
if(syms MATCHES "opus_encode|opus_multistream_encode|opus_multistream_surround_encoder|silk_Encode|celt_encode")
  message(FATAL_ERROR "libopus encoder symbols are linked into a runtime-only executable")
endif()
message(STATUS "no encoder symbols in ${EXE}")
