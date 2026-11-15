# platform module (design.md 12.1). Owner: base/platform/jobs team.
# Dependencies are fixed in the root CMakeLists.txt (SND_MODULE_DEPS_platform).
snd_add_module(platform
  SOURCES
    src/platform_module.cpp)

# SIMD abstraction (header only, snd/platform/simd.hpp): force the scalar path everywhere.
option(SND_SIMD_SCALAR "Force the scalar SIMD path (snd/platform/simd.hpp)" OFF)
if(SND_SIMD_SCALAR)
  target_compile_definitions(sonus_platform PUBLIC SND_SIMD_FORCE_SCALAR=1)
endif()
