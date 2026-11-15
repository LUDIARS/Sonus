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
  LINK sonus_platform)

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
