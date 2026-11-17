// Independent test of the float -> integer conversion the backends share
// (CONTRACTS.md D-62: S32 / S16, clip to [-1, 1], nearest, NaN -> 0).
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>

#include "common/sample_format.hpp"
#include "snd/test/test.hpp"

using namespace snd::platform;

// D-62 conversion: clip to [-1, 1], round to nearest, NaN -> 0 (S32 and S16).
SND_TEST(IndepSampleFormat, ConversionIsClippedRoundedAndNanSafe) {
  using detail::ConvertFromFloat;
  using detail::SampleFormat;
  const float nan = std::numeric_limits<float>::quiet_NaN();
  const float inf = std::numeric_limits<float>::infinity();
  const float in[] = {0.0f, 1.0f, -1.0f, 2.0f, -3.0f, nan, inf, -inf, 0.5f, -0.5f, 1e-30f, 0.25f / 32767.0f,
                      0.75f / 32767.0f, -0.75f / 32767.0f};
  const std::size_t n = sizeof(in) / sizeof(in[0]);
  std::int16_t s16[n];
  ConvertFromFloat(in, s16, n, SampleFormat::Int16);
  const std::int16_t e16[] = {0, 32767, -32767, 32767, -32767, 0, 32767, -32767, 16384, -16384, 0, 0, 1, -1};
  for (std::size_t i = 0; i < n; ++i) {
    if (s16[i] != e16[i]) snd::test::Note("s16[%zu] = %d, expected %d", i, s16[i], e16[i]);
    SND_CHECK_EQ(s16[i], e16[i]);
  }
  std::int32_t s32[n];
  ConvertFromFloat(in, s32, n, SampleFormat::Int32);
  SND_CHECK_EQ(s32[0], 0);
  SND_CHECK_EQ(s32[1], 2147483647);
  SND_CHECK_EQ(s32[2], -2147483647);
  SND_CHECK_EQ(s32[3], 2147483647);
  SND_CHECK_EQ(s32[4], -2147483647);
  SND_CHECK_EQ(s32[5], 0);
  SND_CHECK_EQ(s32[6], 2147483647);
  SND_CHECK_EQ(s32[7], -2147483647);
  SND_CHECK_EQ(s32[8], 1073741824);   // 1073741823.5 rounds away from zero
  SND_CHECK_EQ(s32[9], -1073741824);
  SND_CHECK_EQ(s32[10], 0);
  // Every float in [-1, 1] lands within 0.5 LSB of x * (2^31 - 1).
  bool ok = true;
  std::uint32_t lcg = 1u;
  for (int i = 0; i < 100000; ++i) {
    lcg = lcg * 1664525u + 1013904223u;
    const float x = static_cast<float>(static_cast<std::int32_t>(lcg)) / 2147483648.0f;
    std::int32_t y;
    std::int16_t z;
    ConvertFromFloat(&x, &y, 1, SampleFormat::Int32);
    ConvertFromFloat(&x, &z, 1, SampleFormat::Int16);
    ok = ok && std::fabs(static_cast<double>(y) - static_cast<double>(x) * 2147483647.0) <= 0.5;
    ok = ok && std::fabs(static_cast<double>(z) - static_cast<double>(x) * 32767.0) <= 0.5;
  }
  SND_CHECK(ok);
  float f[n];
  ConvertFromFloat(in, f, n, SampleFormat::Float32);
  SND_CHECK(std::memcmp(f, in, sizeof(f)) == 0);  // float: bit copy (NaN included)
}

