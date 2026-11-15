// The path the build selects by default (SSE2 on x86-64, NEON on arm64, or
// scalar when the build forces it).
#define SND_SIMD_KERNEL_FN SimdKernelsNative
#include "simd_kernels.inc"
