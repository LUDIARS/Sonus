// Tests of the SIMD abstraction (design.md 3.4): every compiled path must give
// the same results as the scalar path (bit-exact, except fused multiply-add,
// which is compared with a tolerance when the path fuses).
#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>

#include "simd_kernels.hpp"
#include "snd/platform/simd.hpp"
#include "snd/test/test.hpp"

namespace {

constexpr std::uint32_t kN = 128;

struct Inputs {
  float a[kN], b[kN], c[kN];
};

void MakeInputs(Inputs* in) {
  std::uint32_t s = 0x12345678u;
  auto next = [&s]() {
    s = s * 1664525u + 1013904223u;
    return static_cast<float>(static_cast<std::int32_t>(s >> 8) - (1 << 23)) / static_cast<float>(1 << 22);
  };
  for (std::uint32_t i = 0; i < kN; ++i) {
    in->a[i] = next();
    in->b[i] = next();
    in->c[i] = next();
  }
  // Edge values: zeros of both signs, equal operands, exact halves, large values.
  const float edges[] = {0.0f, -0.0f, 0.5f / 7.3f, -1.5f / 7.3f, 1.0e30f, -1.0e30f, 3.0f, -3.0f};
  for (std::uint32_t i = 0; i < 8; ++i) {
    in->a[i] = edges[i];
    in->b[i + 8] = in->a[i + 8];
    in->c[i + 16] = in->b[i + 16];
  }
}

bool SameBits(float x, float y) {
  std::uint32_t a, b;
  std::memcpy(&a, &x, 4);
  std::memcpy(&b, &y, 4);
  if (std::isnan(x) && std::isnan(y)) return true;
  return a == b;
}

void Compare(const SimdResults& ref, const SimdResults& act) {
  SND_REQUIRE_EQ(ref.fCount, act.fCount);
  SND_REQUIRE_EQ(ref.iCount, act.iCount);
  SND_REQUIRE_EQ(ref.fusedCount, act.fusedCount);
  std::uint32_t bad = 0;
  for (std::uint32_t k = 0; k < ref.fCount; ++k) {
    if (!SameBits(ref.f[k], act.f[k])) {
      if (bad++ < 5) {
        SND_CHECK_MSG(false, "%s: float result %u differs: %.9g vs %.9g", act.path, k,
                      static_cast<double>(ref.f[k]), static_cast<double>(act.f[k]));
      }
    }
  }
  for (std::uint32_t k = 0; k < ref.iCount; ++k) {
    if (ref.i[k] != act.i[k]) {
      if (bad++ < 10) {
        SND_CHECK_MSG(false, "%s: int result %u differs: %d vs %d", act.path, k, ref.i[k],
                      act.i[k]);
      }
    }
  }
  for (std::uint32_t k = 0; k < ref.fusedCount; ++k) {
    if (act.fusedMulAdd) {
      const double tol = 1e-5 * (1.0 + std::fabs(static_cast<double>(ref.fused[k])));
      SND_CHECK_NEAR(act.fused[k], ref.fused[k], tol);
    } else if (!SameBits(ref.fused[k], act.fused[k])) {
      if (bad++ < 15) {
        SND_CHECK_MSG(false, "%s: fused result %u differs", act.path, k);
      }
    }
  }
  SND_CHECK_EQ(bad, 0u);
}

}  // namespace

SND_TEST(Simd, NativeMatchesScalar) {
  auto in = std::make_unique<Inputs>();
  MakeInputs(in.get());
  auto ref = std::make_unique<SimdResults>();
  auto act = std::make_unique<SimdResults>();
  SimdKernelsScalar(in->a, in->b, in->c, kN, ref.get());
  SimdKernelsNative(in->a, in->b, in->c, kN, act.get());
  SND_CHECK(std::strcmp(ref->path, "scalar") == 0);
  snd::test::Note("native SIMD path: %s (width %u, fused %d)", act->path, act->width,
                  act->fusedMulAdd ? 1 : 0);
  Compare(*ref, *act);
}

#if defined(SND_SIMD_TEST_HAVE_AVX2)
SND_TEST(Simd, Avx2MatchesScalar) {
  if (!__builtin_cpu_supports("avx2") || !__builtin_cpu_supports("fma")) {
    snd::test::Note("CPU without AVX2/FMA: AVX2 path not run");
    return;
  }
  auto in = std::make_unique<Inputs>();
  MakeInputs(in.get());
  auto ref = std::make_unique<SimdResults>();
  auto act = std::make_unique<SimdResults>();
  SimdKernelsScalar(in->a, in->b, in->c, kN, ref.get());
  SimdKernelsAvx2(in->a, in->b, in->c, kN, act.get());
  SND_CHECK(std::strcmp(act->path, "avx2") == 0 || std::strcmp(act->path, "scalar") == 0);
  Compare(*ref, *act);
}
#endif

SND_TEST(Simd, KnownValues) {
  using namespace snd::simd;
  alignas(64) float buf[8] = {1.5f, -2.5f, 3.25f, -0.0f, 7.0f, 8.0f, -9.0f, 10.0f};
  SND_CHECK(IsAligned(buf));
  SND_CHECK(!IsAligned(buf + 1));
  F32x4 v = F32x4::Load(buf);
  SND_CHECK_EQ(ReduceAdd(v), 1.5f + 3.25f + (-2.5f + -0.0f));
  SND_CHECK_EQ(ReduceMax(v), 3.25f);
  SND_CHECK_EQ(ReduceMin(v), -2.5f);
  float out[4];
  Floor(v).StoreU(out);
  SND_CHECK_EQ(out[0], 1.0f);
  SND_CHECK_EQ(out[1], -3.0f);
  Round(F32x4::Set(0.5f, 1.5f, 2.5f, -2.5f)).StoreU(out);
  SND_CHECK_EQ(out[0], 0.0f);
  SND_CHECK_EQ(out[1], 2.0f);
  SND_CHECK_EQ(out[2], 2.0f);
  SND_CHECK_EQ(out[3], -2.0f);
  SND_CHECK_EQ(MoveMask(CmpLt(v, F32x4::Zero())), 0x2u);
  F32x8 w = F32x8::Load(buf);
  SND_CHECK_EQ(ReduceAdd(w), (1.5f + 7.0f + (3.25f + -9.0f)) + (-2.5f + 8.0f + (-0.0f + 10.0f)));
  SND_CHECK_EQ(MoveMask(CmpGt(w, F32x8::Set1(5.0f))), 0xB0u);
  std::int32_t iv[8];
  TruncToInt(w).StoreU(iv);
  SND_CHECK_EQ(iv[1], -2);
  SND_CHECK_EQ(iv[7], 10);
  SND_CHECK(kWidth == 4 || kWidth == 8);
}
