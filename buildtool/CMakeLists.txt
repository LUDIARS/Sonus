# buildtool module (design.md 12.1). Owner: bank/buildtool team.
# Dependencies are fixed in the root CMakeLists.txt (SND_MODULE_DEPS_buildtool).
snd_add_module(buildtool
  SOURCES
    src/buildtool_module.cpp
    src/json.cpp
    src/wav.cpp
    src/plugins.cpp
    src/bank_writer.cpp
    src/convert.cpp
    src/project.cpp
    src/build.cpp
    src/self_check.cpp)

add_executable(sonus-build src/main.cpp)
target_link_libraries(sonus-build PRIVATE sonus_buildtool)
snd_apply_options(sonus-build)
