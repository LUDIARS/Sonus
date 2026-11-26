# core module (design.md 12.1): tables, commands, systems, voice management,
# bus graph, basic 3D, parameters, categories.
# Dependencies are fixed in the root CMakeLists.txt (SND_MODULE_DEPS_core).
snd_add_module(core
  SOURCES
    src/core_module.cpp
    src/create.cpp
    src/core_util.cpp
    src/tables.cpp
    src/recorder.cpp
    src/update.cpp
    src/services.cpp
    src/bank_registry.cpp
    src/sys_apply.cpp
    src/sys_transport.cpp
    src/sys_params.cpp
    src/sys_event.cpp
    src/sys_3d.cpp
    src/sys_dsp.cpp
    src/sys_voices.cpp
    src/sys_renderlist.cpp
    src/sys_render.cpp
    src/bus_graph.cpp
    src/sys_publish.cpp
    src/frame.cpp
    src/live_plugin_pool.cpp
    src/live_structure.cpp
    src/live_link.cpp
    src/fx_control.cpp
    src/voice_fx.cpp
    src/analysis.cpp)
