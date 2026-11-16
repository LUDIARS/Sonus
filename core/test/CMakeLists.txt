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
