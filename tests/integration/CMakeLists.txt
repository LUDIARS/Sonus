# Cross-module tests (design.md 14: golden audio, determinism, load/burst,
# feature coverage). They link the full `sonus` library plus the mocks.
snd_add_test(integration_test
  SOURCES integration_smoke_test.cpp
  LINK sonus sonus_mocks)

# Stage 1.5: buildtool <-> codec / dsp / bank (A1-11). Links the modules directly
# (the composition root `sonus` does not contain the build tool).
snd_add_test(integration_build_pipeline_test
  SOURCES build_pipeline_test.cpp
  LINK sonus_buildtool sonus_bank sonus_codec sonus_dsp sonus_io sonus_jobs sonus_platform sonus_base sonus_mocks)
target_include_directories(integration_build_pipeline_test PRIVATE ${PROJECT_SOURCE_DIR}/buildtool/test)
target_compile_definitions(integration_build_pipeline_test PRIVATE SND_SPEC_DIR="${PROJECT_SOURCE_DIR}/spec")

# Stage 1.5: end to end (buildtool -> memory FS -> IoScheduler -> BankManager ->
# codec -> dsp resampler -> bus effects), golden + analytic checks.
snd_add_test(integration_end_to_end_test
  SOURCES end_to_end_test.cpp
  LINK sonus_buildtool sonus_bank sonus_codec sonus_dsp sonus_io sonus_jobs sonus_platform sonus_base sonus_mocks)
target_include_directories(integration_end_to_end_test PRIVATE ${PROJECT_SOURCE_DIR}/buildtool/test)

# --- Independent tests (contract 1.1 / stage 1.5) ------------------------------------
# buildtool output vs bank-format.md 6.3 / 6.5 with reference code written from the
# spec (PCM bytes, WAV/IMA reference decoder, seek rows, stream files), DSPN vs dsp.
snd_add_test(integration_indep_format11_test
  SOURCES indep_format11_test.cpp
  LINK sonus_buildtool sonus_bank sonus_codec sonus_dsp sonus_io sonus_jobs sonus_platform sonus_base sonus_mocks)
target_include_directories(integration_indep_format11_test PRIVATE ${PROJECT_SOURCE_DIR}/buildtool/test)
set_tests_properties(integration_indep_format11_test PROPERTIES TIMEOUT 600)
# Bus effect parameters from the bank are audible (analytic lowpass response, limiter at its ceiling).
snd_add_test(integration_indep_bus_effects11_test
  SOURCES indep_bus_effects11_test.cpp
  LINK sonus_buildtool sonus_bank sonus_codec sonus_dsp sonus_io sonus_jobs sonus_platform sonus_base sonus_mocks)
target_include_directories(integration_indep_bus_effects11_test PRIVATE ${PROJECT_SOURCE_DIR}/buildtool/test)
# Re-check of D-54: the runtime's DSP reservation (compute hook, fallback, refusals).
snd_add_test(integration_indep_dsp_reserve11_test
  SOURCES indep_dsp_reserve11_test.cpp
  LINK sonus_buildtool sonus_bank sonus_codec sonus_dsp sonus_io sonus_jobs sonus_platform sonus_base sonus_mocks)
target_include_directories(integration_indep_dsp_reserve11_test PRIVATE ${PROJECT_SOURCE_DIR}/buildtool/test)
