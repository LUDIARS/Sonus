snd_add_test(physics_test
  SOURCES physics_smoke_test.cpp
  LINK sonus_physics sonus_mocks)

# Unit tests of the physics module (design.md 9, 14). They drive a real system through the
# C API (`sonus`, manual output device) and make banks with the buildtool; linking past the
# 12.1 table is allowed for tests only.
set(_snd_physics_tests
  physics_culling_test
  physics_resolution_test
  physics_footstep_test
  physics_tracking_test
  physics_bank_test
  physics_playback_test
  physics_api_test)
foreach(_t ${_snd_physics_tests})
  snd_add_test(${_t}
    SOURCES ${_t}.cpp
    LINK sonus_physics sonus sonus_buildtool sonus_codec sonus_mocks)
  target_include_directories(${_t} PRIVATE ${PROJECT_SOURCE_DIR}/buildtool/test)
  set_tests_properties(${_t} PROPERTIES TIMEOUT 600)
endforeach()
