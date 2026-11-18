snd_add_test(api_test
  SOURCES api_smoke_test.cpp
  LINK sonus sonus_mocks)

# Stage 2 api (contract 1.2): the C API end to end through the composition root.
# They link the full `sonus` library; the buildtool (and its test helpers) makes banks.
set(_snd_api_tests api_create_test api_entry_rules_test api_synth_test api_device_test api_threads_test
    api_music_test api_end_to_end_test)
foreach(_t ${_snd_api_tests})
  snd_add_test(${_t}
    SOURCES ${_t}.cpp
    LINK sonus sonus_core sonus_music sonus_source sonus_livelink sonus_platform sonus_io sonus_jobs
         sonus_buildtool sonus_codec sonus_mocks)
  target_include_directories(${_t} PRIVATE ${PROJECT_SOURCE_DIR}/api/compose ${PROJECT_SOURCE_DIR}/buildtool/test
                             ${PROJECT_SOURCE_DIR}/tests/integration)
  set_tests_properties(${_t} PROPERTIES TIMEOUT 600)
endforeach()
