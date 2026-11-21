# Preview engine and replay host (group P; design.md 11.1 試聴, 11.3): a Sonus runtime inside the
# tool. Not a module of the 12.1 table: like the composition root `sonus` (api/compose, D-93) it
# composes the runtime modules for the tool (source kinds, music, IO, core, live link), so the
# tool's core (sonus_tool, deps buildtool;dsp;core;livelink) stays inside its row of the table and
# only this library links the rest. Built with SND_BUILD_TOOL_CORE or SND_BUILD_TOOL; no Tela.
add_library(sonus_tool_preview STATIC
  src/memory_fs.cpp
  src/runtime_host.cpp
  src/preview_engine.cpp
  src/replay_host.cpp)
add_library(sonus::tool_preview ALIAS sonus_tool_preview)
add_library(Sonus::tool_preview ALIAS sonus_tool_preview)
target_include_directories(sonus_tool_preview
  PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
snd_apply_options(sonus_tool_preview)
target_compile_features(sonus_tool_preview INTERFACE cxx_std_20)
target_link_libraries(sonus_tool_preview
  PUBLIC sonus_tool sonus_contract
  PRIVATE sonus sonus_core sonus_source sonus_music sonus_io sonus_codec sonus_dsp sonus_bank sonus_jobs
          sonus_livelink sonus_buildtool sonus_platform sonus_base)
# The public headers name livelink and buildtool types (ReplayInfo, DocBuild).
target_include_directories(sonus_tool_preview PUBLIC
  ${PROJECT_SOURCE_DIR}/livelink/include ${PROJECT_SOURCE_DIR}/buildtool/include)

# sonus-replay: replays recordings and bug reports without a UI (tests, CI, a bug's reporter).
add_executable(sonus-replay cli/sonus_replay.cpp)
snd_apply_options(sonus-replay)
target_link_libraries(sonus-replay PRIVATE sonus_tool_preview sonus_tool sonus_livelink sonus_buildtool)
