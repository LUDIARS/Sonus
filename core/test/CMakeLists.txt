set(_core_test_link sonus_core sonus_buildtool sonus_bank sonus_dsp sonus_jobs sonus_io sonus_codec sonus_mocks)

snd_add_test(core_test
  SOURCES core_smoke_test.cpp core_basic_test.cpp core_mix_test.cpp core_voice_test.cpp core_system_test.cpp core_music_test.cpp
  LINK ${_core_test_link})

# Parallel rendering and late chunks (D-43). Repeat under TSan / Release:
#   ctest --repeat until-fail:20 -R core_parallel_test
snd_add_test(core_parallel_test SOURCES core_parallel_test.cpp LINK ${_core_test_link})
set_tests_properties(core_parallel_test PROPERTIES TIMEOUT 300)

# Load: 128 real voices < 30 % of the block (enforced in Release), 4x the limit.
snd_add_test(core_load_test SOURCES core_load_test.cpp LINK ${_core_test_link})
set_tests_properties(core_load_test PROPERTIES TIMEOUT 300)

# Independent tests (stage 2 independent tester): own source kind, analytic checks.
snd_add_test(indep_core_cmd_test SOURCES indep_core_cmd_test.cpp LINK ${_core_test_link})
snd_add_test(indep_core_mix_test SOURCES indep_core_mix_test.cpp LINK ${_core_test_link})
snd_add_test(indep_core_parallel_test SOURCES indep_core_parallel_test.cpp LINK ${_core_test_link})
set_tests_properties(indep_core_parallel_test PROPERTIES TIMEOUT 300)
snd_add_test(indep_core_bank_test SOURCES indep_core_bank_test.cpp LINK ${_core_test_link})
snd_add_test(indep_core_fixcheck_test SOURCES indep_core_fixcheck_test.cpp LINK ${_core_test_link})
set_tests_properties(indep_core_fixcheck_test PROPERTIES TIMEOUT 300)
snd_add_test(indep_core_fix2check_test SOURCES indep_core_fix2check_test.cpp LINK ${_core_test_link})
set_tests_properties(indep_core_fix2check_test PROPERTIES TIMEOUT 600)
snd_add_test(indep_core_load_test SOURCES indep_core_load_test.cpp LINK ${_core_test_link})
set_tests_properties(indep_core_load_test PROPERTIES TIMEOUT 600)
# Defects the independent tester found in stage 2, now fixed: one executable
# each (the source selects its test with SND_INDEP_FIXED_<key>). Names are the
# former Defect_* names without the prefix (they name the defect they guard).
function(snd_indep_core_fixed name key)
  snd_add_test(${name} SOURCES indep_core_fixed_test.cpp LINK ${_core_test_link})
  target_compile_definitions(${name} PRIVATE SND_INDEP_FIXED_${key}=1)
endfunction()
snd_indep_core_fixed(core_record_order_reserve_before_normal record_order)
snd_indep_core_fixed(core_unregister_loses_unpublished_commands unregister)
snd_indep_core_fixed(core_timed_stop_fades_before_its_clock timed_stop)
snd_indep_core_fixed(core_epoch_ignores_untracked_jobs epoch_untracked)
snd_indep_core_fixed(core_control_on_held_voice control_held)
snd_indep_core_fixed(core_language_switch_back_while_old_bank_unloading language_back)
snd_indep_core_fixed(core_scheduled_start_in_apply_block_never_starts start_in_apply_block)
# Defects found re-checking the stage-2 fixes, now fixed (a3dfbb6): one
# executable each; names are the former Defect_* names without the prefix.
function(snd_indep_core_fixed2 name key)
  snd_add_test(${name} SOURCES indep_core_fixed2_test.cpp LINK ${_core_test_link})
  target_compile_definitions(${name} PRIVATE SND_INDEP_FIXED2_${key}=1)
endfunction()
snd_indep_core_fixed2(core_reregistered_thread_reorders_its_commands reregister)
snd_indep_core_fixed2(core_timed_stop_of_paused_playback_sounds paused_timed_stop)

# Stage 3, session B group 4 (voice management, sequences, notifications, analysis).
snd_add_test(core_g4_test SOURCES core_voice2_test.cpp core_sequence2_test.cpp core_notify2_test.cpp core_analysis_test.cpp
  core_g4_fix1_test.cpp
  LINK ${_core_test_link})
# Determinism (serial / parallel), no allocation, RT safety of the new paths. Repeat under
# TSan / Release: ctest --repeat until-fail:20 -R core_g4_system_test
snd_add_test(core_g4_system_test SOURCES core_g4_system_test.cpp LINK ${_core_test_link})
set_tests_properties(core_g4_system_test PROPERTIES TIMEOUT 300)
# Load: 4x the limit over steal modes; 128 voices with loudness and spectrum (< 30 % in Release).
snd_add_test(core_g4_load_test SOURCES core_g4_load_test.cpp LINK ${_core_test_link})
set_tests_properties(core_g4_load_test PROPERTIES TIMEOUT 600)

# Stage 3 group 4, independent tester (own fixture indep_g4_fixture.hpp). Defect_* tests fail
# on purpose until the implementation is fixed.
snd_add_test(indep_g4_seq_test SOURCES indep_g4_seq_test.cpp LINK ${_core_test_link})
set_tests_properties(indep_g4_seq_test PROPERTIES TIMEOUT 600)
snd_add_test(indep_g4_voice_test SOURCES indep_g4_voice_test.cpp LINK ${_core_test_link})
set_tests_properties(indep_g4_voice_test PROPERTIES TIMEOUT 600)
snd_add_test(indep_g4_analysis_test SOURCES indep_g4_analysis_test.cpp LINK ${_core_test_link})
set_tests_properties(indep_g4_analysis_test PROPERTIES TIMEOUT 600)
snd_add_test(indep_g4_system_test SOURCES indep_g4_system_test.cpp LINK ${_core_test_link})
set_tests_properties(indep_g4_system_test PROPERTIES TIMEOUT 600)
snd_add_test(indep_g4_recheck_test SOURCES indep_g4_recheck_test.cpp LINK ${_core_test_link})
