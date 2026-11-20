# Authoring tool (design.md 11; stage 2: mixer and profiler).
#   -DSND_BUILD_TOOL_CORE=ON: the UI-independent core only (sonus_tool), no Tela (D-101).
#   -DSND_BUILD_TOOL=ON:      the core, the UI and the sonus-tool executable. The UI uses
#                             LUDIARS Tela (CONTRACTS.md D-67), found with find_package
#                             (Tela_DIR or CMAKE_PREFIX_PATH); it is never bundled or fetched.
# The tests in test/ follow the same split (core tests without Tela, UI tests with it).

# --- sonus_tool: the UI-independent core (models: connection, session, mixer, value sync,
# profiler, edit history, recording files). Dependencies from the root table
# (SND_MODULE_DEPS_tool = buildtool;dsp;core;livelink). Hosts with their own UI (a game
# engine's editor) link Sonus::tool_core.
snd_add_module(tool
  SOURCES
    src/project.cpp
    src/connection.cpp
    src/edit_history.cpp
    src/mixer.cpp
    src/cell_sync.cpp
    src/meter_ballistics.cpp
    src/profiler.cpp
    src/recording_file.cpp
    src/names.cpp
    src/session.cpp
    src/document.cpp
    src/doc_build.cpp
    src/event_editor.cpp
    src/mixer_editor.cpp
    src/curve_editor.cpp
    src/spatial_preview.cpp
    src/music_editor.cpp
    src/material_editor.cpp)
# The public headers use C++20 (defaulted operator== in edit_history / project /
# recording_file.hpp): a C++17 host gets cxx_std_20 for the files that include them.
target_compile_features(sonus_tool INTERFACE cxx_std_20)
add_library(sonus::tool_core ALIAS sonus_tool)
add_library(Sonus::tool_core ALIAS sonus_tool)

if(NOT SND_BUILD_TOOL)
  return()
endif()
find_package(Tela CONFIG REQUIRED)

# --- sonus_tool_ui: Tela declarations, controls and the CPU compositor (OS independent).
add_library(sonus_tool_ui STATIC
  ui/src/palette.cpp
  ui/src/safe_drawing.cpp
  ui/src/control_math.cpp
  ui/src/widgets.cpp
  ui/src/pointer_router.cpp
  ui/src/layout_metrics.cpp
  ui/src/status_view.cpp
  ui/src/mixer_view.cpp
  ui/src/profiler_view.cpp
  ui/src/app.cpp
  ui/src/raster_compositor.cpp
  ui/src/image_file.cpp)
target_include_directories(sonus_tool_ui PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/ui/include)
snd_apply_options(sonus_tool_ui)
target_link_libraries(sonus_tool_ui PUBLIC sonus_tool sonus_contract Tela::Core Tela::Raster PRIVATE sonus_livelink)

# --- sonus-tool: the executable. Hosts: headless (every platform), Windows (Tela::Windows),
# macOS (Tela::MacOS). The host files call Tela's throwing API and are the only Sonus
# code compiled with exceptions (they catch everything at the boundary).
set(_tool_app_sources app/main.cpp app/options.cpp app/host_headless.cpp)
add_executable(sonus-tool ${_tool_app_sources})
snd_apply_options(sonus-tool)
target_link_libraries(sonus-tool PRIVATE sonus_tool_ui sonus_tool sonus_platform)
if(WIN32)
  if(NOT TARGET Tela::Windows)
    message(FATAL_ERROR "sonus-tool on Windows needs Tela built with -DTELA_BUILD_WINDOWS=ON (Tela::Windows)")
  endif()
  target_sources(sonus-tool PRIVATE app/host_windows.cpp)
  set_source_files_properties(app/host_windows.cpp PROPERTIES COMPILE_OPTIONS "-fexceptions")
  target_compile_definitions(sonus-tool PRIVATE SND_TOOL_HOST_WINDOWS=1 UNICODE _UNICODE NOMINMAX)
  target_link_libraries(sonus-tool PRIVATE Tela::Windows user32)
elseif(APPLE)
  if(NOT TARGET Tela::MacOS)
    message(FATAL_ERROR "sonus-tool on macOS needs Tela built with -DTELA_BUILD_MACOS=ON (Tela::MacOS)")
  endif()
  enable_language(OBJCXX)
  target_sources(sonus-tool PRIVATE app/host_macos.mm)
  set_source_files_properties(app/host_macos.mm PROPERTIES COMPILE_OPTIONS "-fexceptions;-fobjc-arc")
  set_target_properties(sonus-tool PROPERTIES OBJCXX_STANDARD 20 OBJCXX_STANDARD_REQUIRED YES)
  target_compile_definitions(sonus-tool PRIVATE SND_TOOL_HOST_MACOS=1)
  target_link_libraries(sonus-tool PRIVATE Tela::MacOS "-framework AppKit")
endif()
# Text in the headless snapshots needs Tela's Pictor surface (TELA_BUILD_PICTOR); without
# it the CPU compositor draws everything but the text.
if(TARGET Tela::Pictor)
  target_sources(sonus-tool PRIVATE app/pictor_text.cpp)
  set_source_files_properties(app/pictor_text.cpp PROPERTIES COMPILE_OPTIONS "-fexceptions")
  target_compile_definitions(sonus-tool PRIVATE SND_TOOL_HAVE_PICTOR=1)
  target_link_libraries(sonus-tool PRIVATE Tela::Pictor)
endif()
