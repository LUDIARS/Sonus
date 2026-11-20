// Noise kind (noise_kind.hpp).
#include "noise_kind.hpp"

#include <cmath>
#include <new>

namespace snd::source::detail {

namespace {

constexpr std::size_t Align64(std::size_t n) noexcept { return (n + 63u) & ~std::size_t{63u}; }
std::uint32_t Cap(const SourceLimits& l) noexcept { return l.maxVoices == 0u ? 1u : l.maxVoices; }
constexpr std::uint64_t kGolden = 0x9E3779B97F4A7C15ull;

constexpr std::uint64_t Mix64(std::uint64_t z) noexcept {
  z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
  z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
  return z ^ (z >> 31);
}

// Kellett's economy pink filter: poles and input gains, plus the direct term.
constexpr double kPinkPole[3] = {0.99765, 0.96300, 0.57000};
constexpr double kPinkGain[3] = {0.0990460, 0.2965164, 1.0526913};
constexpr double kPinkDirect = 0.1848;
constexpr double kBrownPole = 0.995;
constexpr double kColoredRms = 0.2;  // D-199

constexpr std::uint32_t kColorWhite = 0, kColorPink = 1, kColorBrown = 2;

std::uint32_t LoadState(const NoiseInst& in) noexcept {
  return std::atomic_ref<std::uint32_t>(const_cast<std::uint32_t&>(in.state)).load(std::memory_order_acquire);
}

// D-199: the coloured outputs are cut to [-1, 1] (rarely reached at RMS 0.2).
inline float Clip(float v) noexcept { return v > 1.0f ? 1.0f : (v < -1.0f ? -1.0f : v); }

// Rate ratio -> 32.32 source samples per output sample (deterministic rounding).
inline std::uint64_t Increment(double ratio) noexcept {
  if (!(ratio > 0.0)) return 0u;
  if (ratio > 1024.0) ratio = 1024.0;
  return static_cast<std::uint64_t>(ratio * 4294967296.0 + 0.5);
}

}  // namespace

float NoiseKind::White(std::uint64_t channelKey, std::uint64_t index) noexcept {
  const std::uint64_t z = Mix64(channelKey + (index + 1u) * kGolden);
  const std::int32_t v = static_cast<std::int32_t>(z >> 40) - 0x800000;  // 24 bits, [-2^23, 2^23)
  return static_cast<float>(v) * (1.0f / 8388608.0f);
}

std::uint64_t NoiseKind::ChannelKey(std::uint64_t key, std::uint32_t channel) noexcept {
  return Mix64(key ^ ((std::uint64_t{channel} + 1u) * kGolden));
}

std::uint64_t NoiseKind::VoiceKey(std::uint32_t paramsSeed, std::uint32_t startSeed) noexcept {
  return Mix64((std::uint64_t{paramsSeed} << 32) | startSeed);
}

float NoiseKind::Step(std::uint32_t color, float* f, float w) const noexcept {
  if (color == kColorPink) {
    f[0] = static_cast<float>(kPinkPole[0]) * f[0] + static_cast<float>(kPinkGain[0]) * w;
    f[1] = static_cast<float>(kPinkPole[1]) * f[1] + static_cast<float>(kPinkGain[1]) * w;
    f[2] = static_cast<float>(kPinkPole[2]) * f[2] + static_cast<float>(kPinkGain[2]) * w;
    return Clip((f[0] + f[1] + f[2] + static_cast<float>(kPinkDirect) * w) * pinkScale_);
  }
  if (color == kColorBrown) {
    f[0] = static_cast<float>(kBrownPole) * f[0] + brownGain_ * w;
    return Clip(f[0] * brownScale_);
  }
  return w;
}

// y0 = Y(index), y1 = Y(index + 1), the filters run from zero state over at most
// kWarmFrames samples before index (exact when index < kWarmFrames).
void NoiseKind::Rebuild(NoiseInst& in) const noexcept {
  const std::uint64_t from = in.index > kWarmFrames ? in.index - kWarmFrames : 0u;
  for (std::uint32_t c = 0; c < in.channels; ++c) {
    float* f = in.f[c];
    f[0] = f[1] = f[2] = 0.0f;
    if (in.color == kColorWhite) {
      in.y0[c] = White(in.key[c], in.index);
    } else {
      float y = 0.0f;
      for (std::uint64_t j = from; j <= in.index; ++j) y = Step(in.color, f, White(in.key[c], j));
      in.y0[c] = y;
    }
    in.y1[c] = Step(in.color, f, White(in.key[c], in.index + 1u));
  }
  in.stale = 0;
}

std::size_t NoiseKind::PoolBytes(const SourceLimits& limits) const noexcept {
  const std::uint32_t n = Cap(limits);
  return Align64(sizeof(NoiseInst) * n) + sizeof(std::uint32_t) * n;
}

bool NoiseKind::Init(void* pool, std::size_t bytes, const SourceLimits& limits, const SourceServices&) noexcept {
  const std::uint32_t n = Cap(limits);
  if (pool == nullptr || bytes < PoolBytes(limits)) return false;
  auto* base = static_cast<std::uint8_t*>(pool);
  cap_ = n;
  inst_ = reinterpret_cast<NoiseInst*>(base);
  freeInst_ = reinterpret_cast<std::uint32_t*>(base + Align64(sizeof(NoiseInst) * n));
  for (std::uint32_t i = 0; i < n; ++i) {
    new (&inst_[i]) NoiseInst();
    freeInst_[i] = n - 1u - i;
  }
  freeCount_ = n;
  // Variance of the pink filter output relative to its input: sum over the pairs of
  // terms (poles p, gains g, the direct term has p = 0) of g_i g_j / (1 - p_i p_j).
  double p[4] = {kPinkPole[0], kPinkPole[1], kPinkPole[2], 0.0};
  double g[4] = {kPinkGain[0], kPinkGain[1], kPinkGain[2], kPinkDirect};
  double v = 0.0;
  for (int i = 0; i < 4; ++i) {
    for (int j = 0; j < 4; ++j) v += g[i] * g[j] / (1.0 - p[i] * p[j]);
  }
  // D-199: RMS 0.2 for pink and brown (the white input has variance 1/3).
  const double toRms = kColoredRms * std::sqrt(3.0);
  pinkScale_ = static_cast<float>(toRms / std::sqrt(v));
  brownGain_ = static_cast<float>(std::sqrt(1.0 - kBrownPole * kBrownPole));
  brownScale_ = static_cast<float>(toRms);
  live_.store(0, std::memory_order_relaxed);
  return true;
}

void NoiseKind::Shutdown() noexcept {
  inst_ = nullptr;
  freeInst_ = nullptr;
  freeCount_ = 0;
  cap_ = 0;
  live_.store(0, std::memory_order_relaxed);
}

void NoiseKind::Start(const SourceStartRequest* requests, std::uint32_t count, SourceStartResult* results) noexcept {
  for (std::uint32_t k = 0; k < count; ++k) {
    const SourceStartRequest& rq = requests[k];
    SourceStartResult& res = results[k];
    res = SourceStartResult{kInvalidSlot, SND_END_DETAIL_NONE, 0u, 0u};
    const auto* p = static_cast<const NoiseParams*>(rq.params);
    if (p == nullptr || inst_ == nullptr || p->color > kColorBrown) continue;  // core ends it
    if (freeCount_ == 0u) {
      res.endDetail = SND_END_DETAIL_VOICE_LIMIT;
      continue;
    }
    const std::uint32_t i = freeInst_[--freeCount_];
    NoiseInst& in = inst_[i];
    in.color = p->color;
    in.channels = rq.channels == 0u ? 1u : (rq.channels > kMaxChannels ? kMaxChannels : rq.channels);
    const std::uint64_t key = VoiceKey(p->seed, rq.seed);
    for (std::uint32_t c = 0; c < kMaxChannels; ++c) in.key[c] = ChannelKey(key, c);
    in.index = rq.startFrame;
    in.fraction = 0;
    in.stale = 1;  // the first RenderBatch builds y0 / y1 (exact from index 0)
    std::atomic_ref<std::uint32_t>(in.state).store(1u, std::memory_order_release);
    live_.fetch_add(1u, std::memory_order_relaxed);
    res.sourceSlot = i;
  }
}

void NoiseKind::Release(const SlotIndex* sourceSlots, std::uint32_t count) noexcept {
  for (std::uint32_t k = 0; k < count; ++k) {
    const SlotIndex i = sourceSlots[k];
    if (i >= cap_ || inst_[i].state != 1u) continue;
    std::atomic_ref<std::uint32_t>(inst_[i].state).store(0u, std::memory_order_release);
    freeInst_[freeCount_++] = i;
    live_.fetch_sub(1u, std::memory_order_relaxed);
  }
}

void NoiseKind::Control(const SourceControlOp* ops, std::uint32_t count) noexcept {
  for (std::uint32_t k = 0; k < count; ++k) {
    const SourceControlOp& op = ops[k];
    if (op.sourceSlot >= cap_ || inst_[op.sourceSlot].state != 1u) continue;
    if (op.op == ControlOp::Seek) {
      NoiseInst& in = inst_[op.sourceSlot];
      in.index = op.frame;
      in.fraction = 0;
      in.stale = 1;
    }
  }
}

void NoiseKind::QueryReady(const SlotIndex*, std::uint32_t count, std::uint8_t* ready) const noexcept {
  for (std::uint32_t k = 0; k < count; ++k) ready[k] = 1u;
}

void NoiseKind::RenderBatch(const source::RenderBatch& batch, VoiceRenderStatus* status) noexcept {
  const double blockFrames = batch.block.frames == 0u ? 1.0 : static_cast<double>(batch.block.frames);
  for (std::uint32_t k = 0; k < batch.count; ++k) {
    const RenderItem& it = batch.items[k];
    VoiceRenderStatus& st = status[k];
    st = VoiceRenderStatus{};
    if (it.sourceSlot >= cap_ || LoadState(inst_[it.sourceSlot]) != 1u) {
      st.ended = 1;
      continue;
    }
    NoiseInst& in = inst_[it.sourceSlot];
    if (in.stale != 0u) Rebuild(in);
    const std::uint32_t n = it.framesRequested;
    const std::uint32_t off = it.frameOffset;
    const std::uint32_t ch = it.channels < in.channels ? it.channels : in.channels;
    const double p0 = it.pitch.start, pd = static_cast<double>(it.pitch.end) - it.pitch.start;
    const bool flat = it.pitch.start == 1.0f && it.pitch.end == 1.0f;
    // Channel by channel (each has its own sequence and filter); the source position
    // advances identically for every channel, so it is replayed from the start values.
    const std::uint64_t index0 = in.index;
    const std::uint32_t fraction0 = in.fraction;
    std::uint64_t index = index0;
    std::uint32_t fraction = fraction0;
    for (std::uint32_t c = 0; c < in.channels; ++c) {
      float* out = c < ch ? it.out[c] + off : nullptr;
      float* f = in.f[c];
      float y0 = in.y0[c], y1 = in.y1[c];
      const std::uint64_t key = in.key[c];
      index = index0;
      fraction = fraction0;
      for (std::uint32_t i = 0; i < n; ++i) {
        if (out != nullptr) {
          out[i] = fraction == 0u ? y0 : y0 + (y1 - y0) * (static_cast<float>(fraction) * (1.0f / 4294967296.0f));
        }
        const std::uint64_t inc =
            flat ? (std::uint64_t{1} << 32) : Increment(p0 + pd * (static_cast<double>(off + i) / blockFrames));
        const std::uint64_t pos = std::uint64_t{fraction} + inc;
        fraction = static_cast<std::uint32_t>(pos);
        for (std::uint64_t s = pos >> 32; s != 0u; --s) {
          ++index;
          y0 = y1;
          y1 = Step(in.color, f, White(key, index + 1u));
        }
      }
      in.y0[c] = y0;
      in.y1[c] = y1;
    }
    in.index = index;
    in.fraction = fraction;
    it.playhead->frame += n;
    st.framesRendered = n;
  }
}

void NoiseKind::AdvanceVirtual(const AdvanceItem* items, std::uint32_t count, VoiceRenderStatus* status) noexcept {
  for (std::uint32_t k = 0; k < count; ++k) {
    const AdvanceItem& a = items[k];
    status[k] = VoiceRenderStatus{};
    if (a.sourceSlot >= cap_ || inst_[a.sourceSlot].state != 1u) {
      status[k].ended = 1;
      continue;
    }
    NoiseInst& in = inst_[a.sourceSlot];
    const std::uint64_t total = std::uint64_t{in.fraction} + Increment(a.pitch) * a.outputFrames;
    in.index += total >> 32;
    in.fraction = static_cast<std::uint32_t>(total);
    in.stale = 1;  // O(1): the colour filters are rebuilt when the voice renders again
    a.playhead->frame += a.outputFrames;
  }
}

}  // namespace snd::source::detail
