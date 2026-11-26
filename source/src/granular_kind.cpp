#include "granular_kind.hpp"

#include <bit>

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
  grain.inverseLength = 1.0 / grain.length;
  grain.hannAge = UINT32_MAX;
  if (p.window == GrainWindow::Hann) grain.twiceCosStep = 2 * std::cos(kTau * grain.inverseLength);
  const double position =
      p.position + positionOffset + p.positionJitter * NoiseKind::White(v.key, serial * 2);
  grain.origin = double(v.begin) + std::clamp(position, 0.0, 1.0) * double(v.end - v.begin - 1);
  const double jitter = std::clamp(p.pitchJitterCents + jitterOffset, 0.0, 4800.0);
  const double cents = p.pitchCents + jitter * NoiseKind::White(v.key, serial * 2 + 1);
  grain.stride = std::exp2(std::clamp(cents, -4800.0, 4800.0) / 1200.0);
}

// Binary32 has 24 significant bits and its least positive value is 2^-149.
// Multiply exactly, then add the product into Q64.160 as seven base-2^32 words.
// Even UINT32_MAX frames need only 56 bits before the shift. Keeping sub-Q32
// bits internally lets a sequence of tiny ratios carry into the public playhead.
std::uint64_t VirtualFrames(GranularVoice& voice, float pitch, std::uint32_t frames) noexcept {
  if (!std::isfinite(pitch) || pitch <= 0 || !frames) return 0;
  const auto bits = std::bit_cast<std::uint32_t>(std::min(pitch, 32.f));
  const unsigned exponent = bits >> 23;
  const unsigned significand = (bits & 0x7fffffu) + (exponent ? 0x800000u : 0u);
  const std::uint64_t product = std::uint64_t{significand} * frames;
  // pitch = significand * 2^(max(1, exponent) - 150).
  const unsigned shift = 10 + std::max(1u, exponent);
  const unsigned word = shift / 32, offset = shift % 32;
  std::uint32_t add[7]{};
  const std::uint64_t lo = (product & UINT32_MAX) << offset;
  const std::uint64_t hi = (product >> 32) << offset;
  add[word] = static_cast<std::uint32_t>(lo);
  add[word + 1] = static_cast<std::uint32_t>(hi | (lo >> 32));
  add[word + 2] = static_cast<std::uint32_t>(hi >> 32);
  std::uint64_t carry = 0;
  for (unsigned w = 0; w < voice.fraction.size(); ++w) {
    const auto sum = std::uint64_t{voice.fraction[w]} + add[w] + carry;
    voice.fraction[w] = static_cast<std::uint32_t>(sum);
    carry = sum >> 32;
  }
  return ((std::uint64_t{add[6]} << 32) | add[5]) + carry;
}

// Advance the same Q32 accumulator as rendering. Only the newest eight births
// can survive, independent of how many samples or whole grain cycles elapsed.
void Skip(GranularVoice& v, std::uint64_t frames, const GrainOffsets& offsets = {}) noexcept {
  const auto firstFrame = v.frame;
  auto spawnAt = [&](std::uint64_t serial, std::uint64_t birth) {
    const double t = double(birth - firstFrame) / double(frames);
    Spawn(v, serial, birth, Modulation(offsets.lengthMs, t), Modulation(offsets.position, t),
          Modulation(offsets.pitchJitterCents, t));
  };
  if (offsets.density.start != offsets.density.end) {
    for (std::uint64_t f = 0; f < frames; ++f, ++v.frame) {
      const auto phase = v.phase + Increment(v, Modulation(offsets.density, double(f) / double(frames)));
      v.phase = static_cast<std::uint32_t>(phase);
      if (phase >= kCycle) spawnAt(v.serial++, v.frame);
    }
    return;
  }
  const auto inc = Increment(v, offsets.density.end);
  const auto tail = v.phase + (frames & UINT32_MAX) * inc;
  const auto births = (frames >> 32) * inc + (tail >> 32);
  v.phase = static_cast<std::uint32_t>(tail);
  v.serial += births;
  v.frame += frames;
  for (std::uint64_t j = 0; j < std::min<std::uint64_t>(births, kGrainSlots); ++j) {
    const auto age = 1 + (v.phase + j * kCycle) / inc;
    spawnAt(v.serial - 1 - j, v.frame - age);
  }
}

void Seek(GranularVoice& v, std::uint64_t frame) noexcept {
  for (auto& g : v.grains) g.length = 0;
  v.frame = v.serial = 0;
  v.phase = UINT32_MAX;
  v.fraction = {};
  Skip(v, frame);
}

constexpr unsigned kTile = 32;

void WindowWeights(Grain& g, GrainWindow window, unsigned first, unsigned count,
                   double* out) noexcept {
  if (window == GrainWindow::Rectangle) {
    std::fill_n(out, count, 1.0);
    return;
  }
  if (window == GrainWindow::Triangle) {
    for (unsigned f = 0; f < count; ++f)
      out[f] = 1.0 - std::abs(2 * ((first + f) * g.inverseLength) - 1);
    return;
  }
  // c(n+2) = 2*cos(step)*c(n+1) - c(n). Re-anchor every 32 samples to
  // bound roundoff. Seeking into a group replays at most 31 rotations from
  // that exact anchor, independently of the voice's history or block cuts.
  const double step = kTau * g.inverseLength;
  for (unsigned f = 0; f < count;) {
    const unsigned age = first + f;
    const unsigned at = age % kTile;
    if (at == 0 || g.hannAge != age) {
      const auto anchor = age - at;
      g.hannCos = std::cos(anchor * step);
      g.hannNextCos = std::cos((anchor + 1) * step);
      for (unsigned k = 0; k < at; ++k) {
        const double next = g.twiceCosStep * g.hannNextCos - g.hannCos;
        g.hannCos = g.hannNextCos;
        g.hannNextCos = next;
      }
    }
    const auto end = f + std::min(kTile - at, count - f);
    for (; f < end; ++f) {
      out[f] = 0.5 - 0.5 * g.hannCos;
      const double next = g.twiceCosStep * g.hannNextCos - g.hannCos;
      g.hannCos = g.hannNextCos;
      g.hannNextCos = next;
    }
    g.hannAge = first + f;
  }
}

// PCM32 stores IEEE binary32. The common in-range path includes signed zero
// and subnormals. Finite outliers clip to full scale and NaN/Inf become zero.
template <bool FloatPcm>
double Decode(const std::uint8_t* sample) noexcept {
  if constexpr (FloatPcm) {
    std::uint32_t bits;
    std::memcpy(&bits, sample, sizeof(bits));
    const auto magnitude = bits & 0x7fffffffu;
    if (magnitude <= 0x3f800000u) return std::bit_cast<float>(bits);
    if (magnitude >= 0x7f800000u) return 0;
    return (bits >> 31) ? -1.0 : 1.0;
  } else {
    return static_cast<std::int16_t>(sample[0] | (sample[1] << 8)) / 32768.0;
  }
}

struct Tile {
  double weights[kTile];
  double sums[kMaxChannels][kTile];
};

// Mix contiguous frames of one grain into this voice's tile. The two reads
// share their frame addresses, interpolation fraction, and window across all
// channels. Only distinct source channels are decoded. Extra output channels
// are copied after the complete mix.
template <bool FloatPcm, bool Stereo>
inline void Interpolate(const std::uint8_t* left, const std::uint8_t* right, double fraction,
                        double weight, unsigned channels, Tile& tile, unsigned f) noexcept {
  constexpr unsigned bytes = FloatPcm ? 4 : 2;
  for (unsigned c = 0; c < (Stereo ? 2u : channels); ++c) {
    const double l = Decode<FloatPcm>(left + c * bytes);
    const double r = Decode<FloatPcm>(right + c * bytes);
    tile.sums[c][f] += weight * (l + fraction * (r - l));
  }
}

template <bool FloatPcm, bool Stereo>
void MixGrain(const GranularVoice& v, Grain& g, unsigned count, unsigned channels,
              Tile& tile) noexcept {
  const auto firstAge = v.frame - g.birth;
  if (firstAge >= g.length) return;
  const auto first = static_cast<unsigned>(firstAge);
  count = std::min(count, g.length - first);
  double window[kTile];
  WindowWeights(g, v.params.window, first, count, window);
  for (unsigned f = 0; f < count; ++f) tile.weights[f] += window[f];
  const unsigned stride = (Stereo ? 2u : v.asset.codec.channels) * (FloatPcm ? 4 : 2);
  // A forward-moving grain whose final interpolation pair fits needs no
  // per-sample wrap, range test, or second address calculation.
  if (g.origin + (first + count - 1) * g.stride < double(v.end - 1)) {
    for (unsigned f = 0; f < count; ++f) {
      const double pos = g.origin + (first + f) * g.stride;
      const auto a = static_cast<std::uint32_t>(pos);
      const auto* left = v.asset.data + std::size_t{a} * stride;
      Interpolate<FloatPcm, Stereo>(left, left + stride, pos - a, window[f], channels, tile, f);
    }
    return;
  }
  for (unsigned f = 0; f < count; ++f) {
    double pos = g.origin + (first + f) * g.stride;
    if (pos >= double(v.end)) {
      if (!v.loop) continue;
      pos = double(v.begin) + std::fmod(pos - double(v.begin), double(v.end - v.begin));
    }
    const auto a = static_cast<std::uint32_t>(pos);
    const auto b = a + 1 < v.end ? a + 1 : static_cast<std::uint32_t>(v.loop ? v.begin : a);
    Interpolate<FloatPcm, Stereo>(v.asset.data + std::size_t{a} * stride,
                                  v.asset.data + std::size_t{b} * stride, pos - a, window[f],
                                  channels, tile, f);
  }
}

template <bool FloatPcm, bool Stereo>
void RenderTile(GranularVoice& v, const RenderItem& item, unsigned offset,
                unsigned count) noexcept {
  Tile tile;
  const unsigned outChannels = std::min(item.channels, kMaxChannels);
  const unsigned channels = Stereo ? 2u : std::min(outChannels, v.asset.codec.channels);
  std::fill_n(tile.weights, count, 0.0);
  for (unsigned c = 0; c < channels; ++c) std::fill_n(tile.sums[c], count, 0.0);
  // Slot order is the per-frame summation order, independent of block boundaries.
  for (auto& g : v.grains) MixGrain<FloatPcm, Stereo>(v, g, count, channels, tile);
  for (unsigned c = 0; c < channels; ++c) {
    for (unsigned f = 0; f < count; ++f) {
      item.out[c][offset + f] =
          static_cast<float>(tile.sums[c][f] / std::max(1.0, tile.weights[f]));
    }
  }
  for (unsigned c = channels; c < outChannels; ++c)
    std::copy_n(item.out[channels - 1] + offset, count, item.out[c] + offset);
}

void RenderVoice(GranularVoice& v, const RenderItem& item, unsigned first = 0) noexcept {
  const unsigned n = item.framesRequested;
  const bool constant = item.grainDensityOffset.start == item.grainDensityOffset.end;
  const auto fixed = Increment(v, Modulation(item.grainDensityOffset, 0));
  const bool stereo = item.channels == 2 && v.asset.codec.channels == 2;
  const bool floating = v.asset.codec.codec == SND_CODEC_PCM_F32;
  for (unsigned f = first; f < n;) {
    const double t = double(f) / n;
    const auto inc = constant ? fixed : Increment(v, Modulation(item.grainDensityOffset, t));
    const auto phase = v.phase + inc;
    v.phase = static_cast<std::uint32_t>(phase);
    if (phase >= kCycle) {
      Spawn(v, v.serial++, v.frame, Modulation(item.grainLengthMsOffset, t),
            Modulation(item.grainPositionOffset, t),
            Modulation(item.grainPitchJitterCentsOffset, t));
    }
    unsigned count = std::min(kTile, n - f);
    // Accumulate until the next birth so every grain remains fixed throughout the tile.
    if (constant) {
      if (inc)
        count =
            static_cast<unsigned>(std::min<std::uint64_t>(count, 1 + (UINT32_MAX - v.phase) / inc));
      v.phase += static_cast<std::uint32_t>((count - 1) * inc);
    } else {
      unsigned step = 1;
      for (; step < count; ++step) {
        const auto next =
            v.phase + Increment(v, Modulation(item.grainDensityOffset, double(f + step) / n));
        if (next >= kCycle) break;
        v.phase = static_cast<std::uint32_t>(next);
      }
      count = step;
    }
    if (floating) {
      if (stereo)
        RenderTile<true, true>(v, item, f, count);
      else
        RenderTile<true, false>(v, item, f, count);
    } else {
      if (stereo)
        RenderTile<false, true>(v, item, f, count);
      else
        RenderTile<false, false>(v, item, f, count);
    }
    v.frame += count;
    f += count;
  }
}
// Write or read the trailing PCM across at most one wrap. Memory belongs to the
// preallocated voice slot; re-promotion can use another real slot or quality.
template<bool Save>
void History(float* history, unsigned capacity, unsigned channels, const RenderItem& it,
             std::uint64_t frame, unsigned offset, unsigned n) noexcept {
  const unsigned pos = static_cast<unsigned>(frame % capacity);
  const auto first = std::min(n, capacity - pos);
  for (unsigned c = 0; c < std::min(it.channels, channels); ++c) {
    auto* pcm = it.out[c] + offset;
    auto* ring = history + std::size_t{c} * capacity;
    if constexpr (Save) {
      std::copy_n(pcm, first, ring + pos);
      std::copy_n(pcm + first, n - first, ring);
    } else {
      std::copy_n(ring + pos, first, pcm);
      std::copy_n(ring, n - first, pcm + first);
    }
  }
}
}  // namespace

std::size_t GranularKind::PoolBytes(const SourceLimits& limits) const noexcept {
  Carver measure{nullptr, ResidentSynth::PoolBytes(limits), 0};
  measure.Take<float>(std::size_t{limits.maxVoices} * limits.grainHistoryChannels * limits.grainHistoryFrames);
  return measure.used;
}

bool GranularKind::Init(void* pool, std::size_t bytes, const SourceLimits& limits, const SourceServices& services) noexcept {
  if (!ResidentSynth::Init(pool, bytes, limits, services)) return false;
  Carver tail{static_cast<std::uint8_t*>(pool), ResidentSynth::PoolBytes(limits), bytes};
  historyFrames_ = limits.grainHistoryFrames;
  historyChannels_ = limits.grainHistoryChannels;
  history_ = tail.Take<float>(std::size_t{limits.maxVoices} * historyFrames_ * historyChannels_);
  return tail.ok;
}

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
    if (item.grainResampled) {
      float* history = history_ + std::size_t{item.sourceSlot} * historyFrames_ * historyChannels_;
      const auto start = item.playhead->frame;
      if (start > v->frame) Skip(*v, start - v->frame);
      const auto replay = static_cast<unsigned>(std::min<std::uint64_t>(v->frame - start, item.framesRequested));
      // Older history can be unavailable after starvation. Leave that prefix
      // zero (RenderItem buffers are cleared by core), then resume on the clock.
      const auto oldest = v->frame > historyFrames_ ? v->frame - historyFrames_ : 0;
      const auto silence = static_cast<unsigned>(std::min<std::uint64_t>(replay, oldest > start ? oldest - start : 0));
      if (replay > silence) History<false>(history, historyFrames_, historyChannels_, item, start + silence, silence, replay - silence);
      RenderVoice(*v, item, replay);
      const auto end = start + item.framesRequested;
      const auto save = std::min(item.framesRequested - replay, historyFrames_);
      if (save) History<true>(history, historyFrames_, historyChannels_, item, end - save, item.framesRequested - save, save);
      item.playhead->frame = end;
      item.playhead->fraction = 0;
    } else {
      RenderVoice(*v, item);
      item.playhead->frame = v->frame;
      item.playhead->fraction = v->fraction.back();
    }
    st.framesRendered = item.framesRequested;
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
    if (a.grain) {
      const auto end = a.playhead->frame;
      if (end > v->frame) Skip(*v, end - v->frame, *a.grain);
    } else {
      Skip(*v, VirtualFrames(*v, a.pitch, a.outputFrames));
      a.playhead->frame = v->frame;
      a.playhead->fraction = v->fraction.back();
    }
  }
}
}  // namespace snd::source::detail
