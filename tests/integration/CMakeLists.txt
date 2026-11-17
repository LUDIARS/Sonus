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

# Stage 2: end to end through core (project -> bank -> IoScheduler -> core -> output).
snd_add_test(integration_core_end_to_end_test
  SOURCES core_end_to_end_test.cpp
  LINK sonus_core sonus_buildtool sonus_bank sonus_codec sonus_dsp sonus_io sonus_jobs sonus_platform sonus_base sonus_mocks)
target_include_directories(integration_core_end_to_end_test PRIVATE ${PROJECT_SOURCE_DIR}/buildtool/test ${PROJECT_SOURCE_DIR}/core/test)

# Opus (D-56..D-60): buildtool -> bank (ParseBank) -> codec, analytic checks; with
# SND_CODEC_OPUS=OFF it checks that the build rejects OPUS assets.
snd_add_test(integration_opus_pipeline_test
  SOURCES opus_pipeline_test.cpp
  LINK sonus_buildtool sonus_bank sonus_codec sonus_dsp sonus_io sonus_jobs sonus_platform sonus_base sonus_mocks)
target_include_directories(integration_opus_pipeline_test PRIVATE ${PROJECT_SOURCE_DIR}/buildtool/test)
set_tests_properties(integration_opus_pipeline_test PROPERTIES TIMEOUT 600)

# Stage 2, source: project -> buildtool -> bank + stream file -> IoScheduler -> core
# with the Sample / Stream kinds of the composition root (compose::SourceKinds).
snd_add_test(integration_source_end_to_end_test
  SOURCES source_end_to_end_test.cpp
  LINK sonus sonus_core sonus_buildtool sonus_bank sonus_codec sonus_dsp sonus_io sonus_jobs sonus_platform sonus_base sonus_mocks)
target_include_directories(integration_source_end_to_end_test PRIVATE ${PROJECT_SOURCE_DIR}/buildtool/test)
set_tests_properties(integration_source_end_to_end_test PROPERTIES TIMEOUT 600)

# Independent tests of the source module through core (indep_*), harness of source/test.
snd_add_test(integration_indep_source_core_test
  SOURCES indep_source_core_test.cpp
  LINK sonus_source sonus_core sonus_buildtool sonus_bank sonus_dsp sonus_jobs sonus_io sonus_codec sonus_mocks)
target_include_directories(integration_indep_source_core_test PRIVATE ${PROJECT_SOURCE_DIR}/buildtool/test ${PROJECT_SOURCE_DIR}/source/test)
set_tests_properties(integration_indep_source_core_test PROPERTIES TIMEOUT 900)
