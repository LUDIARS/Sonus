// Scalar path (forced), whatever the build flags are.
#undef SND_SIMD_FORCE_SCALAR
#define SND_SIMD_FORCE_SCALAR 1
#define SND_SIMD_KERNEL_FN SimdKernelsScalar
#include "simd_kernels.inc"
