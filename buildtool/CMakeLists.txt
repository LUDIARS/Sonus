# buildtool module (design.md 12.1). Owner: bank/buildtool team.
# Dependencies are fixed in the root CMakeLists.txt (SND_MODULE_DEPS_buildtool).
snd_add_module(buildtool
  SOURCES
    src/buildtool_module.cpp)

add_executable(sonus-build src/main.cpp)
target_link_libraries(sonus-build PRIVATE sonus_buildtool)
snd_apply_options(sonus-build)
