set(_core_test_link sonus_core sonus_buildtool sonus_bank sonus_dsp sonus_jobs sonus_io sonus_codec sonus_mocks)

snd_add_test(core_test
  SOURCES core_smoke_test.cpp core_basic_test.cpp
  LINK ${_core_test_link})
