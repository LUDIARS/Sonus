# api module (design.md 12.1). Owner: stage 2 team.
# Dependencies are fixed in the root CMakeLists.txt (SND_MODULE_DEPS_api).
snd_add_module(api
  SOURCES
    src/api_module.cpp)

# Composition root: the full runtime library `sonus` (all runtime modules).
# This is the only target allowed to link modules outside the 12.1 table. It
# holds the C API (snd/sonus.h) and wires source kinds, the music system, IO,
# platform devices and the live link into core at snd_create() time
# (CONTRACTS.md D-03, D-93). The C API entry checks use the live link's shared
# rule table (livelink/command_check.hpp, D-84), so the C API lives here too.
add_library(sonus STATIC
  compose/compose.cpp
  compose/config_check.cpp
  compose/c_api.cpp)
add_library(sonus::sonus ALIAS sonus)
target_include_directories(sonus PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/compose)
target_link_libraries(sonus
  PUBLIC sonus_contract_public sonus_api
  PRIVATE sonus_core sonus_source sonus_music sonus_io sonus_codec sonus_dsp sonus_bank
          sonus_jobs sonus_platform sonus_base sonus_livelink)
snd_apply_options(sonus)

# Sonus::sonus -- the window for a game engine that takes Sonus in (add_subdirectory,
# CONTRACTS.md D-100): the C API (snd/sonus.h and the other public headers) and the
# composition root with every runtime module. Unlike `sonus` it carries no compile
# feature: sonus_contract_public asks for cxx_std_20 (the C++ wrapper snd/sonus.hpp needs
# it), which would turn a C++17 engine's files into C++20. The C headers are C11 / C++17
# clean; snd/sonus.hpp still needs C++20 where it is included. $<LINK_ONLY:> links the
# library and its static dependencies without their usage requirements.
add_library(sonus_c_api INTERFACE)
target_include_directories(sonus_c_api INTERFACE
  $<TARGET_PROPERTY:sonus_contract_public,INTERFACE_INCLUDE_DIRECTORIES>)
target_link_libraries(sonus_c_api INTERFACE $<LINK_ONLY:sonus>)
add_library(Sonus::sonus ALIAS sonus_c_api)
