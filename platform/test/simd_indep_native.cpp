// Default path of the build (SSE2 / NEON / scalar / AVX2 when the build flags ask for it).
#define SND_SIMD_INDEP_FN RunNative
#include "simd_indep_kernels.inc"
