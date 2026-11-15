// AVX2 + FMA path (compiled with -mavx2 -mfma on x86-64; run only when the CPU has both).
#define SND_SIMD_INDEP_FN RunAvx2
#include "simd_indep_kernels.inc"
