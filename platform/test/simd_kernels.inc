// Battery of SIMD operations evaluated by one SIMD path (included by
// simd_kernels_<path>.cpp with SND_SIMD_KERNEL_FN set). The results of every
// path are compared against the scalar path in simd_test.cpp.
#include <cstdint>
#include <cstring>

#include "simd_kernels.hpp"
#include "snd/platform/simd.hpp"

namespace {
using namespace snd::simd;

struct Cursor {
  SimdResults* r;
  std::uint32_t fi = 0, ii = 0;
};

template <class F, int W>
void PutF(Cursor& c, F v) {
  float tmp[W];
  v.StoreU(tmp);
  for (int k = 0; k < W; ++k) c.r->f[c.fi++] = tmp[k];
}
template <class I, int W>
void PutI(Cursor& c, I v) {
  std::int32_t tmp[W];
  v.StoreU(tmp);
  for (int k = 0; k < W; ++k) c.r->i[c.ii++] = tmp[k];
}

template <class F, class I, int W>
void RunWidth(const float* a, const float* b, const float* c, std::uint32_t n, Cursor& cur) {
  for (std::uint32_t o = 0; o + W <= n; o += W) {
    F x = F::LoadU(a + o), y = F::LoadU(b + o), z = F::LoadU(c + o);
    PutF<F, W>(cur, x + y);
    PutF<F, W>(cur, x - y);
    PutF<F, W>(cur, x * y);
    PutF<F, W>(cur, x / (Abs(y) + F::Set1(0.5f)));
    PutF<F, W>(cur, -x);
    PutF<F, W>(cur, Min(x, y));
    PutF<F, W>(cur, Max(x, y));
    PutF<F, W>(cur, Abs(x));
    PutF<F, W>(cur, Sqrt(Abs(x)));
    PutF<F, W>(cur, Floor(x * F::Set1(7.3f)));
    PutF<F, W>(cur, Trunc(x * F::Set1(7.3f)));
    PutF<F, W>(cur, Round(x * F::Set1(7.3f)));
    PutF<F, W>(cur, Select(CmpLt(x, y), x, z));
    PutF<F, W>(cur, Select(CmpGe(x, z) | CmpEq(y, z), y, z));
    PutF<F, W>(cur, Select(CmpLe(x, y) & CmpNe(x, z), F::Set1(1.0f), F::Zero()));
    PutF<F, W>(cur, Select(~CmpGt(y, z), x, y));
    PutF<F, W>(cur, Select(CmpLt(x, y) ^ CmpLt(y, z), x, y));
    PutF<F, W>(cur, Clamp(x, F::Set1(-0.25f), F::Set1(0.25f)));
    PutF<F, W>(cur, ToFloat(TruncToInt(x * F::Set1(1000.0f))));
    PutI<I, W>(cur, TruncToInt(x * F::Set1(1000.0f)));
    PutI<I, W>(cur, RoundToInt(y * F::Set1(1000.5f)));
    PutI<I, W>(cur, TruncToInt(x * F::Set1(1.0e12f)));  // out of range -> INT32_MIN
    cur.r->f[cur.fi++] = ReduceAdd(x);
    cur.r->f[cur.fi++] = ReduceMax(y);
    cur.r->f[cur.fi++] = ReduceMin(z);
    cur.r->i[cur.ii++] = static_cast<std::int32_t>(MoveMask(CmpLt(x, y)));
    // Fused ops are compared with a tolerance when kFusedMulAdd differs.
    cur.r->fused[cur.r->fusedCount++] = ReduceAdd(MulAdd(x, y, z));
    cur.r->fused[cur.r->fusedCount++] = ReduceAdd(NegMulAdd(x, y, z));
    cur.r->fused[cur.r->fusedCount++] = ReduceAdd(Lerp(x, y, z));
  }
}

// Integer lane operations (4 wide).
void RunInt(Cursor& cur) {
  I32x4 a = I32x4::Set(1, -7, 0x7FFFFFFF, -0x40000000);
  I32x4 b = I32x4::Set(3, 9, 1, -5);
  const I32x4 outs[] = {a + b,
                        a - b,
                        a & b,
                        a | b,
                        a ^ b,
                        ShiftLeft<3>(a),
                        ShiftRightLogical<2>(a),
                        ShiftRightArith<2>(a),
                        Select(CmpGt(a, b), a, b),
                        Select(CmpEq(a, a), b, a),
                        BitCastToInt(F32x4::Set(1.0f, -2.0f, 0.5f, 0.0f))};
  for (const I32x4& v : outs) PutI<I32x4, 4>(cur, v);
  PutF<F32x4, 4>(cur, BitCastToFloat(I32x4::Set(0x3F800000, 0x40000000, -0x40800000, 0)));
  PutF<F32x8, 8>(cur, F32x8::Combine(F32x4::Set(1, 2, 3, 4), F32x4::Set(5, 6, 7, 8)));
  PutF<F32x4, 4>(cur, HighHalf(F32x8::Combine(F32x4::Set(1, 2, 3, 4), F32x4::Set(5, 6, 7, 8))));
}
}  // namespace

void SND_SIMD_KERNEL_FN(const float* a, const float* b, const float* c, std::uint32_t n,
                        SimdResults* r) {
  std::memset(r, 0, sizeof(*r));
  r->path = kPathName;
  r->fusedMulAdd = kFusedMulAdd;
  r->width = kWidth;
  Cursor cur{r};
  RunWidth<F32x4, I32x4, 4>(a, b, c, n, cur);
  RunWidth<F32x8, I32x8, 8>(a, b, c, n, cur);
  RunInt(cur);
  r->fCount = cur.fi;
  r->iCount = cur.ii;
}
