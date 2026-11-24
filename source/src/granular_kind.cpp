#include "granular_kind.hpp"

#include "noise_kind.hpp"

namespace snd::source::detail {
namespace {
constexpr std::uint64_t kCycle = std::uint64_t{1} << 32;
constexpr double kTau = 6.2831853071795864769;

double Finite(double x) noexcept {
  return std::isfinite(x) ? x : 0.0;
}

std::uint64_t Increment(const GranularVoice& v, double offset) noexcept {
  const double rate = v.asset.codec.sample_rate;
  const double density = std::clamp(v.params.density + offset, 0.0, rate);
  return static_cast<std::uint64_t>(std::round(density * kCycle / rate));
}

void Spawn(GranularVoice& v, std::uint64_t serial, std::uint64_t birth, double lengthOffset,
           double positionOffset, double jitterOffset) noexcept {
  auto& grain = v.grains[serial % kGrainSlots];
  const auto& p = v.params;
  const double ms = std::clamp(p.lengthMs + lengthOffset, 0.0, 1000.0);
  grain.length = static_cast<std::uint32_t>(
      std::max(1.0, std::round(ms * v.asset.codec.sample_rate / 1000.0)));
  grain.birth = birth;
  const double position =
      p.position + positionOffset + p.positionJitter * NoiseKind::White(v.key, serial * 2);
  grain.origin = double(v.begin) + std::clamp(position, 0.0, 1.0) * double(v.end - v.begin - 1);
  const double jitter = std::clamp(p.pitchJitterCents + jitterOffset, 0.0, 4800.0);
  const double cents = p.pitchCents + jitter * NoiseKind::White(v.key, serial * 2 + 1);
  grain.stride = std::exp2(std::clamp(cents, -4800.0, 4800.0) / 1200.0);
}

// Advance the same Q32 accumulator as rendering. Only the newest eight births
// can survive, independent of how many samples or whole grain cycles elapsed.
void Skip(GranularVoice& v, std::uint64_t frames) noexcept {
  const auto inc = Increment(v, 0);
  const auto tail = v.phase + (frames & UINT32_MAX) * inc;
  const auto births = (frames >> 32) * inc + (tail >> 32);
  v.phase = static_cast<std::uint32_t>(tail);
  v.serial += births;
  v.frame += frames;
  for (std::uint64_t j = 0; j < std::min<std::uint64_t>(births, kGrainSlots); ++j) {
    const auto age = 1 + (v.phase + j * kCycle) / inc;
    Spawn(v, v.serial - 1 - j, v.frame - age, 0, 0, 0);
  }
}

void Seek(GranularVoice& v, std::uint64_t frame) noexcept {
  for (auto& g : v.grains) g.length = 0;
  v.frame = v.serial = 0;
  v.phase = UINT32_MAX;
  v.fraction = 0;
  Skip(v, frame);
}

float Sample(const GranularVoice& v, std::uint32_t frame, std::uint32_t channel) noexcept {
  const auto& codec = v.asset.codec;
  const std::size_t index =
      std::size_t{frame} * codec.channels + std::min(channel, codec.channels - 1);
  if (codec.codec == SND_CODEC_PCM_S16) {
    const auto* p = v.asset.data + 2 * index;
    return static_cast<std::int16_t>(p[0] | (p[1] << 8)) / 32768.f;
  }
  float value;
  std::memcpy(&value, v.asset.data + 4 * index, 4);
  return std::isfinite(value) ? std::clamp(value, -1.f, 1.f) : 0.f;
}

double Window(GrainWindow window, double t) noexcept {
  if (window == GrainWindow::Triangle) return 1.0 - std::abs(2 * t - 1);
  if (window == GrainWindow::Rectangle) return 1;
  return 0.5 - 0.5 * std::cos(kTau * t);
}

void RenderFrame(const GranularVoice& v, const RenderItem& item, std::uint32_t f) noexcept {
  double sum[kMaxChannels]{};
  double weights = 0;
  const auto channels = std::min(item.channels, kMaxChannels);
  for (const auto& g : v.grains) {
    const auto age = v.frame - g.birth;
    if (age >= g.length) continue;
    const double w = Window(v.params.window, double(age) / g.length);
    weights += w;
    double x = g.origin + double(age) * g.stride;
    if (x >= double(v.end)) {
      if (!v.loop) continue;
      x = double(v.begin) + std::fmod(x - double(v.begin), double(v.end - v.begin));
    }
    const auto a = static_cast<std::uint32_t>(x);
    const auto b = a + 1 < v.end ? a + 1 : static_cast<std::uint32_t>(v.loop ? v.begin : a);
    const double fraction = x - a;
    for (unsigned c = 0; c < channels; ++c) {
      const double left = Sample(v, a, c);
      sum[c] += w * (left + fraction * (Sample(v, b, c) - left));
    }
  }
  for (unsigned c = 0; c < channels; ++c)
    item.out[c][f] = static_cast<float>(sum[c] / std::max(1.0, weights));
}
}  // namespace

bool GranularKind::Prepare(GranularVoice& v, const SourceStartRequest& rq,
                           SourceStartResult& result) noexcept {
  const auto& p = *static_cast<const GranularParams*>(rq.params);
  if (!p.asset || p.window > GrainWindow::Rectangle) return false;
  const auto& asset = *p.asset;
  const auto& h = asset.codec;
  if (asset.form != LoadForm::PcmResident || !asset.data || !h.total_frames ||
      h.total_frames > UINT32_MAX || !h.channels || h.channels > kMaxChannels || !h.sample_rate ||
      h.sample_rate > 384000 || (h.codec != SND_CODEC_PCM_F32 && h.codec != SND_CODEC_PCM_S16))
    return false;
  const auto bytes = h.total_frames * h.channels * (h.codec == SND_CODEC_PCM_F32 ? 4u : 2u);
  if (bytes > asset.dataBytes || bytes > SIZE_MAX) return false;
  v.asset = asset;
  v.params = p;
  v.params.asset = nullptr;
  v.params.density = static_cast<float>(Finite(p.density));
  v.params.lengthMs = static_cast<float>(Finite(p.lengthMs));
  v.params.position = static_cast<float>(Finite(p.position));
  v.params.positionJitter = static_cast<float>(std::clamp(Finite(p.positionJitter), 0.0, 1.0));
  v.params.pitchJitterCents = static_cast<float>(Finite(p.pitchJitterCents));
  v.params.pitchCents = static_cast<float>(Finite(p.pitchCents));
  v.key = NoiseKind::VoiceKey(0, rq.seed);
  v.loop = (rq.flags & kStartLoop) != 0;
  v.end = h.total_frames;
  if (v.loop && asset.loopStart < asset.loopEnd && asset.loopEnd <= v.end) {
    v.begin = asset.loopStart;
    v.end = asset.loopEnd;
  }
  Seek(v, rq.startFrame);
  result.reserved = h.sample_rate == rate_ ? 0u : h.sample_rate;
  return true;
}

void GranularKind::Control(const SourceControlOp* ops, std::uint32_t count) noexcept {
  for (unsigned i = 0; i < count; ++i) {
    const auto& op = ops[i];
    auto* v = Get(op.sourceSlot);
    if (!v) continue;
    if (op.op == ControlOp::Seek) Seek(*v, op.frame);
    if (op.op == ControlOp::SetLoop) {
      v->loop = op.frame < op.frame2 && op.frame2 <= v->asset.codec.total_frames;
      v->begin = v->loop ? op.frame : 0;
      v->end = v->loop ? op.frame2 : v->asset.codec.total_frames;
      for (auto& g : v->grains) g.length = 0;
    }
  }
}

void GranularKind::RenderBatch(const source::RenderBatch& batch,
                               VoiceRenderStatus* status) noexcept {
  for (unsigned k = 0; k < batch.count; ++k) {
    const auto& item = batch.items[k];
    auto& st = status[k];
    st = {};
    auto* v = Get(item.sourceSlot);
    if (!v) {
      st.ended = 1;
      continue;
    }
    const auto n = item.framesRequested;
    const bool constant = item.grainDensityOffset.start == item.grainDensityOffset.end;
    const auto fixed = Increment(*v, Modulation(item.grainDensityOffset, 0));
    for (unsigned f = 0; f < n; ++f) {
      const double t = double(f) / n;
      const auto inc = constant ? fixed : Increment(*v, Modulation(item.grainDensityOffset, t));
      const auto phase = v->phase + inc;
      v->phase = static_cast<std::uint32_t>(phase);
      if (phase >= kCycle) {
        Spawn(*v, v->serial++, v->frame, Modulation(item.grainLengthMsOffset, t),
              Modulation(item.grainPositionOffset, t),
              Modulation(item.grainPitchJitterCentsOffset, t));
      }
      RenderFrame(*v, item, f);
      ++v->frame;
    }
    item.playhead->frame = v->frame;
    st.framesRendered = n;
  }
}

void GranularKind::AdvanceVirtual(const AdvanceItem* items, std::uint32_t count,
                                  VoiceRenderStatus* status) noexcept {
  for (unsigned i = 0; i < count; ++i) {
    const auto& a = items[i];
    status[i] = {};
    auto* v = Get(a.sourceSlot);
    if (!v) {
      status[i].ended = 1;
      continue;
    }
    const double ratio = std::clamp(Finite(a.pitch), 0.0, 32.0);
    const double elapsed = v->fraction + double(a.outputFrames) * ratio;
    const auto n = static_cast<std::uint64_t>(elapsed);
    v->fraction = elapsed - double(n);
    Skip(*v, n);
    a.playhead->frame = v->frame;
    a.playhead->fraction = static_cast<std::uint32_t>(v->fraction * kCycle);
  }
}
}  // namespace snd::source::detail
