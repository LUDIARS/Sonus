// D-150..157. The scene uses a mono PCMF source, with separately observable
// direct and auxiliary buses. Only the selected bus is a child of the master.
#ifndef SND_CORE_TEST_INDEP_G1_SCENE_HPP
#define SND_CORE_TEST_INDEP_G1_SCENE_HPP

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

#include "harness.hpp"

namespace indep_g1 {
using namespace coretest;
using snd::HashId;

inline constexpr auto kDry = HashId("Direct");
inline constexpr auto kWet = HashId("Zone");
inline constexpr auto kEvent = HashId("Tone");
inline constexpr snd_vec3 kFront{0, 0, 1};
inline constexpr snd_vec3 kOrigin{};

inline snd_config Config() {
  auto cfg = SmallConfig();
  cfg.tuning.lod_down_blocks = 0;
  cfg.tuning.rt_spin_microseconds = 5000000;
  return cfg;
}

inline snd_listener_desc Listener(snd_vec3 pos = {}, float weight = 1.0f) {
  return snd_listener_desc{pos, kFront, {0, 1, 0}, {}, weight, 1};
}

inline TestBank Signal(bool wet = false, float hz = 0, std::size_t frames = 8192) {
  TestBank b;
  b.buses.push_back(TBus{kDry, wet ? kNone : 2u, 2, wet ? static_cast<std::uint32_t>(bf::kBusOutput) : 0u, 0, {}, {}});
  b.buses.push_back(TBus{kWet, wet ? 2u : kNone, 2, wet ? 0u : static_cast<std::uint32_t>(bf::kBusOutput), 0, {}, {}});
  b.buses.push_back(TBus{HashId("Master"), kNone, 2, bf::kBusMaster, 0, {}, {}});
  std::vector<float> data(frames, 0.1f);
  if (hz != 0) {
    for (std::size_t i = 0; i < frames; ++i)
      data[i] *= static_cast<float>(std::sin(6.283185307179586 * hz * static_cast<double>(i) / 48000.0));
  }
  b.assets.push_back(TAsset{HashId("PCM"), 1, 48000, data, 0, frames});
  b.events.push_back(SimpleEvent("Tone", 0));
  b.events[0].tracks[0].clips[0].loopCount = 0;
  b.events[0].flags = bf::kEvent3D;
  return b;
}

inline std::uint32_t Curve(TestBank& b, bf::CurveTarget target, float a, float z, float distance = 4) {
  const auto ix = static_cast<std::uint32_t>(b.curves.size());
  const auto compose = target == bf::CurveTarget::Highpass ? bf::CurveCompose::Max : bf::CurveCompose::Min;
  b.curves.push_back(TCurve{bf::kInputDistance, static_cast<std::uint32_t>(target),
                            static_cast<std::uint32_t>(compose), {{0, a}, {distance, z}}});
  return ix;
}

struct Scene {
  explicit Scene(const TestBank& b, const snd_config& cfg = Config(), snd::IAllocator* alloc = nullptr)
      : Scene(b.Build(HashId("Scene")), cfg, alloc) {}
  explicit Scene(const std::vector<std::uint8_t>& bytes, const snd_config& cfg = Config(),
                 snd::IAllocator* alloc = nullptr) : h(cfg, {}, nullptr, 0, alloc) {
    SND_REQUIRE_EQ(h.created, SND_OK);
    const auto bank = h.Load("fixture", bytes);
    SND_REQUIRE_EQ(h.sys->GetBankState(bank), SND_BANK_STATE_LOADED);
    em = h.sys->CreateEmitter();
    SND_REQUIRE(em.value != 0);
    const auto listener = Listener();
    SND_CHECK_EQ(h.sys->SetListener(0, &listener), SND_OK);
    Position(kFront);
    h.Step();
  }
  void Position(snd_vec3 pos, snd_vec3 forward = kFront, snd_vec3 velocity = {}) {
    SND_CHECK_EQ(h.sys->SetTransform(em, &pos, &forward, &velocity), SND_OK);
  }
  snd_playback_handle Play() {
    h.Clear();
    const auto playback = h.sys->Play(kEvent, em, nullptr);
    SND_CHECK(playback.value != 0);
    return playback;
  }
  void Start() {
    Play();
    h.Steps(8);
  }
  double Level(std::uint32_t channel = 0) const {
    const auto last = h.TotalFrames();
    double sumSquares = 0.0;
    for (auto i = last - h.Frames(); i < last; ++i) {
      const double sample = h.At(i, channel);
      sumSquares += sample * sample;
    }
    return std::sqrt(sumSquares / h.Frames());
  }
  Harness h;
  snd_emitter_handle em{};
};
}  // namespace indep_g1
#endif
