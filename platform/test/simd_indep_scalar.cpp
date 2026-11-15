// Scalar path (forced) for simd_indep_test.cpp.
#undef SND_SIMD_FORCE_SCALAR
#define SND_SIMD_FORCE_SCALAR 1
#define SND_SIMD_INDEP_FN RunScalar
#include "simd_indep_kernels.inc"
