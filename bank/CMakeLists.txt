# bank module (design.md 12.1). Owner: bank/buildtool team.
# Dependencies are fixed in the root CMakeLists.txt (SND_MODULE_DEPS_bank).
snd_add_module(bank
  SOURCES
    src/bank_module.cpp)
