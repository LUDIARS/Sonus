snd_add_test(platform_test
  SOURCES platform_smoke_test.cpp
  LINK sonus_platform sonus_mocks)

# clock, threads, semaphore, FTZ/DAZ, CPU features, timer output device (base/platform/jobs team)
snd_add_test(platform_runtime_test
  SOURCES platform_runtime_test.cpp
  LINK sonus_platform sonus_mocks)
snd_add_header_checks(platform_header_check
  HEADERS
    snd/platform/clock.hpp snd/platform/thread.hpp snd/platform/semaphore.hpp
    snd/platform/cpu.hpp snd/platform/output_device.hpp snd/platform/platform_module.hpp
    snd/platform/platform_output_device.hpp snd/platform/timer_output_device.hpp
    snd/platform/device_enum.hpp snd/platform/workgroup.hpp snd/platform/output_ring.hpp
    snd/platform/load_monitor.hpp snd/platform/driver_core.hpp snd/platform/audio_driver.hpp
  LINK sonus_platform)

# --- Stage 2: output backends and the audio-thread driver (CONTRACTS.md D-61..D-66) ---------
# OS-independent backend decisions (src/common): tested on every OS.
snd_add_test(platform_stream_policy_test SOURCES stream_policy_test.cpp LINK sonus_platform)
target_include_directories(platform_stream_policy_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
snd_add_test(platform_driver_core_test SOURCES driver_core_test.cpp LINK sonus_platform)
snd_add_test(platform_audio_driver_test SOURCES audio_driver_test.cpp LINK sonus_platform sonus_mocks)

# ALSA without a sound card (private null / file PCMs). Own main(): exit 77 = SKIP.
if(SND_PLATFORM_ALSA_ENABLED)
  add_executable(platform_alsa_test alsa_device_test.cpp alsa_test_main.cpp)
  snd_apply_options(platform_alsa_test)
  target_link_libraries(platform_alsa_test PRIVATE sonus_testkit sonus_testkit_rthooks sonus_platform)
  add_test(NAME platform_alsa_test COMMAND platform_alsa_test)
  set_tests_properties(platform_alsa_test PROPERTIES TIMEOUT 120 SKIP_RETURN_CODE 77)
endif()

# Real devices (WASAPI / Core Audio, and ALSA "default" on Linux): built on
# every OS but plays sound only when SND_DEVICE_TEST=1 is set; otherwise it
# exits with 77 (SKIP). See README "出力デバイスの確認".
add_executable(platform_device_play_test device_play_test.cpp device_play_main.cpp)
snd_apply_options(platform_device_play_test)
target_link_libraries(platform_device_play_test PRIVATE sonus_testkit sonus_platform)
if(NOT WIN32)
  # The operator new hooks use std::aligned_alloc, which MinGW / MSVC lack
  # (tests/framework/src/rt_hooks_new.cpp); the RT scope checks of the
  # SND_RT_NOTE kind still run without them.
  target_link_libraries(platform_device_play_test PRIVATE sonus_testkit_rthooks)
endif()
add_test(NAME platform_device_play_test COMMAND platform_device_play_test)
set_tests_properties(platform_device_play_test PROPERTIES TIMEOUT 300 SKIP_RETURN_CODE 77)
if(CMAKE_SYSTEM_NAME STREQUAL "iOS")
  set_target_properties(platform_device_play_test PROPERTIES MACOSX_BUNDLE TRUE)
endif()

# --- Independent (adversarial) tests ------------------------------------------------------
snd_add_test(platform_adv_test SOURCES adv_platform_test.cpp LINK sonus_platform sonus_mocks)
# Reproduces a defect (fails until fixed): deadline arithmetic of the timer device wraps.
snd_add_test(platform_defect_timer_test SOURCES defect_timer_test.cpp LINK sonus_platform sonus_mocks)

# SIMD abstraction (dsp team): every compiled path against the scalar path.
snd_add_test(platform_simd_test
  SOURCES simd_test.cpp simd_kernels_scalar.cpp simd_kernels_native.cpp
  LINK sonus_platform)
if(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64|amd64")
  target_sources(platform_simd_test PRIVATE simd_kernels_avx2.cpp)
  set_source_files_properties(simd_kernels_avx2.cpp PROPERTIES COMPILE_OPTIONS "-mavx2;-mfma")
  target_compile_definitions(platform_simd_test PRIVATE SND_SIMD_TEST_HAVE_AVX2=1)
endif()

# Independent SIMD tests: every path against plain C++ references.
snd_add_test(platform_simd_indep_test
  SOURCES simd_indep_test.cpp simd_indep_scalar.cpp simd_indep_native.cpp
  LINK sonus_platform)
if(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64|amd64")
  target_sources(platform_simd_indep_test PRIVATE simd_indep_avx2.cpp)
  set_source_files_properties(simd_indep_avx2.cpp PROPERTIES COMPILE_OPTIONS "-mavx2;-mfma")
  target_compile_definitions(platform_simd_indep_test PRIVATE SND_SIMD_TEST_HAVE_AVX2=1)
endif()
