// Operation battery for one SIMD path (included by simd_indep_<path>.cpp with
// SND_SIMD_INDEP_FN set). Loads use deliberately misaligned addresses (LoadU /
// StoreU) except kLoadAligned, which goes through Load / Store on 64-byte
// aligned scratch.
#include <cstdint>
#include <cstring>

#include "simd_indep_kernels.hpp"
#include "snd/platform/simd.hpp"

namespace {
using namespace snd::simd;
using simdindep::Inputs;
using simdindep::kN;
using simdindep::PathResults;
using simdindep::WidthResults;

template <class V, int W>
void Put(float* dst, V v) {
  float tmp[W + 3];
  v.StoreU(tmp + 3);  // misaligned store
  std::memcpy(dst, tmp + 3, sizeof(float) * W);
}
template <class V, int W>
void PutI(std::int32_t* dst, V v) {
  std::int32_t tmp[W + 1];
  v.StoreU(tmp + 1);
  std::memcpy(dst, tmp + 1, sizeof(std::int32_t) * W);
}

// Integer-only operations that exist for I32x4 only (shifts, Select, bit casts).
void IntOps4(const Inputs& in, std::uint32_t o, WidthResults* r) {
  const I32x4 x = I32x4::LoadU(in.ia + o), y = I32x4::LoadU(in.ib + o);
  PutI<I32x4, 4>(r->i[simdindep::kIShl1] + o, ShiftLeft<1>(x));
  PutI<I32x4, 4>(r->i[simdindep::kIShl31] + o, ShiftLeft<31>(x));
  PutI<I32x4, 4>(r->i[simdindep::kIShr7] + o, ShiftRightLogical<7>(x));
  PutI<I32x4, 4>(r->i[simdindep::kISar7] + o, ShiftRightArith<7>(x));
  PutI<I32x4, 4>(r->i[simdindep::kISar31] + o, ShiftRightArith<31>(x));
  PutI<I32x4, 4>(r->i[simdindep::kISelect] + o, Select(CmpGt(x, y), x, y));
  const F32x4 a = F32x4::LoadU(in.a + o);
  PutI<I32x4, 4>(r->i[simdindep::kBitCast] + o, BitCastToInt(a));
  Put<F32x4, 4>(r->f[simdindep::kBitRoundTrip] + o, BitCastToFloat(BitCastToInt(a)));
}

template <class F, class I, int W>
void RunWidth(const Inputs& in, WidthResults* r) {
  using namespace simdindep;
  r->lanes = W;
  // Misaligned sources: copy into buffers at +1 float.
  alignas(64) static float ua[kN + 16], ub[kN + 16], uc[kN + 16];
  alignas(64) static float al[kN], alOut[kN];
  std::memcpy(ua + 1, in.a, sizeof(in.a));
  std::memcpy(ub + 1, in.b, sizeof(in.b));
  std::memcpy(uc + 1, in.c, sizeof(in.c));
  std::memcpy(al, in.a, sizeof(in.a));
  for (std::uint32_t o = 0; o < kN; o += W) {
    const F a = F::LoadU(ua + 1 + o), b = F::LoadU(ub + 1 + o), c = F::LoadU(uc + 1 + o);
    Put<F, W>(r->f[kAdd] + o, a + b);
    Put<F, W>(r->f[kSub] + o, a - b);
    Put<F, W>(r->f[kMul] + o, a * b);
    Put<F, W>(r->f[kDiv] + o, a / b);
    Put<F, W>(r->f[kNeg] + o, -a);
    Put<F, W>(r->f[kMin] + o, Min(a, b));
    Put<F, W>(r->f[kMax] + o, Max(a, b));
    Put<F, W>(r->f[kAbs] + o, Abs(a));
    Put<F, W>(r->f[kSqrt] + o, Sqrt(a));
    Put<F, W>(r->f[kFloor] + o, Floor(a));
    Put<F, W>(r->f[kTrunc] + o, Trunc(a));
    Put<F, W>(r->f[kRound] + o, Round(a));
    Put<F, W>(r->f[kSelLt] + o, Select(CmpLt(a, b), a, c));
    Put<F, W>(r->f[kSelLe] + o, Select(CmpLe(a, b), a, c));
    Put<F, W>(r->f[kSelGt] + o, Select(CmpGt(a, b), a, c));
    Put<F, W>(r->f[kSelGe] + o, Select(CmpGe(a, b), a, c));
    Put<F, W>(r->f[kSelEq] + o, Select(CmpEq(a, b), a, c));
    Put<F, W>(r->f[kSelNe] + o, Select(CmpNe(a, b), a, c));
    Put<F, W>(r->f[kSelAnd] + o, Select(CmpLt(a, b) & CmpGe(b, c), a, c));
    Put<F, W>(r->f[kSelOr] + o, Select(CmpLt(a, b) | CmpGe(b, c), a, c));
    Put<F, W>(r->f[kSelXor] + o, Select(CmpLt(a, b) ^ CmpGe(b, c), a, c));
    Put<F, W>(r->f[kSelNot] + o, Select(~CmpLt(a, b), a, c));
    Put<F, W>(r->f[kMulAdd] + o, MulAdd(a, b, c));
    Put<F, W>(r->f[kNegMulAdd] + o, NegMulAdd(a, b, c));
    Put<F, W>(r->f[kLerp] + o, Lerp(a, b, c));
    Put<F, W>(r->f[kClamp] + o, Clamp(a, F::Set1(-0.5f), F::Set1(0.75f)));
    const I ia = I::LoadU(in.ia + o), ib = I::LoadU(in.ib + o);
    Put<F, W>(r->f[kToFloat] + o, ToFloat(ia));
    F::Load(al + o).Store(alOut + o);
    Put<F, W>(r->f[kSet1Lane] + o, F::Set1(in.a[o]));
    PutI<I, W>(r->i[kTruncToInt] + o, TruncToInt(a));
    PutI<I, W>(r->i[kRoundToInt] + o, RoundToInt(a));
    PutI<I, W>(r->i[kIAdd] + o, ia + ib);
    PutI<I, W>(r->i[kISub] + o, ia - ib);
    PutI<I, W>(r->i[kIAnd] + o, ia & ib);
    PutI<I, W>(r->i[kIOr] + o, ia | ib);
    PutI<I, W>(r->i[kIXor] + o, ia ^ ib);
    {
      // Int compare masks, observed through MoveMask-free float Select.
      const F one = F::Set1(1.0f), zero = F::Zero();
      float eq[W], gt[W];
      Put<F, W>(eq, Select(CmpEq(ia, ib), one, zero));
      Put<F, W>(gt, Select(CmpGt(ia, ib), one, zero));
      for (int k = 0; k < W; ++k) {
        r->i[kICmpEq][o + k] = static_cast<std::int32_t>(eq[k]);
        r->i[kICmpGt][o + k] = static_cast<std::int32_t>(gt[k]);
      }
    }
    for (std::uint32_t h = 0; h < static_cast<std::uint32_t>(W); h += 4) IntOps4(in, o + h, r);
    const std::uint32_t v = o / W;
    r->r[kReduceAdd][v] = ReduceAdd(a);
    r->r[kReduceMax][v] = ReduceMax(a);
    r->r[kReduceMin][v] = ReduceMin(a);
    r->r[kMoveMaskLt][v] = static_cast<float>(MoveMask(CmpLt(a, b)));
    r->r[kMoveMaskNe][v] = static_cast<float>(MoveMask(CmpNe(a, b)));
    if constexpr (W == 4) {
      r->r[kLane0][v] = GetLane0(a);
    } else {
      r->r[kLane0][v] = GetLane0(LowHalf(a));
    }
  }
  std::memcpy(r->f[kLoadAligned], alOut, sizeof(alOut));
}
}  // namespace

namespace simdindep {
void SND_SIMD_INDEP_FN(const Inputs& in, PathResults* out) {
  std::memset(out, 0, sizeof(*out));
  out->path = kPathName;
  out->fused = kFusedMulAdd;
  RunWidth<F32x4, I32x4, 4>(in, &out->w4);
  RunWidth<F32x8, I32x8, 8>(in, &out->w8);
  const F32x8 comb = F32x8::Combine(F32x4::LoadU(in.a), F32x4::LoadU(in.b));
  comb.StoreU(out->combine);
  LowHalf(comb).StoreU(out->lo);
  HighHalf(comb).StoreU(out->hi);
  F32x4::Set(in.a[0], in.a[1], in.a[2], in.a[3]).StoreU(out->set4);
}
}  // namespace simdindep
