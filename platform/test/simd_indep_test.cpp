// Independent tests of the SIMD abstraction (design.md 3.4, simd.hpp header
// comment). Unlike simd_test.cpp (paths against the scalar path), every path
// here is checked against plain C++ / <cmath> references on inputs that include
// NaN, +-Inf, +-0, denormals, FLT_MAX / FLT_MIN, exact halves and values around
// the int32 range (seeded random plus a fixed edge list).
//
// Expected semantics, taken from simd.hpp:
//   - Min/Max: x86 semantics, a < b ? a : b / a > b ? a : b (second operand on NaN)
//   - CmpNe is true for unordered operands; the other compares are false
//   - Sqrt correctly rounded; Floor/Trunc/Round exact (Round = half to even)
//   - TruncToInt / RoundToInt: out of range or NaN -> INT32_MIN
//   - MulAdd / NegMulAdd fused (one rounding) iff kFusedMulAdd
//   - ReduceAdd order: F32x4 (v0 + v2) + (v1 + v3); F32x8 ReduceAdd(lo + hi)
#include <cfloat>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>

#include "simd_indep_kernels.hpp"
#include "snd/test/test.hpp"

using namespace simdindep;

namespace {

std::uint32_t Bits(float f) {
  std::uint32_t u;
  std::memcpy(&u, &f, 4);
  return u;
}
float FromBits(std::uint32_t u) {
  float f;
  std::memcpy(&f, &u, 4);
  return f;
}

struct Rng {
  std::uint64_t s;
  std::uint32_t Next() {
    s ^= s << 13;
    s ^= s >> 7;
    s ^= s << 17;
    return static_cast<std::uint32_t>(s >> 11);
  }
  float Uniform(float lo, float hi) { return lo + (hi - lo) * static_cast<float>(Next() & 0xFFFFFF) / 16777216.0f; }
};

const float kEdges[] = {
    std::numeric_limits<float>::quiet_NaN(), -std::numeric_limits<float>::quiet_NaN(),
    INFINITY, -INFINITY, 0.0f, -0.0f,
    std::numeric_limits<float>::denorm_min(), -std::numeric_limits<float>::denorm_min(),
    1.0e-40f, -3.0e-39f, FLT_MIN, -FLT_MIN, FLT_MAX, -FLT_MAX,
    0.5f, -0.5f, 1.5f, -1.5f, 2.5f, -2.5f, 0.49999997f, -0.49999997f,
    8388607.5f, -8388607.5f, 8388608.0f, -8388608.0f, 16777215.0f, 4194303.5f,
    2147483520.0f, -2147483520.0f, 2147483648.0f, -2147483648.0f, -2147483904.0f, 4294967296.0f,
    1.0e10f, -1.0e10f, 1.0f, -1.0f, 3.0f, -0.25f, 1.0e-7f, -2.0e30f,
};
constexpr std::uint32_t kEdgeCount = sizeof(kEdges) / sizeof(kEdges[0]);

void MakeInputs(Inputs* in) {
  Rng r{0x9E3779B97F4A7C15ull};
  auto pick = [&](std::uint32_t i, std::uint32_t salt) -> float {
    const std::uint32_t mode = (r.Next() + salt) % 6u;
    if (i < kEdgeCount * 3u && mode < 3u) return kEdges[(i * 7u + salt) % kEdgeCount];
    switch (mode) {
      case 0: return FromBits(r.Next() ^ (r.Next() << 21));  // any bit pattern
      case 1: return r.Uniform(-4.0f, 4.0f);
      case 2: return std::round(r.Uniform(-20.0f, 20.0f) * 2.0f) * 0.5f;  // exact halves
      case 3: return r.Uniform(-3.0e9f, 3.0e9f);
      case 4: return kEdges[r.Next() % kEdgeCount];
      default: return r.Uniform(-1.0f, 1.0f) * 1.0e-38f;  // denormal / tiny
    }
  };
  for (std::uint32_t i = 0; i < kN; ++i) {
    in->a[i] = pick(i, 0);
    in->b[i] = pick(i, 1);
    in->c[i] = pick(i, 2);
    if (i % 11u == 3u) in->b[i] = in->a[i];  // equal operands
    if (i % 13u == 5u) in->b[i] = -in->a[i];
    in->ia[i] = static_cast<std::int32_t>(r.Next() ^ (r.Next() << 16));
    in->ib[i] = (i % 9u == 0u) ? in->ia[i] : static_cast<std::int32_t>(r.Next() ^ (r.Next() << 16));
  }
  const std::int32_t iedge[] = {0, -1, 1, INT32_MIN, INT32_MAX, 0x40000000, -0x40000000, 7};
  for (std::uint32_t i = 0; i < 8; ++i) {
    in->ia[i] = iedge[i];
    in->ib[i] = iedge[7 - i];
  }
}

// ---- references -------------------------------------------------------------
float RMin(float a, float b) { return a < b ? a : b; }
float RMax(float a, float b) { return a > b ? a : b; }
float RUnfusedMulAdd(float a, float b, float c) {
  volatile float p = a * b;
  return p + c;
}
float RUnfusedNegMulAdd(float a, float b, float c) {
  volatile float p = a * b;
  return c - p;
}
std::int32_t RTruncToInt(float x) {
  if (!(x >= -2147483648.0f && x < 2147483648.0f)) return INT32_MIN;
  return static_cast<std::int32_t>(x);
}
std::int32_t RRoundToInt(float x) {
  if (!(x >= -2147483648.0f && x < 2147483648.0f)) return INT32_MIN;
  return static_cast<std::int32_t>(std::nearbyint(x));
}
std::int32_t Wrap(std::int64_t v) { return static_cast<std::int32_t>(static_cast<std::uint32_t>(v)); }

// Bitwise identity, any NaN equal to any NaN. valueOnly: +0 == -0.
bool Same(float x, float y, bool valueOnly = false) {
  if (std::isnan(x) || std::isnan(y)) return std::isnan(x) && std::isnan(y);
  if (valueOnly) return x == y;
  return Bits(x) == Bits(y);
}

float RefF(FOp op, const Inputs& in, std::uint32_t i, bool fused) {
  const float a = in.a[i], b = in.b[i], c = in.c[i];
  switch (op) {
    case kAdd: return a + b;
    case kSub: return a - b;
    case kMul: return a * b;
    case kDiv: return a / b;
    case kNeg: return FromBits(Bits(a) ^ 0x80000000u);
    case kMin: return RMin(a, b);
    case kMax: return RMax(a, b);
    case kAbs: return FromBits(Bits(a) & 0x7FFFFFFFu);
    case kSqrt: return std::sqrt(a);
    case kFloor: return std::floor(a);
    case kTrunc: return std::trunc(a);
    case kRound: return std::nearbyint(a);
    case kSelLt: return a < b ? a : c;
    case kSelLe: return a <= b ? a : c;
    case kSelGt: return a > b ? a : c;
    case kSelGe: return a >= b ? a : c;
    case kSelEq: return a == b ? a : c;
    case kSelNe: return !(a == b) ? a : c;
    case kSelAnd: return (a < b && b >= c) ? a : c;
    case kSelOr: return (a < b || b >= c) ? a : c;
    case kSelXor: return ((a < b) != (b >= c)) ? a : c;
    case kSelNot: return !(a < b) ? a : c;
    case kMulAdd: return fused ? std::fma(a, b, c) : RUnfusedMulAdd(a, b, c);
    case kNegMulAdd: return fused ? std::fma(-a, b, c) : RUnfusedNegMulAdd(a, b, c);
    case kLerp: {
      const float d = b - a;
      return fused ? std::fma(d, c, a) : RUnfusedMulAdd(d, c, a);
    }
    case kClamp: return RMin(RMax(a, -0.5f), 0.75f);
    case kToFloat: return static_cast<float>(in.ia[i]);
    case kBitRoundTrip: return a;
    case kLoadAligned: return a;
    case kSet1Lane: return 0.0f;  // checked separately
    case kFOpCount: break;
  }
  return 0.0f;
}

std::int32_t RefI(IOp op, const Inputs& in, std::uint32_t i) {
  const std::int32_t x = in.ia[i], y = in.ib[i];
  const std::uint32_t ux = static_cast<std::uint32_t>(x);
  switch (op) {
    case kTruncToInt: return RTruncToInt(in.a[i]);
    case kRoundToInt: return RRoundToInt(in.a[i]);
    case kIAdd: return Wrap(static_cast<std::int64_t>(x) + y);
    case kISub: return Wrap(static_cast<std::int64_t>(x) - y);
    case kIAnd: return x & y;
    case kIOr: return x | y;
    case kIXor: return x ^ y;
    case kIShl1: return static_cast<std::int32_t>(ux << 1);
    case kIShl31: return static_cast<std::int32_t>(ux << 31);
    case kIShr7: return static_cast<std::int32_t>(ux >> 7);
    case kISar7: return x >> 7;
    case kISar31: return x >> 31;
    case kICmpEq: return x == y ? 1 : 0;
    case kICmpGt: return x > y ? 1 : 0;
    case kISelect: return x > y ? x : y;
    case kBitCast: return static_cast<std::int32_t>(Bits(in.a[i]));
    case kIOpCount: break;
  }
  return 0;
}

const char* kFNames[] = {"Add", "Sub", "Mul", "Div", "Neg", "Min", "Max", "Abs", "Sqrt", "Floor",
                         "Trunc", "Round", "SelLt", "SelLe", "SelGt", "SelGe", "SelEq", "SelNe",
                         "SelAnd", "SelOr", "SelXor", "SelNot", "MulAdd", "NegMulAdd", "Lerp",
                         "Clamp", "ToFloat", "BitRoundTrip", "LoadAligned", "Set1"};
const char* kINames[] = {"TruncToInt", "RoundToInt", "IAdd", "ISub", "IAnd", "IOr", "IXor", "Shl1",
                         "Shl31", "Shr7", "Sar7", "Sar31", "ICmpEq", "ICmpGt", "ISelect", "BitCast"};
static_assert(sizeof(kFNames) / sizeof(kFNames[0]) == kFOpCount);
static_assert(sizeof(kINames) / sizeof(kINames[0]) == kIOpCount);

bool IsRounding(FOp op) { return op == kFloor || op == kTrunc || op == kRound; }

// Checks one width record. signOfZero: also require the IEEE sign of zero for
// Floor/Trunc/Round (separate test).
std::uint32_t CheckWidth(const char* path, bool fused, const WidthResults& w, const Inputs& in,
                         bool roundingSignOnly) {
  std::uint32_t bad = 0;
  const std::uint32_t W = w.lanes;
  for (std::uint32_t op = 0; op < kFOpCount; ++op) {
    const FOp f = static_cast<FOp>(op);
    if (roundingSignOnly && !IsRounding(f)) continue;
    std::uint32_t shown = 0;
    for (std::uint32_t i = 0; i < kN; ++i) {
      float ref = RefF(f, in, i, fused);
      if (f == kSet1Lane) ref = in.a[(i / W) * W];
      const bool ok = Same(w.f[op][i], ref, IsRounding(f) && !roundingSignOnly);
      if (!ok) {
        ++bad;
        if (shown++ < 3) {
          SND_CHECK_MSG(false, "%s x%u %s lane %u: a=%.9g b=%.9g c=%.9g -> %.9g (0x%08X), expected %.9g (0x%08X)",
                        path, W, kFNames[op], i, static_cast<double>(in.a[i]), static_cast<double>(in.b[i]),
                        static_cast<double>(in.c[i]), static_cast<double>(w.f[op][i]), Bits(w.f[op][i]),
                        static_cast<double>(ref), Bits(ref));
        }
      }
    }
  }
  if (roundingSignOnly) return bad;
  for (std::uint32_t op = 0; op < kIOpCount; ++op) {
    std::uint32_t shown = 0;
    for (std::uint32_t i = 0; i < kN; ++i) {
      const std::int32_t ref = RefI(static_cast<IOp>(op), in, i);
      if (w.i[op][i] != ref) {
        ++bad;
        if (shown++ < 3) {
          SND_CHECK_MSG(false, "%s x%u %s lane %u: a=%.9g ia=%d ib=%d -> %d, expected %d", path, W, kINames[op], i,
                        static_cast<double>(in.a[i]), in.ia[i], in.ib[i], w.i[op][i], ref);
        }
      }
    }
  }
  // Per-vector reductions.
  for (std::uint32_t v = 0; v < kN / W; ++v) {
    const float* x = in.a + v * W;
    float s[4];
    float mx[4], mn[4];
    for (std::uint32_t k = 0; k < 4; ++k) {
      if (W == 8) {
        s[k] = x[k] + x[k + 4];
        mx[k] = RMax(x[k], x[k + 4]);
        mn[k] = RMin(x[k], x[k + 4]);
      } else {
        s[k] = mx[k] = mn[k] = x[k];
      }
    }
    const float add = (s[0] + s[2]) + (s[1] + s[3]);
    const float rmax = RMax(RMax(mx[0], mx[2]), RMax(mx[1], mx[3]));
    const float rmin = RMin(RMin(mn[0], mn[2]), RMin(mn[1], mn[3]));
    std::uint32_t mLt = 0, mNe = 0;
    for (std::uint32_t k = 0; k < W; ++k) {
      if (in.a[v * W + k] < in.b[v * W + k]) mLt |= 1u << k;
      if (!(in.a[v * W + k] == in.b[v * W + k])) mNe |= 1u << k;
    }
    const float refs[kROpCount] = {add, rmax, rmin, static_cast<float>(mLt), static_cast<float>(mNe), x[0]};
    const char* names[kROpCount] = {"ReduceAdd", "ReduceMax", "ReduceMin", "MoveMaskLt", "MoveMaskNe", "Lane0"};
    for (std::uint32_t op = 0; op < kROpCount; ++op) {
      if (!Same(w.r[op][v], refs[op])) {
        if (bad++ < 40) {
          SND_CHECK_MSG(false, "%s x%u %s vector %u: %.9g, expected %.9g", path, W, names[op], v,
                        static_cast<double>(w.r[op][v]), static_cast<double>(refs[op]));
        }
      }
    }
  }
  return bad;
}

void CheckPath(const PathResults& r, const Inputs& in) {
  snd::test::Note("path %s fused %d", r.path, r.fused ? 1 : 0);
  std::uint32_t bad = CheckWidth(r.path, r.fused, r.w4, in, false);
  bad += CheckWidth(r.path, r.fused, r.w8, in, false);
  for (std::uint32_t k = 0; k < 4; ++k) {
    bad += !Same(r.combine[k], in.a[k]);
    bad += !Same(r.combine[4 + k], in.b[k]);
    bad += !Same(r.lo[k], in.a[k]);
    bad += !Same(r.hi[k], in.b[k]);
    bad += !Same(r.set4[k], in.a[k]);
  }
  SND_CHECK_EQ(bad, 0u);
}

void CheckRoundingSign(const PathResults& r, const Inputs& in) {
  std::uint32_t bad = CheckWidth(r.path, r.fused, r.w4, in, true);
  bad += CheckWidth(r.path, r.fused, r.w8, in, true);
  SND_CHECK_EQ(bad, 0u);
}

struct All {
  Inputs in;
  PathResults scalar, native;
#if defined(SND_SIMD_TEST_HAVE_AVX2)
  PathResults avx2;
#endif
  bool haveAvx2 = false;
};

std::unique_ptr<All> RunAll() {
  auto a = std::make_unique<All>();
  MakeInputs(&a->in);
  RunScalar(a->in, &a->scalar);
  RunNative(a->in, &a->native);
#if defined(SND_SIMD_TEST_HAVE_AVX2)
  if (__builtin_cpu_supports("avx2") && __builtin_cpu_supports("fma")) {
    RunAvx2(a->in, &a->avx2);
    a->haveAvx2 = true;
  }
#endif
  return a;
}

}  // namespace

// Every operation of every path against the C++ reference (Floor/Trunc/Round
// compared by value here; the sign of a zero result is checked below).
SND_TEST(SimdIndep, ScalarPathMatchesReference) {
  auto a = RunAll();
  SND_CHECK(std::strcmp(a->scalar.path, "scalar") == 0);
  SND_CHECK(!a->scalar.fused);
  CheckPath(a->scalar, a->in);
}

SND_TEST(SimdIndep, NativePathMatchesReference) {
  auto a = RunAll();
  CheckPath(a->native, a->in);
}

#if defined(SND_SIMD_TEST_HAVE_AVX2)
SND_TEST(SimdIndep, Avx2PathMatchesReference) {
  auto a = RunAll();
  if (!a->haveAvx2) {
    snd::test::Note("CPU without AVX2/FMA: not run");
    return;
  }
  CheckPath(a->avx2, a->in);
}
#endif

// simd.hpp: "Floor/Trunc/Round are exact" and "element-wise operations give the
// same bits on every path". IEEE roundToIntegral keeps the sign of the input
// for a zero result (trunc(-0.5) = -0, floor(-0) = -0, round(-0.4) = -0), and
// the NEON path (vrndq / vrndmq / vrndnq) does so. The x86 and scalar paths go
// through int32 conversion and return +0, so NEON and x86 builds give
// different bits for the same input.
SND_TEST(SimdIndep, RoundingKeepsSignOfZero_X86AndScalarDifferFromNeon) {
  auto a = RunAll();
  CheckRoundingSign(a->scalar, a->in);
  CheckRoundingSign(a->native, a->in);
#if defined(SND_SIMD_TEST_HAVE_AVX2)
  if (a->haveAvx2) CheckRoundingSign(a->avx2, a->in);
#endif
}

// Every compiled path gives the same bits (non-fused ops) as every other.
SND_TEST(SimdIndep, PathsAgreeBitwise) {
  auto a = RunAll();
  const PathResults* paths[3] = {&a->scalar, &a->native, nullptr};
#if defined(SND_SIMD_TEST_HAVE_AVX2)
  if (a->haveAvx2) paths[2] = &a->avx2;
#endif
  std::uint32_t bad = 0;
  for (const PathResults* p : paths) {
    if (p == nullptr) continue;
    for (const WidthResults* w : {&p->w4, &p->w8}) {
      const WidthResults& ref = (w->lanes == 4) ? a->scalar.w4 : a->scalar.w8;
      for (std::uint32_t op = 0; op < kFOpCount; ++op) {
        if ((op == kMulAdd || op == kNegMulAdd || op == kLerp) && p->fused) continue;
        for (std::uint32_t i = 0; i < kN; ++i) {
          if (!Same(w->f[op][i], ref.f[op][i]) && bad++ < 5) {
            SND_CHECK_MSG(false, "%s x%u %s lane %u differs from scalar", p->path, w->lanes, kFNames[op], i);
          }
        }
      }
      for (std::uint32_t op = 0; op < kIOpCount; ++op)
        for (std::uint32_t i = 0; i < kN; ++i) bad += w->i[op][i] != ref.i[op][i];
    }
  }
  SND_CHECK_EQ(bad, 0u);
}
