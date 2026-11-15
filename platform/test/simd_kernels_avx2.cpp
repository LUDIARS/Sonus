// AVX2 (+FMA) path. CMake compiles this file with -mavx2 -mfma on x86-64; it is
// only called when the CPU supports both.
#define SND_SIMD_KERNEL_FN SimdKernelsAvx2
#include "simd_kernels.inc"
