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

# Stage 4: music through core (buildtool -> bank -> core + music system -> output).
# Analytic checks: sample-exact segment boundaries, beat clocks, fade shapes,
# transitions, playlists, stems, stalls, determinism and RT safety.
set(_music_test_link sonus_music sonus_core sonus_buildtool sonus_bank sonus_codec sonus_dsp sonus_io sonus_jobs
    sonus_platform sonus_base sonus_mocks)
function(snd_music_test name)
  snd_add_test(${name} SOURCES ${name}.cpp LINK ${_music_test_link})
  target_include_directories(${name} PRIVATE ${PROJECT_SOURCE_DIR}/buildtool/test)
  set_tests_properties(${name} PROPERTIES TIMEOUT 600)
endfunction()
snd_music_test(music_segment_test)
snd_music_test(music_transition_test)
snd_music_test(music_transport_test)
snd_music_test(music_system_test)
# Independent music tests (stage 4, written apart from the implementation; properties in
# music_indep_properties.md). Defect_music_* tests fail on purpose until the defect is fixed;
# fixed ones lose the prefix (music_indep_fixed_*).
snd_music_test(music_indep_test)
snd_music_test(music_indep_recheck_test)
foreach(n stall_beat_twice state_lost_while_paused seek_before_signature_change samebar_other_signature shuffle_over_32)
  snd_music_test(music_indep_fixed_${n}_test)
endforeach()
function(snd_music_defect name)
  snd_add_test(Defect_music_${name}_test SOURCES defect_music_${name}_test.cpp LINK ${_music_test_link})
  target_include_directories(Defect_music_${name}_test PRIVATE ${PROJECT_SOURCE_DIR}/buildtool/test)
  set_tests_properties(Defect_music_${name}_test PROPERTIES TIMEOUT 600)
endfunction()
snd_music_defect(long_stall_beat_early)
