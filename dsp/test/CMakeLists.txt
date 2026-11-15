set(SND_DSP_TEST_SOURCES
  dsp_smoke_test.cpp
  kernels_test.cpp
  resampler_test.cpp
  filter_meter_test.cpp
  effects_test.cpp
  pipeline_bench_test.cpp)

snd_add_test(dsp_test
  SOURCES ${SND_DSP_TEST_SOURCES}
  LINK sonus_dsp sonus_mocks)

# Same tests against the dsp sources built with the scalar SIMD path forced
# (snd/platform/simd.hpp). Both executables compare with the same golden files,
# which checks scalar and SIMD results against each other within tolerance.
list(TRANSFORM SND_DSP_SOURCES PREPEND "${CMAKE_CURRENT_SOURCE_DIR}/../" OUTPUT_VARIABLE _snd_dsp_src_abs)
snd_add_test(dsp_test_scalar
  SOURCES ${SND_DSP_TEST_SOURCES} ${_snd_dsp_src_abs}
  LINK sonus_base sonus_platform sonus_mocks)
target_include_directories(dsp_test_scalar PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../include
                                                   ${CMAKE_CURRENT_SOURCE_DIR}/../src)
target_compile_definitions(dsp_test_scalar PRIVATE SND_SIMD_FORCE_SCALAR=1)
