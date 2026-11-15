# tests module (design.md 12.1, 12.2-6/7, 14).
#
#   sonus_testkit         runner, assertions, golden audio, RT violation recorder
#   sonus_testkit_main    main() for every test executable (OBJECT)
#   sonus_testkit_rthooks operator new/delete + libc interposition (OBJECT)
#   sonus_mocks           null output device, memory file system, deterministic jobs
#   selftest/             tests of all of the above
#   integration/          cross-module tests (stage 2+), added from the root

add_library(sonus_testkit STATIC
  framework/src/runner.cpp
  framework/src/golden.cpp
  framework/src/rt_recorder.cpp)
target_include_directories(sonus_testkit PUBLIC framework/include)
target_link_libraries(sonus_testkit PUBLIC sonus_contract)
snd_apply_options(sonus_testkit)

add_library(sonus_testkit_main OBJECT framework/src/test_main.cpp)
target_link_libraries(sonus_testkit_main PRIVATE sonus_testkit)
snd_apply_options(sonus_testkit_main)

add_library(sonus_testkit_rthooks OBJECT
  framework/src/rt_hooks_new.cpp
  framework/src/rt_hooks_libc.c)
target_link_libraries(sonus_testkit_rthooks PRIVATE sonus_contract)
snd_apply_options(sonus_testkit_rthooks)
# dlsym(RTLD_NEXT) for the libc hooks (part of libc on glibc >= 2.34, harmless otherwise).
target_link_libraries(sonus_testkit_rthooks INTERFACE ${CMAKE_DL_LIBS})

add_library(sonus_mocks STATIC
  mocks/src/null_output_device.cpp
  mocks/src/memory_file_system.cpp
  mocks/src/deterministic_job_system.cpp)
add_library(sonus::mocks ALIAS sonus_mocks)
target_include_directories(sonus_mocks PUBLIC mocks/include)
target_link_libraries(sonus_mocks PUBLIC sonus_contract)
snd_apply_options(sonus_mocks)

add_subdirectory(selftest)
