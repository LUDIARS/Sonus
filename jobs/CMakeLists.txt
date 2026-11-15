# jobs module (design.md 12.1). Owner: base/platform/jobs team.
# Dependencies are fixed in the root CMakeLists.txt (SND_MODULE_DEPS_jobs).
snd_add_module(jobs
  SOURCES
    src/jobs_module.cpp)
