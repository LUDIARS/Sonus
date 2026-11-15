// Independent SIMD reference tests (simd_indep_test.cpp): every compiled path
// evaluates the whole operation set on the same inputs; the test compares each
// path against plain C++ / <cmath> references (not against another path).
#ifndef SND_PLATFORM_TEST_SIMD_INDEP_KERNELS_HPP
#define SND_PLATFORM_TEST_SIMD_INDEP_KERNELS_HPP

#include <cstdint>

namespace simdindep {

inline constexpr std::uint32_t kN = 512;  // multiple of 8

// Float lane operations (one output per input lane).
enum FOp : std::uint32_t {
  kAdd, kSub, kMul, kDiv, kNeg, kMin, kMax, kAbs, kSqrt, kFloor, kTrunc, kRound,
  kSelLt, kSelLe, kSelGt, kSelGe, kSelEq, kSelNe, kSelAnd, kSelOr, kSelXor, kSelNot,
  kMulAdd, kNegMulAdd, kLerp, kClamp, kToFloat, kBitRoundTrip, kLoadAligned, kSet1Lane,
  kFOpCount
};
// Int lane results.
enum IOp : std::uint32_t {
  kTruncToInt, kRoundToInt, kIAdd, kISub, kIAnd, kIOr, kIXor, kIShl1, kIShl31, kIShr7,
  kISar7, kISar31, kICmpEq, kICmpGt, kISelect, kBitCast, kIOpCount
};
// Per-vector results (one per vector of W lanes).
enum ROp : std::uint32_t { kReduceAdd, kReduceMax, kReduceMin, kMoveMaskLt, kMoveMaskNe, kLane0, kROpCount };

struct Inputs {
  float a[kN], b[kN], c[kN];
  std::int32_t ia[kN], ib[kN];
};

struct WidthResults {
  float f[kFOpCount][kN];
  std::int32_t i[kIOpCount][kN];
  float r[kROpCount][kN / 4];
  std::uint32_t lanes;  // vector width W of this record (4 or 8)
};

struct PathResults {
  const char* path;
  bool fused;
  WidthResults w4, w8;
  // F32x8 helpers (Combine / halves) and F32x4::Set lane order.
  float combine[8], lo[4], hi[4], set4[4];
};

void RunScalar(const Inputs& in, PathResults* out);
void RunNative(const Inputs& in, PathResults* out);
#if defined(SND_SIMD_TEST_HAVE_AVX2)
void RunAvx2(const Inputs& in, PathResults* out);
#endif

}  // namespace simdindep

#endif  // SND_PLATFORM_TEST_SIMD_INDEP_KERNELS_HPP
