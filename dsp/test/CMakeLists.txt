set(SND_DSP_TEST_SOURCES
  dsp_smoke_test.cpp
  kernels_test.cpp
  resampler_test.cpp
  filter_meter_test.cpp
  effects_test.cpp
  pipeline_bench_test.cpp
  # Independent tests (analytic expectations, guards, edge cases).
  indep_kernels_test.cpp
  indep_resampler_test.cpp
  indep_filter_test.cpp
  indep_pan_meter_test.cpp
  indep_effects_test.cpp
  indep_effects_behaviour_test.cpp)

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

# The dsp tests run minutes of audio (tails, extreme parameters); under
# ThreadSanitizer they exceed the default 120 s.
set_tests_properties(dsp_test dsp_test_scalar PROPERTIES TIMEOUT 1200)

# Contract 1.1 D-48 (stage 1.5 integration).
snd_add_test(dsp_effect_rate_test SOURCES effect_rate_test.cpp LINK sonus_dsp)
# Independent: D-48 for every standard kind, D-44 pitch-ratio limit / resampler pool slot.
snd_add_test(dsp_indep_contract11_test SOURCES indep_contract11_dsp_test.cpp LINK sonus_dsp)
