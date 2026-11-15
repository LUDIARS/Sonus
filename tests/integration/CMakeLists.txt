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
