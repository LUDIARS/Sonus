// D-217(8): exercise the absolute counter through normal public calls. The
// generated source has a 256-frame period, so whole blocks can reuse memory.
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <memory>
#include <vector>

#include "snd/dsp/time_stretch.hpp"
#include "snd/test/test.hpp"

namespace {
using namespace snd::dsp;
constexpr unsigned rate = 65536;
constexpr float windowMs = 125;
constexpr unsigned hop = 4096;       // rate * windowMs / 2000
constexpr unsigned block = 1u << 20;

struct Instance {
  std::vector<unsigned char> storage;
  void* state;
  Instance() {
    const auto bytes = TimeStretchStateBytes(1, rate, 2, windowMs, 0);
    storage.resize(bytes + 63);
    void* ptr = storage.data();
    auto space = storage.size();
    state = std::align(64, bytes, ptr, space);
    if (state != nullptr && !TimeStretchInit(state, bytes, 1, rate, 2, windowMs, 0)) state = nullptr;
  }
};

float Sample(std::uint64_t frame) {
  return static_cast<float>((frame * 73u + 31u) & 255u) / 256.0f - 0.5f;
}
}

SND_TEST(IndepTimeStretch, AbsoluteFrameCarryPreservesFractionAndWaveform) {
  Instance stream, reference;
  SND_REQUIRE(stream.state != nullptr && reference.state != nullptr);
  const auto pad = TimeStretchPadFrames(stream.state);
  SND_REQUIRE_EQ(TimeStretchHopFrames(stream.state), hop);
  std::vector<float> input(pad + block + 2 * hop), rendered(block), expected(block);
  float* in[] = {input.data()};
  float* out[] = {rendered.data()};
  float* alt[] = {expected.data()};
  for (unsigned i = pad; i < input.size(); ++i) input[i] = Sample(i - pad);

  constexpr std::uint64_t boundary = std::uint64_t{1} << 32;
  constexpr unsigned warmCalls = boundary / block - 1;
  for (unsigned i = 0; i < warmCalls; ++i) {
    const auto need = TimeStretchInputFrames(stream.state, block, 1, 1);
    SND_REQUIRE_EQ(need, i == 0 ? block + hop : block);
    SND_REQUIRE_EQ(TimeStretchProcess(stream.state, in, need, out, block, 1, 1), need);
    SND_REQUIRE_EQ(TimeStretchConsumedFrames(stream.state), std::uint64_t{i + 1u} * block + hop);
  }
  const auto first = TimeStretchInputFrames(reference.state, block, 1, 1);
  SND_REQUIRE_EQ(TimeStretchProcess(reference.state, in, first, alt, block, 1, 1), first);
  SND_CHECK(std::memcmp(expected.data(), rendered.data(), block * sizeof(float)) == 0);
  SND_CHECK(std::memcmp(rendered.data(), input.data() + pad, block * sizeof(float)) == 0);

  // Histories are equal at this point but the older instance is one block
  // short of the 32-bit boundary. Each new hop advances exactly 4096.5
  // input frames. The calls after crossing include partial and whole hops.
  const std::uint64_t difference = std::uint64_t{warmCalls - 1u} * block;
  constexpr float speed = 1.0f + 1.0f / 8192;
  for (unsigned count : {block, 1u, hop - 1u, hop + 3u, 3u * hop}) {
    const auto start = TimeStretchConsumedFrames(stream.state);
    const auto n = TimeStretchInputFrames(stream.state, count, speed, speed);
    SND_REQUIRE_EQ(n, TimeStretchInputFrames(reference.state, count, speed, speed));
    SND_REQUIRE(n <= input.size() - pad);
    if (count == block) SND_CHECK_EQ(n, block + (block / hop) / 2u);
    for (unsigned i = 0; i < n; ++i) input[pad + i] = Sample(start + i);
    SND_REQUIRE_EQ(TimeStretchProcess(stream.state, in, n, out, count, speed, speed), n);
    SND_REQUIRE_EQ(TimeStretchProcess(reference.state, in, n, alt, count, speed, speed), n);
    SND_CHECK(std::memcmp(expected.data(), rendered.data(), count * sizeof(float)) == 0);
    SND_CHECK_EQ(TimeStretchConsumedFrames(stream.state), start + n);
    SND_CHECK_EQ(TimeStretchConsumedFrames(stream.state), difference + TimeStretchConsumedFrames(reference.state));
    SND_CHECK_EQ(TimeStretchPendingOffset(stream.state), TimeStretchPendingOffset(reference.state));
  }
  SND_CHECK(TimeStretchConsumedFrames(stream.state) > boundary);
  TimeStretchReset(stream.state);
  SND_CHECK_EQ(TimeStretchConsumedFrames(stream.state), 0u);
  SND_CHECK_EQ(TimeStretchPendingOffset(stream.state), 0.0);
}
