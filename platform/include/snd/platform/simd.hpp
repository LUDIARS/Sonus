// Thin SIMD abstraction (design.md 3.4). Header only.
//
// One set of value types and free functions that DSP and 3D code are written
// against. The implementation is chosen at compile time:
//
//   SND_SIMD_FORCE_SCALAR=1      -> portable scalar emulation (always available)
//   __AVX2__ (+ __FMA__ for FMA) -> AVX2: F32x8 native, F32x4 = SSE
//   __SSE2__ / x86-64            -> SSE2: F32x4 native, F32x8 = 2 x F32x4
//   __ARM_NEON                   -> NEON: F32x4 native, F32x8 = 2 x F32x4
//   otherwise                    -> scalar
//
// Every path lives in its own inline namespace (snd::simd::v_scalar, v_sse2,
// v_avx2, v_neon), so translation units compiled with different paths can be
// linked into one program without ODR conflicts (the tests do exactly that to
// compare paths).
//
// Types: F32x4 / F32x8 (float lanes), I32x4 / I32x8 (int32 lanes), M32x4 /
// M32x8 (lane masks, all bits set = true). F32xN / I32xN / M32xN alias the
// widest native width (kWidth lanes).
//
// Determinism rules (2.5 "same SIMD path"):
//   - Element-wise operations give the same bits on every path, except
//     MulAdd/NegMulAdd, which are fused (one rounding) when kFusedMulAdd is
//     true (AVX2 + FMA, NEON) and unfused otherwise.
//   - Horizontal reductions use one fixed order on every path:
//     F32x4: (v0 + v2) + (v1 + v3); F32x8: ReduceAdd(lo + hi).
//   - Sqrt is correctly rounded everywhere. Floor/Trunc/Round are exact.
//
// Loads/stores: Load/Store require natural vector alignment (16 bytes for x4,
// 32 bytes for x8; DSP buffers are 64-byte aligned, 3.4). LoadU/StoreU accept
// any address.
#ifndef SND_PLATFORM_SIMD_HPP
#define SND_PLATFORM_SIMD_HPP

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>

#if !defined(SND_SIMD_FORCE_SCALAR)
#define SND_SIMD_FORCE_SCALAR 0
#endif

#if SND_SIMD_FORCE_SCALAR
#define SND_SIMD_SCALAR 1
#elif defined(__AVX2__)
#define SND_SIMD_AVX2 1
#elif defined(__SSE2__) || defined(_M_X64) || (defined(_M_IX86_FP) && _M_IX86_FP >= 2)
#define SND_SIMD_SSE2 1
#elif defined(__ARM_NEON) || defined(__ARM_NEON__) || defined(_M_ARM64)
#define SND_SIMD_NEON 1
#else
#define SND_SIMD_SCALAR 1
#endif

#if defined(SND_SIMD_AVX2) || defined(SND_SIMD_SSE2)
#include <emmintrin.h>
#endif
#if defined(SND_SIMD_AVX2)
#include <immintrin.h>
#endif
#if defined(SND_SIMD_NEON)
#include <arm_neon.h>
#endif

#if defined(_MSC_VER) && !defined(__clang__)
#define SND_SIMD_INLINE __forceinline
#else
#define SND_SIMD_INLINE inline __attribute__((always_inline))
#endif

namespace snd::simd {

#if defined(SND_SIMD_AVX2)
inline namespace v_avx2 {
#elif defined(SND_SIMD_SSE2)
inline namespace v_sse2 {
#elif defined(SND_SIMD_NEON)
inline namespace v_neon {
#else
inline namespace v_scalar {
#endif

// ---------------------------------------------------------------------------
// Path description
// ---------------------------------------------------------------------------
#if defined(SND_SIMD_AVX2)
inline constexpr const char* kPathName = "avx2";
inline constexpr std::uint32_t kWidth = 8;
#if defined(__FMA__)
inline constexpr bool kFusedMulAdd = true;
#else
inline constexpr bool kFusedMulAdd = false;
#endif
#elif defined(SND_SIMD_SSE2)
inline constexpr const char* kPathName = "sse2";
inline constexpr std::uint32_t kWidth = 4;
inline constexpr bool kFusedMulAdd = false;
#elif defined(SND_SIMD_NEON)
inline constexpr const char* kPathName = "neon";
inline constexpr std::uint32_t kWidth = 4;
inline constexpr bool kFusedMulAdd = true;
#else
inline constexpr const char* kPathName = "scalar";
inline constexpr std::uint32_t kWidth = 4;
inline constexpr bool kFusedMulAdd = false;
#endif

inline constexpr std::size_t kBufferAlignment = 64;  // 3.4

inline bool IsAligned(const void* p, std::size_t alignment = kBufferAlignment) noexcept {
  return (reinterpret_cast<std::uintptr_t>(p) & (alignment - 1u)) == 0u;
}

namespace scalar_detail {
SND_SIMD_INLINE float MaskTrue() noexcept {
  std::uint32_t u = 0xFFFFFFFFu;
  float f;
  std::memcpy(&f, &u, 4);
  return f;
}
SND_SIMD_INLINE std::uint32_t Bits(float f) noexcept {
  std::uint32_t u;
  std::memcpy(&u, &f, 4);
  return u;
}
SND_SIMD_INLINE float FromBits(std::uint32_t u) noexcept {
  float f;
  std::memcpy(&f, &u, 4);
  return f;
}
// Exact floor for any float (|x| >= 2^23 is already integral).
SND_SIMD_INLINE float Floor(float x) noexcept {
  if (!(std::fabs(x) < 8388608.0f)) return x;
  float t = static_cast<float>(static_cast<std::int32_t>(x));
  return t > x ? t - 1.0f : t;
}
SND_SIMD_INLINE float Trunc(float x) noexcept {
  if (!(std::fabs(x) < 8388608.0f)) return x;
  return static_cast<float>(static_cast<std::int32_t>(x));
}
// Round half to even (default FP rounding), exact.
SND_SIMD_INLINE float RoundEven(float x) noexcept {
  if (!(std::fabs(x) < 8388608.0f)) return x;
  const float big = x >= 0.0f ? 8388608.0f : -8388608.0f;
  volatile float t = x + big;  // volatile: keep the rounding step
  return t - big;
}
// cvttps semantics: out of range / NaN -> INT32_MIN.
SND_SIMD_INLINE std::int32_t TruncToInt(float x) noexcept {
  if (!(x >= -2147483648.0f && x < 2147483648.0f)) return INT32_MIN;
  return static_cast<std::int32_t>(x);
}
SND_SIMD_INLINE std::int32_t RoundToInt(float x) noexcept {
  if (!(x >= -2147483648.0f && x < 2147483648.0f)) return INT32_MIN;
  return static_cast<std::int32_t>(RoundEven(x));
}
}  // namespace scalar_detail

// ===========================================================================
// 4-wide types
// ===========================================================================
#if defined(SND_SIMD_AVX2) || defined(SND_SIMD_SSE2)

struct M32x4 {
  __m128 v;
};
struct I32x4 {
  __m128i v;
  static SND_SIMD_INLINE I32x4 Zero() noexcept { return {_mm_setzero_si128()}; }
  static SND_SIMD_INLINE I32x4 Set1(std::int32_t x) noexcept { return {_mm_set1_epi32(x)}; }
  static SND_SIMD_INLINE I32x4 Set(std::int32_t a, std::int32_t b, std::int32_t c,
                                   std::int32_t d) noexcept {
    return {_mm_setr_epi32(a, b, c, d)};
  }
  static SND_SIMD_INLINE I32x4 LoadU(const std::int32_t* p) noexcept {
    return {_mm_loadu_si128(reinterpret_cast<const __m128i*>(p))};
  }
  SND_SIMD_INLINE void StoreU(std::int32_t* p) const noexcept {
    _mm_storeu_si128(reinterpret_cast<__m128i*>(p), v);
  }
};
struct F32x4 {
  __m128 v;
  static SND_SIMD_INLINE F32x4 Zero() noexcept { return {_mm_setzero_ps()}; }
  static SND_SIMD_INLINE F32x4 Set1(float x) noexcept { return {_mm_set1_ps(x)}; }
  static SND_SIMD_INLINE F32x4 Set(float a, float b, float c, float d) noexcept {
    return {_mm_setr_ps(a, b, c, d)};
  }
  static SND_SIMD_INLINE F32x4 Load(const float* p) noexcept { return {_mm_load_ps(p)}; }
  static SND_SIMD_INLINE F32x4 LoadU(const float* p) noexcept { return {_mm_loadu_ps(p)}; }
  SND_SIMD_INLINE void Store(float* p) const noexcept { _mm_store_ps(p, v); }
  SND_SIMD_INLINE void StoreU(float* p) const noexcept { _mm_storeu_ps(p, v); }
};

SND_SIMD_INLINE F32x4 operator+(F32x4 a, F32x4 b) noexcept { return {_mm_add_ps(a.v, b.v)}; }
SND_SIMD_INLINE F32x4 operator-(F32x4 a, F32x4 b) noexcept { return {_mm_sub_ps(a.v, b.v)}; }
SND_SIMD_INLINE F32x4 operator*(F32x4 a, F32x4 b) noexcept { return {_mm_mul_ps(a.v, b.v)}; }
SND_SIMD_INLINE F32x4 operator/(F32x4 a, F32x4 b) noexcept { return {_mm_div_ps(a.v, b.v)}; }
SND_SIMD_INLINE F32x4 operator-(F32x4 a) noexcept {
  return {_mm_xor_ps(a.v, _mm_set1_ps(-0.0f))};
}
SND_SIMD_INLINE F32x4 MulAdd(F32x4 a, F32x4 b, F32x4 c) noexcept {
#if defined(SND_SIMD_AVX2) && defined(__FMA__)
  return {_mm_fmadd_ps(a.v, b.v, c.v)};
#else
  return {_mm_add_ps(_mm_mul_ps(a.v, b.v), c.v)};
#endif
}
SND_SIMD_INLINE F32x4 NegMulAdd(F32x4 a, F32x4 b, F32x4 c) noexcept {  // c - a*b
#if defined(SND_SIMD_AVX2) && defined(__FMA__)
  return {_mm_fnmadd_ps(a.v, b.v, c.v)};
#else
  return {_mm_sub_ps(c.v, _mm_mul_ps(a.v, b.v))};
#endif
}
SND_SIMD_INLINE F32x4 Min(F32x4 a, F32x4 b) noexcept { return {_mm_min_ps(a.v, b.v)}; }
SND_SIMD_INLINE F32x4 Max(F32x4 a, F32x4 b) noexcept { return {_mm_max_ps(a.v, b.v)}; }
SND_SIMD_INLINE F32x4 Abs(F32x4 a) noexcept { return {_mm_andnot_ps(_mm_set1_ps(-0.0f), a.v)}; }
SND_SIMD_INLINE F32x4 Sqrt(F32x4 a) noexcept { return {_mm_sqrt_ps(a.v)}; }

SND_SIMD_INLINE M32x4 CmpLt(F32x4 a, F32x4 b) noexcept { return {_mm_cmplt_ps(a.v, b.v)}; }
SND_SIMD_INLINE M32x4 CmpLe(F32x4 a, F32x4 b) noexcept { return {_mm_cmple_ps(a.v, b.v)}; }
SND_SIMD_INLINE M32x4 CmpGt(F32x4 a, F32x4 b) noexcept { return {_mm_cmpgt_ps(a.v, b.v)}; }
SND_SIMD_INLINE M32x4 CmpGe(F32x4 a, F32x4 b) noexcept { return {_mm_cmpge_ps(a.v, b.v)}; }
SND_SIMD_INLINE M32x4 CmpEq(F32x4 a, F32x4 b) noexcept { return {_mm_cmpeq_ps(a.v, b.v)}; }
SND_SIMD_INLINE M32x4 CmpNe(F32x4 a, F32x4 b) noexcept { return {_mm_cmpneq_ps(a.v, b.v)}; }
SND_SIMD_INLINE M32x4 operator&(M32x4 a, M32x4 b) noexcept { return {_mm_and_ps(a.v, b.v)}; }
SND_SIMD_INLINE M32x4 operator|(M32x4 a, M32x4 b) noexcept { return {_mm_or_ps(a.v, b.v)}; }
SND_SIMD_INLINE M32x4 operator^(M32x4 a, M32x4 b) noexcept { return {_mm_xor_ps(a.v, b.v)}; }
SND_SIMD_INLINE M32x4 operator~(M32x4 a) noexcept {
  return {_mm_xor_ps(a.v, _mm_castsi128_ps(_mm_set1_epi32(-1)))};
}
// m ? a : b per lane.
SND_SIMD_INLINE F32x4 Select(M32x4 m, F32x4 a, F32x4 b) noexcept {
  return {_mm_or_ps(_mm_and_ps(m.v, a.v), _mm_andnot_ps(m.v, b.v))};
}
// Bit i = lane i is true.
SND_SIMD_INLINE std::uint32_t MoveMask(M32x4 m) noexcept {
  return static_cast<std::uint32_t>(_mm_movemask_ps(m.v));
}

SND_SIMD_INLINE float GetLane0(F32x4 a) noexcept { return _mm_cvtss_f32(a.v); }
SND_SIMD_INLINE float ReduceAdd(F32x4 a) noexcept {
  __m128 hi = _mm_movehl_ps(a.v, a.v);           // v2 v3 v2 v3
  __m128 s = _mm_add_ps(a.v, hi);                // v0+v2, v1+v3
  __m128 s1 = _mm_shuffle_ps(s, s, _MM_SHUFFLE(1, 1, 1, 1));
  return _mm_cvtss_f32(_mm_add_ss(s, s1));       // (v0+v2)+(v1+v3)
}
SND_SIMD_INLINE float ReduceMax(F32x4 a) noexcept {
  __m128 hi = _mm_movehl_ps(a.v, a.v);
  __m128 s = _mm_max_ps(a.v, hi);
  __m128 s1 = _mm_shuffle_ps(s, s, _MM_SHUFFLE(1, 1, 1, 1));
  return _mm_cvtss_f32(_mm_max_ss(s, s1));
}
SND_SIMD_INLINE float ReduceMin(F32x4 a) noexcept {
  __m128 hi = _mm_movehl_ps(a.v, a.v);
  __m128 s = _mm_min_ps(a.v, hi);
  __m128 s1 = _mm_shuffle_ps(s, s, _MM_SHUFFLE(1, 1, 1, 1));
  return _mm_cvtss_f32(_mm_min_ss(s, s1));
}

// Integer lanes.
SND_SIMD_INLINE I32x4 operator+(I32x4 a, I32x4 b) noexcept { return {_mm_add_epi32(a.v, b.v)}; }
SND_SIMD_INLINE I32x4 operator-(I32x4 a, I32x4 b) noexcept { return {_mm_sub_epi32(a.v, b.v)}; }
SND_SIMD_INLINE I32x4 operator&(I32x4 a, I32x4 b) noexcept { return {_mm_and_si128(a.v, b.v)}; }
SND_SIMD_INLINE I32x4 operator|(I32x4 a, I32x4 b) noexcept { return {_mm_or_si128(a.v, b.v)}; }
SND_SIMD_INLINE I32x4 operator^(I32x4 a, I32x4 b) noexcept { return {_mm_xor_si128(a.v, b.v)}; }
template <int N>
SND_SIMD_INLINE I32x4 ShiftLeft(I32x4 a) noexcept {
  return {_mm_slli_epi32(a.v, N)};
}
template <int N>
SND_SIMD_INLINE I32x4 ShiftRightLogical(I32x4 a) noexcept {
  return {_mm_srli_epi32(a.v, N)};
}
template <int N>
SND_SIMD_INLINE I32x4 ShiftRightArith(I32x4 a) noexcept {
  return {_mm_srai_epi32(a.v, N)};
}
SND_SIMD_INLINE M32x4 CmpEq(I32x4 a, I32x4 b) noexcept {
  return {_mm_castsi128_ps(_mm_cmpeq_epi32(a.v, b.v))};
}
SND_SIMD_INLINE M32x4 CmpGt(I32x4 a, I32x4 b) noexcept {
  return {_mm_castsi128_ps(_mm_cmpgt_epi32(a.v, b.v))};
}
SND_SIMD_INLINE I32x4 Select(M32x4 m, I32x4 a, I32x4 b) noexcept {
  __m128i mi = _mm_castps_si128(m.v);
  return {_mm_or_si128(_mm_and_si128(mi, a.v), _mm_andnot_si128(mi, b.v))};
}

// Conversions. Out-of-range / NaN gives INT32_MIN on every path.
SND_SIMD_INLINE I32x4 TruncToInt(F32x4 a) noexcept { return {_mm_cvttps_epi32(a.v)}; }
SND_SIMD_INLINE I32x4 RoundToInt(F32x4 a) noexcept { return {_mm_cvtps_epi32(a.v)}; }
SND_SIMD_INLINE F32x4 ToFloat(I32x4 a) noexcept { return {_mm_cvtepi32_ps(a.v)}; }
SND_SIMD_INLINE I32x4 BitCastToInt(F32x4 a) noexcept { return {_mm_castps_si128(a.v)}; }
SND_SIMD_INLINE F32x4 BitCastToFloat(I32x4 a) noexcept { return {_mm_castsi128_ps(a.v)}; }

// Exact for every float (|x| >= 2^23 is integral already).
SND_SIMD_INLINE F32x4 Trunc(F32x4 a) noexcept {
  const __m128 big = _mm_set1_ps(8388608.0f);
  __m128 small = _mm_cmplt_ps(_mm_andnot_ps(_mm_set1_ps(-0.0f), a.v), big);
  __m128 t = _mm_cvtepi32_ps(_mm_cvttps_epi32(a.v));
  return {_mm_or_ps(_mm_and_ps(small, t), _mm_andnot_ps(small, a.v))};
}
SND_SIMD_INLINE F32x4 Floor(F32x4 a) noexcept {
  F32x4 t = Trunc(a);
  __m128 gt = _mm_cmpgt_ps(t.v, a.v);
  return {_mm_sub_ps(t.v, _mm_and_ps(gt, _mm_set1_ps(1.0f)))};
}
SND_SIMD_INLINE F32x4 Round(F32x4 a) noexcept {  // half to even
  const __m128 big = _mm_set1_ps(8388608.0f);
  __m128 small = _mm_cmplt_ps(_mm_andnot_ps(_mm_set1_ps(-0.0f), a.v), big);
  __m128 r = _mm_cvtepi32_ps(_mm_cvtps_epi32(a.v));
  return {_mm_or_ps(_mm_and_ps(small, r), _mm_andnot_ps(small, a.v))};
}

#elif defined(SND_SIMD_NEON)

struct M32x4 {
  uint32x4_t v;
};
struct I32x4 {
  int32x4_t v;
  static SND_SIMD_INLINE I32x4 Zero() noexcept { return {vdupq_n_s32(0)}; }
  static SND_SIMD_INLINE I32x4 Set1(std::int32_t x) noexcept { return {vdupq_n_s32(x)}; }
  static SND_SIMD_INLINE I32x4 Set(std::int32_t a, std::int32_t b, std::int32_t c,
                                   std::int32_t d) noexcept {
    const std::int32_t t[4] = {a, b, c, d};
    return {vld1q_s32(t)};
  }
  static SND_SIMD_INLINE I32x4 LoadU(const std::int32_t* p) noexcept { return {vld1q_s32(p)}; }
  SND_SIMD_INLINE void StoreU(std::int32_t* p) const noexcept { vst1q_s32(p, v); }
};
struct F32x4 {
  float32x4_t v;
  static SND_SIMD_INLINE F32x4 Zero() noexcept { return {vdupq_n_f32(0.0f)}; }
  static SND_SIMD_INLINE F32x4 Set1(float x) noexcept { return {vdupq_n_f32(x)}; }
  static SND_SIMD_INLINE F32x4 Set(float a, float b, float c, float d) noexcept {
    const float t[4] = {a, b, c, d};
    return {vld1q_f32(t)};
  }
  static SND_SIMD_INLINE F32x4 Load(const float* p) noexcept { return {vld1q_f32(p)}; }
  static SND_SIMD_INLINE F32x4 LoadU(const float* p) noexcept { return {vld1q_f32(p)}; }
  SND_SIMD_INLINE void Store(float* p) const noexcept { vst1q_f32(p, v); }
  SND_SIMD_INLINE void StoreU(float* p) const noexcept { vst1q_f32(p, v); }
};

SND_SIMD_INLINE F32x4 operator+(F32x4 a, F32x4 b) noexcept { return {vaddq_f32(a.v, b.v)}; }
SND_SIMD_INLINE F32x4 operator-(F32x4 a, F32x4 b) noexcept { return {vsubq_f32(a.v, b.v)}; }
SND_SIMD_INLINE F32x4 operator*(F32x4 a, F32x4 b) noexcept { return {vmulq_f32(a.v, b.v)}; }
SND_SIMD_INLINE F32x4 operator/(F32x4 a, F32x4 b) noexcept { return {vdivq_f32(a.v, b.v)}; }
SND_SIMD_INLINE F32x4 operator-(F32x4 a) noexcept { return {vnegq_f32(a.v)}; }
SND_SIMD_INLINE F32x4 MulAdd(F32x4 a, F32x4 b, F32x4 c) noexcept {
  return {vfmaq_f32(c.v, a.v, b.v)};
}
SND_SIMD_INLINE F32x4 NegMulAdd(F32x4 a, F32x4 b, F32x4 c) noexcept {
  return {vfmsq_f32(c.v, a.v, b.v)};
}
SND_SIMD_INLINE F32x4 Min(F32x4 a, F32x4 b) noexcept {
  // SSE semantics (second operand when unordered): a < b ? a : b
  return {vbslq_f32(vcltq_f32(a.v, b.v), a.v, b.v)};
}
SND_SIMD_INLINE F32x4 Max(F32x4 a, F32x4 b) noexcept {
  return {vbslq_f32(vcgtq_f32(a.v, b.v), a.v, b.v)};
}
SND_SIMD_INLINE F32x4 Abs(F32x4 a) noexcept { return {vabsq_f32(a.v)}; }
SND_SIMD_INLINE F32x4 Sqrt(F32x4 a) noexcept { return {vsqrtq_f32(a.v)}; }

SND_SIMD_INLINE M32x4 CmpLt(F32x4 a, F32x4 b) noexcept { return {vcltq_f32(a.v, b.v)}; }
SND_SIMD_INLINE M32x4 CmpLe(F32x4 a, F32x4 b) noexcept { return {vcleq_f32(a.v, b.v)}; }
SND_SIMD_INLINE M32x4 CmpGt(F32x4 a, F32x4 b) noexcept { return {vcgtq_f32(a.v, b.v)}; }
SND_SIMD_INLINE M32x4 CmpGe(F32x4 a, F32x4 b) noexcept { return {vcgeq_f32(a.v, b.v)}; }
SND_SIMD_INLINE M32x4 CmpEq(F32x4 a, F32x4 b) noexcept { return {vceqq_f32(a.v, b.v)}; }
SND_SIMD_INLINE M32x4 CmpNe(F32x4 a, F32x4 b) noexcept {
  return {vmvnq_u32(vceqq_f32(a.v, b.v))};
}
SND_SIMD_INLINE M32x4 operator&(M32x4 a, M32x4 b) noexcept { return {vandq_u32(a.v, b.v)}; }
SND_SIMD_INLINE M32x4 operator|(M32x4 a, M32x4 b) noexcept { return {vorrq_u32(a.v, b.v)}; }
SND_SIMD_INLINE M32x4 operator^(M32x4 a, M32x4 b) noexcept { return {veorq_u32(a.v, b.v)}; }
SND_SIMD_INLINE M32x4 operator~(M32x4 a) noexcept { return {vmvnq_u32(a.v)}; }
SND_SIMD_INLINE F32x4 Select(M32x4 m, F32x4 a, F32x4 b) noexcept {
  return {vbslq_f32(m.v, a.v, b.v)};
}
SND_SIMD_INLINE std::uint32_t MoveMask(M32x4 m) noexcept {
  const std::uint32_t w[4] = {1u, 2u, 4u, 8u};
  uint32x4_t bits = vandq_u32(m.v, vld1q_u32(w));
  return vaddvq_u32(bits);
}
SND_SIMD_INLINE float GetLane0(F32x4 a) noexcept { return vgetq_lane_f32(a.v, 0); }
SND_SIMD_INLINE float ReduceAdd(F32x4 a) noexcept {
  float32x2_t lo = vget_low_f32(a.v), hi = vget_high_f32(a.v);
  float32x2_t s = vadd_f32(lo, hi);  // v0+v2, v1+v3
  return vget_lane_f32(s, 0) + vget_lane_f32(s, 1);
}
SND_SIMD_INLINE float ReduceMax(F32x4 a) noexcept {
  float32x2_t lo = vget_low_f32(a.v), hi = vget_high_f32(a.v);
  float32x2_t s = vbsl_f32(vcgt_f32(lo, hi), lo, hi);
  float x = vget_lane_f32(s, 0), y = vget_lane_f32(s, 1);
  return x > y ? x : y;
}
SND_SIMD_INLINE float ReduceMin(F32x4 a) noexcept {
  float32x2_t lo = vget_low_f32(a.v), hi = vget_high_f32(a.v);
  float32x2_t s = vbsl_f32(vclt_f32(lo, hi), lo, hi);
  float x = vget_lane_f32(s, 0), y = vget_lane_f32(s, 1);
  return x < y ? x : y;
}

SND_SIMD_INLINE I32x4 operator+(I32x4 a, I32x4 b) noexcept { return {vaddq_s32(a.v, b.v)}; }
SND_SIMD_INLINE I32x4 operator-(I32x4 a, I32x4 b) noexcept { return {vsubq_s32(a.v, b.v)}; }
SND_SIMD_INLINE I32x4 operator&(I32x4 a, I32x4 b) noexcept { return {vandq_s32(a.v, b.v)}; }
SND_SIMD_INLINE I32x4 operator|(I32x4 a, I32x4 b) noexcept { return {vorrq_s32(a.v, b.v)}; }
SND_SIMD_INLINE I32x4 operator^(I32x4 a, I32x4 b) noexcept { return {veorq_s32(a.v, b.v)}; }
template <int N>
SND_SIMD_INLINE I32x4 ShiftLeft(I32x4 a) noexcept {
  return {vshlq_n_s32(a.v, N)};
}
template <int N>
SND_SIMD_INLINE I32x4 ShiftRightLogical(I32x4 a) noexcept {
  return {vreinterpretq_s32_u32(vshrq_n_u32(vreinterpretq_u32_s32(a.v), N))};
}
template <int N>
SND_SIMD_INLINE I32x4 ShiftRightArith(I32x4 a) noexcept {
  return {vshrq_n_s32(a.v, N)};
}
SND_SIMD_INLINE M32x4 CmpEq(I32x4 a, I32x4 b) noexcept { return {vceqq_s32(a.v, b.v)}; }
SND_SIMD_INLINE M32x4 CmpGt(I32x4 a, I32x4 b) noexcept { return {vcgtq_s32(a.v, b.v)}; }
SND_SIMD_INLINE I32x4 Select(M32x4 m, I32x4 a, I32x4 b) noexcept {
  return {vbslq_s32(m.v, a.v, b.v)};
}
SND_SIMD_INLINE I32x4 TruncToInt(F32x4 a) noexcept {
  // Match the x86 out-of-range result (INT32_MIN).
  uint32x4_t ok = vandq_u32(vcgeq_f32(a.v, vdupq_n_f32(-2147483648.0f)),
                            vcltq_f32(a.v, vdupq_n_f32(2147483648.0f)));
  return {vbslq_s32(ok, vcvtq_s32_f32(a.v), vdupq_n_s32(INT32_MIN))};
}
SND_SIMD_INLINE I32x4 RoundToInt(F32x4 a) noexcept {
  uint32x4_t ok = vandq_u32(vcgeq_f32(a.v, vdupq_n_f32(-2147483648.0f)),
                            vcltq_f32(a.v, vdupq_n_f32(2147483648.0f)));
  return {vbslq_s32(ok, vcvtnq_s32_f32(a.v), vdupq_n_s32(INT32_MIN))};
}
SND_SIMD_INLINE F32x4 ToFloat(I32x4 a) noexcept { return {vcvtq_f32_s32(a.v)}; }
SND_SIMD_INLINE I32x4 BitCastToInt(F32x4 a) noexcept { return {vreinterpretq_s32_f32(a.v)}; }
SND_SIMD_INLINE F32x4 BitCastToFloat(I32x4 a) noexcept { return {vreinterpretq_f32_s32(a.v)}; }
SND_SIMD_INLINE F32x4 Trunc(F32x4 a) noexcept { return {vrndq_f32(a.v)}; }
SND_SIMD_INLINE F32x4 Floor(F32x4 a) noexcept { return {vrndmq_f32(a.v)}; }
SND_SIMD_INLINE F32x4 Round(F32x4 a) noexcept { return {vrndnq_f32(a.v)}; }

#else  // scalar

struct M32x4 {
  std::uint32_t v[4];
};
struct I32x4 {
  std::int32_t v[4];
  static SND_SIMD_INLINE I32x4 Zero() noexcept { return {{0, 0, 0, 0}}; }
  static SND_SIMD_INLINE I32x4 Set1(std::int32_t x) noexcept { return {{x, x, x, x}}; }
  static SND_SIMD_INLINE I32x4 Set(std::int32_t a, std::int32_t b, std::int32_t c,
                                   std::int32_t d) noexcept {
    return {{a, b, c, d}};
  }
  static SND_SIMD_INLINE I32x4 LoadU(const std::int32_t* p) noexcept {
    return {{p[0], p[1], p[2], p[3]}};
  }
  SND_SIMD_INLINE void StoreU(std::int32_t* p) const noexcept {
    for (int i = 0; i < 4; ++i) p[i] = v[i];
  }
};
struct F32x4 {
  float v[4];
  static SND_SIMD_INLINE F32x4 Zero() noexcept { return {{0.0f, 0.0f, 0.0f, 0.0f}}; }
  static SND_SIMD_INLINE F32x4 Set1(float x) noexcept { return {{x, x, x, x}}; }
  static SND_SIMD_INLINE F32x4 Set(float a, float b, float c, float d) noexcept {
    return {{a, b, c, d}};
  }
  static SND_SIMD_INLINE F32x4 Load(const float* p) noexcept { return {{p[0], p[1], p[2], p[3]}}; }
  static SND_SIMD_INLINE F32x4 LoadU(const float* p) noexcept {
    return {{p[0], p[1], p[2], p[3]}};
  }
  SND_SIMD_INLINE void Store(float* p) const noexcept {
    for (int i = 0; i < 4; ++i) p[i] = v[i];
  }
  SND_SIMD_INLINE void StoreU(float* p) const noexcept {
    for (int i = 0; i < 4; ++i) p[i] = v[i];
  }
};

#define SND_SIMD_MAP4(expr) \
  F32x4 r;                  \
  for (int i = 0; i < 4; ++i) r.v[i] = (expr);  \
  return r
#define SND_SIMD_MASK4(cond) \
  M32x4 r;                   \
  for (int i = 0; i < 4; ++i) r.v[i] = (cond) ? 0xFFFFFFFFu : 0u; \
  return r
#define SND_SIMD_IMAP4(expr) \
  I32x4 r;                   \
  for (int i = 0; i < 4; ++i) r.v[i] = (expr);  \
  return r

SND_SIMD_INLINE F32x4 operator+(F32x4 a, F32x4 b) noexcept { SND_SIMD_MAP4(a.v[i] + b.v[i]); }
SND_SIMD_INLINE F32x4 operator-(F32x4 a, F32x4 b) noexcept { SND_SIMD_MAP4(a.v[i] - b.v[i]); }
SND_SIMD_INLINE F32x4 operator*(F32x4 a, F32x4 b) noexcept { SND_SIMD_MAP4(a.v[i] * b.v[i]); }
SND_SIMD_INLINE F32x4 operator/(F32x4 a, F32x4 b) noexcept { SND_SIMD_MAP4(a.v[i] / b.v[i]); }
SND_SIMD_INLINE F32x4 operator-(F32x4 a) noexcept { SND_SIMD_MAP4(-a.v[i]); }
SND_SIMD_INLINE F32x4 MulAdd(F32x4 a, F32x4 b, F32x4 c) noexcept {
  F32x4 r;
  for (int i = 0; i < 4; ++i) {
    float p = a.v[i] * b.v[i];  // separate statement: never contracted into an FMA
    r.v[i] = p + c.v[i];
  }
  return r;
}
SND_SIMD_INLINE F32x4 NegMulAdd(F32x4 a, F32x4 b, F32x4 c) noexcept {
  F32x4 r;
  for (int i = 0; i < 4; ++i) {
    float p = a.v[i] * b.v[i];
    r.v[i] = c.v[i] - p;
  }
  return r;
}
// x86 semantics: a < b ? a : b (second operand on NaN).
SND_SIMD_INLINE F32x4 Min(F32x4 a, F32x4 b) noexcept {
  SND_SIMD_MAP4(a.v[i] < b.v[i] ? a.v[i] : b.v[i]);
}
SND_SIMD_INLINE F32x4 Max(F32x4 a, F32x4 b) noexcept {
  SND_SIMD_MAP4(a.v[i] > b.v[i] ? a.v[i] : b.v[i]);
}
SND_SIMD_INLINE F32x4 Abs(F32x4 a) noexcept {
  SND_SIMD_MAP4(scalar_detail::FromBits(scalar_detail::Bits(a.v[i]) & 0x7FFFFFFFu));
}
SND_SIMD_INLINE F32x4 Sqrt(F32x4 a) noexcept { SND_SIMD_MAP4(std::sqrt(a.v[i])); }

SND_SIMD_INLINE M32x4 CmpLt(F32x4 a, F32x4 b) noexcept { SND_SIMD_MASK4(a.v[i] < b.v[i]); }
SND_SIMD_INLINE M32x4 CmpLe(F32x4 a, F32x4 b) noexcept { SND_SIMD_MASK4(a.v[i] <= b.v[i]); }
SND_SIMD_INLINE M32x4 CmpGt(F32x4 a, F32x4 b) noexcept { SND_SIMD_MASK4(a.v[i] > b.v[i]); }
SND_SIMD_INLINE M32x4 CmpGe(F32x4 a, F32x4 b) noexcept { SND_SIMD_MASK4(a.v[i] >= b.v[i]); }
SND_SIMD_INLINE M32x4 CmpEq(F32x4 a, F32x4 b) noexcept { SND_SIMD_MASK4(a.v[i] == b.v[i]); }
SND_SIMD_INLINE M32x4 CmpNe(F32x4 a, F32x4 b) noexcept { SND_SIMD_MASK4(!(a.v[i] == b.v[i])); }
SND_SIMD_INLINE M32x4 operator&(M32x4 a, M32x4 b) noexcept {
  M32x4 r;
  for (int i = 0; i < 4; ++i) r.v[i] = a.v[i] & b.v[i];
  return r;
}
SND_SIMD_INLINE M32x4 operator|(M32x4 a, M32x4 b) noexcept {
  M32x4 r;
  for (int i = 0; i < 4; ++i) r.v[i] = a.v[i] | b.v[i];
  return r;
}
SND_SIMD_INLINE M32x4 operator^(M32x4 a, M32x4 b) noexcept {
  M32x4 r;
  for (int i = 0; i < 4; ++i) r.v[i] = a.v[i] ^ b.v[i];
  return r;
}
SND_SIMD_INLINE M32x4 operator~(M32x4 a) noexcept {
  M32x4 r;
  for (int i = 0; i < 4; ++i) r.v[i] = ~a.v[i];
  return r;
}
SND_SIMD_INLINE F32x4 Select(M32x4 m, F32x4 a, F32x4 b) noexcept {
  SND_SIMD_MAP4(scalar_detail::FromBits((m.v[i] & scalar_detail::Bits(a.v[i])) |
                                        (~m.v[i] & scalar_detail::Bits(b.v[i]))));
}
SND_SIMD_INLINE std::uint32_t MoveMask(M32x4 m) noexcept {
  std::uint32_t r = 0;
  for (int i = 0; i < 4; ++i) r |= (m.v[i] >> 31) << i;
  return r;
}
SND_SIMD_INLINE float GetLane0(F32x4 a) noexcept { return a.v[0]; }
SND_SIMD_INLINE float ReduceAdd(F32x4 a) noexcept {
  float s0 = a.v[0] + a.v[2];
  float s1 = a.v[1] + a.v[3];
  return s0 + s1;
}
SND_SIMD_INLINE float ReduceMax(F32x4 a) noexcept {
  float s0 = a.v[0] > a.v[2] ? a.v[0] : a.v[2];
  float s1 = a.v[1] > a.v[3] ? a.v[1] : a.v[3];
  return s0 > s1 ? s0 : s1;
}
SND_SIMD_INLINE float ReduceMin(F32x4 a) noexcept {
  float s0 = a.v[0] < a.v[2] ? a.v[0] : a.v[2];
  float s1 = a.v[1] < a.v[3] ? a.v[1] : a.v[3];
  return s0 < s1 ? s0 : s1;
}

SND_SIMD_INLINE I32x4 operator+(I32x4 a, I32x4 b) noexcept {
  SND_SIMD_IMAP4(static_cast<std::int32_t>(static_cast<std::uint32_t>(a.v[i]) +
                                           static_cast<std::uint32_t>(b.v[i])));
}
SND_SIMD_INLINE I32x4 operator-(I32x4 a, I32x4 b) noexcept {
  SND_SIMD_IMAP4(static_cast<std::int32_t>(static_cast<std::uint32_t>(a.v[i]) -
                                           static_cast<std::uint32_t>(b.v[i])));
}
SND_SIMD_INLINE I32x4 operator&(I32x4 a, I32x4 b) noexcept { SND_SIMD_IMAP4(a.v[i] & b.v[i]); }
SND_SIMD_INLINE I32x4 operator|(I32x4 a, I32x4 b) noexcept { SND_SIMD_IMAP4(a.v[i] | b.v[i]); }
SND_SIMD_INLINE I32x4 operator^(I32x4 a, I32x4 b) noexcept { SND_SIMD_IMAP4(a.v[i] ^ b.v[i]); }
template <int N>
SND_SIMD_INLINE I32x4 ShiftLeft(I32x4 a) noexcept {
  SND_SIMD_IMAP4(static_cast<std::int32_t>(static_cast<std::uint32_t>(a.v[i]) << N));
}
template <int N>
SND_SIMD_INLINE I32x4 ShiftRightLogical(I32x4 a) noexcept {
  SND_SIMD_IMAP4(static_cast<std::int32_t>(static_cast<std::uint32_t>(a.v[i]) >> N));
}
template <int N>
SND_SIMD_INLINE I32x4 ShiftRightArith(I32x4 a) noexcept {
  SND_SIMD_IMAP4(a.v[i] >> N);
}
SND_SIMD_INLINE M32x4 CmpEq(I32x4 a, I32x4 b) noexcept { SND_SIMD_MASK4(a.v[i] == b.v[i]); }
SND_SIMD_INLINE M32x4 CmpGt(I32x4 a, I32x4 b) noexcept { SND_SIMD_MASK4(a.v[i] > b.v[i]); }
SND_SIMD_INLINE I32x4 Select(M32x4 m, I32x4 a, I32x4 b) noexcept {
  SND_SIMD_IMAP4(m.v[i] ? a.v[i] : b.v[i]);
}
SND_SIMD_INLINE I32x4 TruncToInt(F32x4 a) noexcept {
  SND_SIMD_IMAP4(scalar_detail::TruncToInt(a.v[i]));
}
SND_SIMD_INLINE I32x4 RoundToInt(F32x4 a) noexcept {
  SND_SIMD_IMAP4(scalar_detail::RoundToInt(a.v[i]));
}
SND_SIMD_INLINE F32x4 ToFloat(I32x4 a) noexcept { SND_SIMD_MAP4(static_cast<float>(a.v[i])); }
SND_SIMD_INLINE I32x4 BitCastToInt(F32x4 a) noexcept {
  SND_SIMD_IMAP4(static_cast<std::int32_t>(scalar_detail::Bits(a.v[i])));
}
SND_SIMD_INLINE F32x4 BitCastToFloat(I32x4 a) noexcept {
  SND_SIMD_MAP4(scalar_detail::FromBits(static_cast<std::uint32_t>(a.v[i])));
}
SND_SIMD_INLINE F32x4 Trunc(F32x4 a) noexcept { SND_SIMD_MAP4(scalar_detail::Trunc(a.v[i])); }
SND_SIMD_INLINE F32x4 Floor(F32x4 a) noexcept { SND_SIMD_MAP4(scalar_detail::Floor(a.v[i])); }
SND_SIMD_INLINE F32x4 Round(F32x4 a) noexcept { SND_SIMD_MAP4(scalar_detail::RoundEven(a.v[i])); }

#undef SND_SIMD_MAP4
#undef SND_SIMD_MASK4
#undef SND_SIMD_IMAP4

#endif  // 4-wide paths

// ===========================================================================
// 8-wide types
// ===========================================================================
#if defined(SND_SIMD_AVX2)

struct M32x8 {
  __m256 v;
};
struct I32x8 {
  __m256i v;
  static SND_SIMD_INLINE I32x8 Zero() noexcept { return {_mm256_setzero_si256()}; }
  static SND_SIMD_INLINE I32x8 Set1(std::int32_t x) noexcept { return {_mm256_set1_epi32(x)}; }
  static SND_SIMD_INLINE I32x8 LoadU(const std::int32_t* p) noexcept {
    return {_mm256_loadu_si256(reinterpret_cast<const __m256i*>(p))};
  }
  SND_SIMD_INLINE void StoreU(std::int32_t* p) const noexcept {
    _mm256_storeu_si256(reinterpret_cast<__m256i*>(p), v);
  }
};
struct F32x8 {
  __m256 v;
  static SND_SIMD_INLINE F32x8 Zero() noexcept { return {_mm256_setzero_ps()}; }
  static SND_SIMD_INLINE F32x8 Set1(float x) noexcept { return {_mm256_set1_ps(x)}; }
  static SND_SIMD_INLINE F32x8 Load(const float* p) noexcept { return {_mm256_load_ps(p)}; }
  static SND_SIMD_INLINE F32x8 LoadU(const float* p) noexcept { return {_mm256_loadu_ps(p)}; }
  SND_SIMD_INLINE void Store(float* p) const noexcept { _mm256_store_ps(p, v); }
  SND_SIMD_INLINE void StoreU(float* p) const noexcept { _mm256_storeu_ps(p, v); }
  static SND_SIMD_INLINE F32x8 Combine(F32x4 lo, F32x4 hi) noexcept {
    return {_mm256_insertf128_ps(_mm256_castps128_ps256(lo.v), hi.v, 1)};
  }
  SND_SIMD_INLINE F32x4 Lo() const noexcept { return {_mm256_castps256_ps128(v)}; }
  SND_SIMD_INLINE F32x4 Hi() const noexcept { return {_mm256_extractf128_ps(v, 1)}; }
};

SND_SIMD_INLINE F32x8 operator+(F32x8 a, F32x8 b) noexcept { return {_mm256_add_ps(a.v, b.v)}; }
SND_SIMD_INLINE F32x8 operator-(F32x8 a, F32x8 b) noexcept { return {_mm256_sub_ps(a.v, b.v)}; }
SND_SIMD_INLINE F32x8 operator*(F32x8 a, F32x8 b) noexcept { return {_mm256_mul_ps(a.v, b.v)}; }
SND_SIMD_INLINE F32x8 operator/(F32x8 a, F32x8 b) noexcept { return {_mm256_div_ps(a.v, b.v)}; }
SND_SIMD_INLINE F32x8 operator-(F32x8 a) noexcept {
  return {_mm256_xor_ps(a.v, _mm256_set1_ps(-0.0f))};
}
SND_SIMD_INLINE F32x8 MulAdd(F32x8 a, F32x8 b, F32x8 c) noexcept {
#if defined(__FMA__)
  return {_mm256_fmadd_ps(a.v, b.v, c.v)};
#else
  return {_mm256_add_ps(_mm256_mul_ps(a.v, b.v), c.v)};
#endif
}
SND_SIMD_INLINE F32x8 NegMulAdd(F32x8 a, F32x8 b, F32x8 c) noexcept {
#if defined(__FMA__)
  return {_mm256_fnmadd_ps(a.v, b.v, c.v)};
#else
  return {_mm256_sub_ps(c.v, _mm256_mul_ps(a.v, b.v))};
#endif
}
SND_SIMD_INLINE F32x8 Min(F32x8 a, F32x8 b) noexcept { return {_mm256_min_ps(a.v, b.v)}; }
SND_SIMD_INLINE F32x8 Max(F32x8 a, F32x8 b) noexcept { return {_mm256_max_ps(a.v, b.v)}; }
SND_SIMD_INLINE F32x8 Abs(F32x8 a) noexcept {
  return {_mm256_andnot_ps(_mm256_set1_ps(-0.0f), a.v)};
}
SND_SIMD_INLINE F32x8 Sqrt(F32x8 a) noexcept { return {_mm256_sqrt_ps(a.v)}; }
SND_SIMD_INLINE M32x8 CmpLt(F32x8 a, F32x8 b) noexcept {
  return {_mm256_cmp_ps(a.v, b.v, _CMP_LT_OQ)};
}
SND_SIMD_INLINE M32x8 CmpLe(F32x8 a, F32x8 b) noexcept {
  return {_mm256_cmp_ps(a.v, b.v, _CMP_LE_OQ)};
}
SND_SIMD_INLINE M32x8 CmpGt(F32x8 a, F32x8 b) noexcept {
  return {_mm256_cmp_ps(a.v, b.v, _CMP_GT_OQ)};
}
SND_SIMD_INLINE M32x8 CmpGe(F32x8 a, F32x8 b) noexcept {
  return {_mm256_cmp_ps(a.v, b.v, _CMP_GE_OQ)};
}
SND_SIMD_INLINE M32x8 CmpEq(F32x8 a, F32x8 b) noexcept {
  return {_mm256_cmp_ps(a.v, b.v, _CMP_EQ_OQ)};
}
SND_SIMD_INLINE M32x8 CmpNe(F32x8 a, F32x8 b) noexcept {
  return {_mm256_cmp_ps(a.v, b.v, _CMP_NEQ_UQ)};
}
SND_SIMD_INLINE M32x8 operator&(M32x8 a, M32x8 b) noexcept { return {_mm256_and_ps(a.v, b.v)}; }
SND_SIMD_INLINE M32x8 operator|(M32x8 a, M32x8 b) noexcept { return {_mm256_or_ps(a.v, b.v)}; }
SND_SIMD_INLINE M32x8 operator^(M32x8 a, M32x8 b) noexcept { return {_mm256_xor_ps(a.v, b.v)}; }
SND_SIMD_INLINE M32x8 operator~(M32x8 a) noexcept {
  return {_mm256_xor_ps(a.v, _mm256_castsi256_ps(_mm256_set1_epi32(-1)))};
}
SND_SIMD_INLINE F32x8 Select(M32x8 m, F32x8 a, F32x8 b) noexcept {
  return {_mm256_blendv_ps(b.v, a.v, m.v)};
}
SND_SIMD_INLINE std::uint32_t MoveMask(M32x8 m) noexcept {
  return static_cast<std::uint32_t>(_mm256_movemask_ps(m.v));
}
SND_SIMD_INLINE F32x4 LowHalf(F32x8 a) noexcept { return a.Lo(); }
SND_SIMD_INLINE F32x4 HighHalf(F32x8 a) noexcept { return a.Hi(); }

SND_SIMD_INLINE I32x8 operator+(I32x8 a, I32x8 b) noexcept {
  return {_mm256_add_epi32(a.v, b.v)};
}
SND_SIMD_INLINE I32x8 operator-(I32x8 a, I32x8 b) noexcept {
  return {_mm256_sub_epi32(a.v, b.v)};
}
SND_SIMD_INLINE I32x8 operator&(I32x8 a, I32x8 b) noexcept {
  return {_mm256_and_si256(a.v, b.v)};
}
SND_SIMD_INLINE I32x8 operator|(I32x8 a, I32x8 b) noexcept { return {_mm256_or_si256(a.v, b.v)}; }
SND_SIMD_INLINE I32x8 operator^(I32x8 a, I32x8 b) noexcept {
  return {_mm256_xor_si256(a.v, b.v)};
}
SND_SIMD_INLINE M32x8 CmpEq(I32x8 a, I32x8 b) noexcept {
  return {_mm256_castsi256_ps(_mm256_cmpeq_epi32(a.v, b.v))};
}
SND_SIMD_INLINE M32x8 CmpGt(I32x8 a, I32x8 b) noexcept {
  return {_mm256_castsi256_ps(_mm256_cmpgt_epi32(a.v, b.v))};
}
SND_SIMD_INLINE I32x8 TruncToInt(F32x8 a) noexcept { return {_mm256_cvttps_epi32(a.v)}; }
SND_SIMD_INLINE I32x8 RoundToInt(F32x8 a) noexcept { return {_mm256_cvtps_epi32(a.v)}; }
SND_SIMD_INLINE F32x8 ToFloat(I32x8 a) noexcept { return {_mm256_cvtepi32_ps(a.v)}; }
// Same construction as the 4-wide x86 path (identical bits, including the sign of zero).
SND_SIMD_INLINE F32x8 Trunc(F32x8 a) noexcept {
  const __m256 big = _mm256_set1_ps(8388608.0f);
  __m256 small = _mm256_cmp_ps(_mm256_andnot_ps(_mm256_set1_ps(-0.0f), a.v), big, _CMP_LT_OQ);
  __m256 t = _mm256_cvtepi32_ps(_mm256_cvttps_epi32(a.v));
  return {_mm256_blendv_ps(a.v, t, small)};
}
SND_SIMD_INLINE F32x8 Floor(F32x8 a) noexcept {
  F32x8 t = Trunc(a);
  __m256 gt = _mm256_cmp_ps(t.v, a.v, _CMP_GT_OQ);
  return {_mm256_sub_ps(t.v, _mm256_and_ps(gt, _mm256_set1_ps(1.0f)))};
}
SND_SIMD_INLINE F32x8 Round(F32x8 a) noexcept {
  const __m256 big = _mm256_set1_ps(8388608.0f);
  __m256 small = _mm256_cmp_ps(_mm256_andnot_ps(_mm256_set1_ps(-0.0f), a.v), big, _CMP_LT_OQ);
  __m256 r = _mm256_cvtepi32_ps(_mm256_cvtps_epi32(a.v));
  return {_mm256_blendv_ps(a.v, r, small)};
}

#else  // 8 wide = 2 x 4 wide

struct M32x8 {
  M32x4 lo, hi;
};
struct I32x8 {
  I32x4 lo, hi;
  static SND_SIMD_INLINE I32x8 Zero() noexcept { return {I32x4::Zero(), I32x4::Zero()}; }
  static SND_SIMD_INLINE I32x8 Set1(std::int32_t x) noexcept {
    return {I32x4::Set1(x), I32x4::Set1(x)};
  }
  static SND_SIMD_INLINE I32x8 LoadU(const std::int32_t* p) noexcept {
    return {I32x4::LoadU(p), I32x4::LoadU(p + 4)};
  }
  SND_SIMD_INLINE void StoreU(std::int32_t* p) const noexcept {
    lo.StoreU(p);
    hi.StoreU(p + 4);
  }
};
struct F32x8 {
  F32x4 lo, hi;
  static SND_SIMD_INLINE F32x8 Zero() noexcept { return {F32x4::Zero(), F32x4::Zero()}; }
  static SND_SIMD_INLINE F32x8 Set1(float x) noexcept { return {F32x4::Set1(x), F32x4::Set1(x)}; }
  static SND_SIMD_INLINE F32x8 Load(const float* p) noexcept {
    return {F32x4::Load(p), F32x4::Load(p + 4)};
  }
  static SND_SIMD_INLINE F32x8 LoadU(const float* p) noexcept {
    return {F32x4::LoadU(p), F32x4::LoadU(p + 4)};
  }
  SND_SIMD_INLINE void Store(float* p) const noexcept {
    lo.Store(p);
    hi.Store(p + 4);
  }
  SND_SIMD_INLINE void StoreU(float* p) const noexcept {
    lo.StoreU(p);
    hi.StoreU(p + 4);
  }
  static SND_SIMD_INLINE F32x8 Combine(F32x4 l, F32x4 h) noexcept { return {l, h}; }
  SND_SIMD_INLINE F32x4 Lo() const noexcept { return lo; }
  SND_SIMD_INLINE F32x4 Hi() const noexcept { return hi; }
};

SND_SIMD_INLINE F32x8 operator+(F32x8 a, F32x8 b) noexcept { return {a.lo + b.lo, a.hi + b.hi}; }
SND_SIMD_INLINE F32x8 operator-(F32x8 a, F32x8 b) noexcept { return {a.lo - b.lo, a.hi - b.hi}; }
SND_SIMD_INLINE F32x8 operator*(F32x8 a, F32x8 b) noexcept { return {a.lo * b.lo, a.hi * b.hi}; }
SND_SIMD_INLINE F32x8 operator/(F32x8 a, F32x8 b) noexcept { return {a.lo / b.lo, a.hi / b.hi}; }
SND_SIMD_INLINE F32x8 operator-(F32x8 a) noexcept { return {-a.lo, -a.hi}; }
SND_SIMD_INLINE F32x8 MulAdd(F32x8 a, F32x8 b, F32x8 c) noexcept {
  return {MulAdd(a.lo, b.lo, c.lo), MulAdd(a.hi, b.hi, c.hi)};
}
SND_SIMD_INLINE F32x8 NegMulAdd(F32x8 a, F32x8 b, F32x8 c) noexcept {
  return {NegMulAdd(a.lo, b.lo, c.lo), NegMulAdd(a.hi, b.hi, c.hi)};
}
SND_SIMD_INLINE F32x8 Min(F32x8 a, F32x8 b) noexcept { return {Min(a.lo, b.lo), Min(a.hi, b.hi)}; }
SND_SIMD_INLINE F32x8 Max(F32x8 a, F32x8 b) noexcept { return {Max(a.lo, b.lo), Max(a.hi, b.hi)}; }
SND_SIMD_INLINE F32x8 Abs(F32x8 a) noexcept { return {Abs(a.lo), Abs(a.hi)}; }
SND_SIMD_INLINE F32x8 Sqrt(F32x8 a) noexcept { return {Sqrt(a.lo), Sqrt(a.hi)}; }
SND_SIMD_INLINE M32x8 CmpLt(F32x8 a, F32x8 b) noexcept {
  return {CmpLt(a.lo, b.lo), CmpLt(a.hi, b.hi)};
}
SND_SIMD_INLINE M32x8 CmpLe(F32x8 a, F32x8 b) noexcept {
  return {CmpLe(a.lo, b.lo), CmpLe(a.hi, b.hi)};
}
SND_SIMD_INLINE M32x8 CmpGt(F32x8 a, F32x8 b) noexcept {
  return {CmpGt(a.lo, b.lo), CmpGt(a.hi, b.hi)};
}
SND_SIMD_INLINE M32x8 CmpGe(F32x8 a, F32x8 b) noexcept {
  return {CmpGe(a.lo, b.lo), CmpGe(a.hi, b.hi)};
}
SND_SIMD_INLINE M32x8 CmpEq(F32x8 a, F32x8 b) noexcept {
  return {CmpEq(a.lo, b.lo), CmpEq(a.hi, b.hi)};
}
SND_SIMD_INLINE M32x8 CmpNe(F32x8 a, F32x8 b) noexcept {
  return {CmpNe(a.lo, b.lo), CmpNe(a.hi, b.hi)};
}
SND_SIMD_INLINE M32x8 operator&(M32x8 a, M32x8 b) noexcept { return {a.lo & b.lo, a.hi & b.hi}; }
SND_SIMD_INLINE M32x8 operator|(M32x8 a, M32x8 b) noexcept { return {a.lo | b.lo, a.hi | b.hi}; }
SND_SIMD_INLINE M32x8 operator^(M32x8 a, M32x8 b) noexcept { return {a.lo ^ b.lo, a.hi ^ b.hi}; }
SND_SIMD_INLINE M32x8 operator~(M32x8 a) noexcept { return {~a.lo, ~a.hi}; }
SND_SIMD_INLINE F32x8 Select(M32x8 m, F32x8 a, F32x8 b) noexcept {
  return {Select(m.lo, a.lo, b.lo), Select(m.hi, a.hi, b.hi)};
}
SND_SIMD_INLINE std::uint32_t MoveMask(M32x8 m) noexcept {
  return MoveMask(m.lo) | (MoveMask(m.hi) << 4);
}
SND_SIMD_INLINE F32x4 LowHalf(F32x8 a) noexcept { return a.lo; }
SND_SIMD_INLINE F32x4 HighHalf(F32x8 a) noexcept { return a.hi; }
SND_SIMD_INLINE I32x8 operator+(I32x8 a, I32x8 b) noexcept { return {a.lo + b.lo, a.hi + b.hi}; }
SND_SIMD_INLINE I32x8 operator-(I32x8 a, I32x8 b) noexcept { return {a.lo - b.lo, a.hi - b.hi}; }
SND_SIMD_INLINE I32x8 operator&(I32x8 a, I32x8 b) noexcept { return {a.lo & b.lo, a.hi & b.hi}; }
SND_SIMD_INLINE I32x8 operator|(I32x8 a, I32x8 b) noexcept { return {a.lo | b.lo, a.hi | b.hi}; }
SND_SIMD_INLINE I32x8 operator^(I32x8 a, I32x8 b) noexcept { return {a.lo ^ b.lo, a.hi ^ b.hi}; }
SND_SIMD_INLINE M32x8 CmpEq(I32x8 a, I32x8 b) noexcept {
  return {CmpEq(a.lo, b.lo), CmpEq(a.hi, b.hi)};
}
SND_SIMD_INLINE M32x8 CmpGt(I32x8 a, I32x8 b) noexcept {
  return {CmpGt(a.lo, b.lo), CmpGt(a.hi, b.hi)};
}
SND_SIMD_INLINE I32x8 TruncToInt(F32x8 a) noexcept { return {TruncToInt(a.lo), TruncToInt(a.hi)}; }
SND_SIMD_INLINE I32x8 RoundToInt(F32x8 a) noexcept { return {RoundToInt(a.lo), RoundToInt(a.hi)}; }
SND_SIMD_INLINE F32x8 ToFloat(I32x8 a) noexcept { return {ToFloat(a.lo), ToFloat(a.hi)}; }
SND_SIMD_INLINE F32x8 Trunc(F32x8 a) noexcept { return {Trunc(a.lo), Trunc(a.hi)}; }
SND_SIMD_INLINE F32x8 Floor(F32x8 a) noexcept { return {Floor(a.lo), Floor(a.hi)}; }
SND_SIMD_INLINE F32x8 Round(F32x8 a) noexcept { return {Round(a.lo), Round(a.hi)}; }

#endif  // 8-wide

// Reductions of 8 lanes: lanes i and i+4 are added first, then the 4-wide order.
SND_SIMD_INLINE float ReduceAdd(F32x8 a) noexcept { return ReduceAdd(LowHalf(a) + HighHalf(a)); }
SND_SIMD_INLINE float ReduceMax(F32x8 a) noexcept {
  return ReduceMax(Max(LowHalf(a), HighHalf(a)));
}
SND_SIMD_INLINE float ReduceMin(F32x8 a) noexcept {
  return ReduceMin(Min(LowHalf(a), HighHalf(a)));
}

// Widest native vector.
#if defined(SND_SIMD_AVX2)
using F32xN = F32x8;
using I32xN = I32x8;
using M32xN = M32x8;
#else
using F32xN = F32x4;
using I32xN = I32x4;
using M32xN = M32x4;
#endif

// Generic helpers usable with any width.
template <class V>
SND_SIMD_INLINE V Clamp(V x, V lo, V hi) noexcept {
  return Min(Max(x, lo), hi);
}
template <class V>
SND_SIMD_INLINE V Lerp(V a, V b, V t) noexcept {  // a + (b - a) * t
  return MulAdd(b - a, t, a);
}

}  // inline namespace v_*
}  // namespace snd::simd

#endif  // SND_PLATFORM_SIMD_HPP
