# api module (design.md 12.1). Owner: stage 2 team.
# Dependencies are fixed in the root CMakeLists.txt (SND_MODULE_DEPS_api).
snd_add_module(api
  SOURCES
    src/api_module.cpp src/c_api_stub.cpp)

# Composition root: the full runtime library `sonus` (all runtime modules).
# This is the only target allowed to link modules outside the 12.1 table; it
# wires source kinds, the music system, IO, platform devices and live link into
# core at snd_create() time (CONTRACTS.md D-03).
add_library(sonus STATIC compose/compose.cpp)
add_library(sonus::sonus ALIAS sonus)
target_link_libraries(sonus
  PUBLIC sonus_contract_public sonus_api
  PRIVATE sonus_core sonus_source sonus_music sonus_io sonus_codec sonus_dsp sonus_bank
          sonus_jobs sonus_platform sonus_base sonus_livelink)
snd_apply_options(sonus)
