// Result record shared by the per-path SIMD kernel translation units.
#ifndef SND_PLATFORM_TEST_SIMD_KERNELS_HPP
#define SND_PLATFORM_TEST_SIMD_KERNELS_HPP

#include <cstdint>

struct SimdResults {
  const char* path;
  bool fusedMulAdd;
  std::uint32_t width;
  std::uint32_t fCount, iCount, fusedCount;
  float f[16384];
  std::int32_t i[8192];
  float fused[4096];
};

// One entry point per SIMD path. n must be a multiple of 8.
void SimdKernelsScalar(const float* a, const float* b, const float* c, std::uint32_t n,
                       SimdResults* r);
void SimdKernelsNative(const float* a, const float* b, const float* c, std::uint32_t n,
                       SimdResults* r);
#if defined(SND_SIMD_TEST_HAVE_AVX2)
void SimdKernelsAvx2(const float* a, const float* b, const float* c, std::uint32_t n,
                     SimdResults* r);
#endif

#endif  // SND_PLATFORM_TEST_SIMD_KERNELS_HPP
