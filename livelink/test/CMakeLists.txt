set(_ll_test_link sonus_livelink sonus_core sonus_bank sonus_dsp sonus_jobs sonus_io sonus_codec sonus_platform
    sonus_base sonus_mocks)
snd_add_test(livelink_test SOURCES livelink_unit_test.cpp LINK ${_ll_test_link})
# Fuzz (design.md 14). Scale with SND_FUZZ_ITERATIONS; run under SND_SANITIZE=address,undefined.
snd_add_test(livelink_fuzz_test SOURCES livelink_fuzz_test.cpp LINK ${_ll_test_link})
set_tests_properties(livelink_fuzz_test PROPERTIES TIMEOUT 600)
