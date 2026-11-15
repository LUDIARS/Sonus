snd_add_test(platform_test
  SOURCES platform_smoke_test.cpp
  LINK sonus_platform sonus_mocks)

# SIMD abstraction (dsp team): every compiled path against the scalar path.
snd_add_test(platform_simd_test
  SOURCES simd_test.cpp simd_kernels_scalar.cpp simd_kernels_native.cpp
  LINK sonus_platform)
if(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64|amd64")
  target_sources(platform_simd_test PRIVATE simd_kernels_avx2.cpp)
  set_source_files_properties(simd_kernels_avx2.cpp PROPERTIES COMPILE_OPTIONS "-mavx2;-mfma")
  target_compile_definitions(platform_simd_test PRIVATE SND_SIMD_TEST_HAVE_AVX2=1)
endif()
