// Oscillator kind (oscillator_kind.hpp).
#include "oscillator_kind.hpp"
#include "waveform.hpp"

#include <cmath>
#include <cstring>
#include <new>

#include "snd/format/bank_format.hpp"

namespace snd::source::detail {

namespace {

namespace bf = snd::bank_format;

constexpr std::size_t Align64(std::size_t n) noexcept { return (n + 63u) & ~std::size_t{63u}; }
std::uint32_t Cap(const SourceLimits& l) noexcept { return l.maxVoices == 0u ? 1u : l.maxVoices; }
constexpr double kMaxDt = 0.4999;  // just below Nyquist
constexpr std::uint32_t kDefaultTableLength = 2048;

std::uint32_t LoadState(const OscInst& in) noexcept {
  return std::atomic_ref<std::uint32_t>(const_cast<std::uint32_t&>(in.state)).load(std::memory_order_acquire);
}

double Wrap(double p) noexcept { return p - std::floor(p); }

inline float PcmAt(const OscInst& in, std::uint32_t frame) noexcept {
  const std::size_t i = std::size_t{frame} * in.tableChannels;
  if (in.tableCodec == SND_CODEC_PCM_F32) {
    float v;
    std::memcpy(&v, in.table + i * 4u, 4);
    return v - v == 0.0f ? v : 0.0f;
  }
  const std::uint8_t* s = in.table + i * 2u;
  const std::int16_t v = static_cast<std::int16_t>(static_cast<std::uint16_t>(s[0] | (s[1] << 8)));
  return static_cast<float>(v) * (1.0f / 32768.0f);
}

// Linear read of table `k` at phase t.
inline double TableAt(const OscInst& in, std::uint32_t k, double t) noexcept {
  const double x = t * static_cast<double>(in.tableLength);
  std::uint32_t j = static_cast<std::uint32_t>(x);
  if (j >= in.tableLength) j = in.tableLength - 1u;
  const double fr = x - static_cast<double>(j);
  const std::uint32_t j1 = j + 1u == in.tableLength ? 0u : j + 1u;
  const std::uint32_t base = in.tableOrigin + k * in.tableLength;
  const double a = PcmAt(in, base + j);
  const double b = PcmAt(in, base + j1);
  return a + (b - a) * fr;
}

inline double Wavetable(const OscInst& in, double t, double pos) noexcept {
  if (in.tableCount <= 1u) return TableAt(in, 0u, t);
  pos = pos < 0.0 ? 0.0 : (pos > 1.0 ? 1.0 : pos);
  const double x = pos * static_cast<double>(in.tableCount - 1u);
  std::uint32_t k = static_cast<std::uint32_t>(x);
  if (k >= in.tableCount - 1u) k = in.tableCount - 2u;
  const double fr = x - static_cast<double>(k);
  const double a = TableAt(in, k, t);
  if (fr <= 0.0) return a;
  return a + (TableAt(in, k + 1u, t) - a) * fr;
}

Ramp Finite(const Ramp& r) noexcept {
  return Ramp{std::isfinite(r.start) && std::isfinite(r.end) ? r.start : 0.0f,
              std::isfinite(r.start) && std::isfinite(r.end) ? r.end : 0.0f};
}

double StartPhaseOf(std::uint32_t shape) noexcept {
  switch (shape) {
    case static_cast<std::uint32_t>(bf::Waveform::Saw): return 0.5;
    case static_cast<std::uint32_t>(bf::Waveform::Triangle): return 0.25;
    default: return 0.0;
  }
}


// Per-sample loop shared by the waveforms (D-198, D-200): dt from the pitch ramp, the
// phase waits while dt <= 0 and the gain fades linearly (fadePos / fadeFrames) to 0 and
// back. wave(phase, dt, a) is the band-limited value at `phase` (a = block position).
struct OscLoop {
  double blockFrames, off, p0, pd, baseDt;
  std::uint32_t fadeFrames;
  template <class Wave>
  void Run(OscInst& in, float* out, std::uint32_t n, Wave wave) const noexcept {
    double phase = in.phase;
    double held = in.heldDt;
    std::uint32_t g = in.fadePos;
    if (!in.gainStarted && n != 0) {
      const double firstRatio = p0 + pd * (off / blockFrames);
      g = baseDt * firstRatio > 0 ? fadeFrames : 0;
      in.gainStarted = true;
    }
    const double inv = 1.0 / static_cast<double>(fadeFrames);
    for (std::uint32_t i = 0; i < n; ++i) {
      const double a = (off + static_cast<double>(i)) / blockFrames;
      double dt = baseDt * (p0 + pd * a);
      if (!(dt > 0.0)) {  // no frequency: the phase waits, the output fades out
        if (g == 0u) {
          out[i] = 0.0f;
          continue;
        }
        --g;
        out[i] = static_cast<float>(wave(phase, held, a) * (static_cast<double>(g) * inv));
        continue;
      }
      dt = dt > kMaxDt ? kMaxDt : dt;
      double y = wave(phase, dt, a);
      if (g < fadeFrames) {
        ++g;
        y *= static_cast<double>(g) * inv;
      }
      out[i] = static_cast<float>(y);
      held = dt;
      phase += dt;
      if (phase >= 1.0) phase -= 1.0;
    }
    in.phase = phase;
    in.heldDt = held;
    in.fadePos = g;
  }
};

}  // namespace

std::size_t OscillatorKind::PoolBytes(const SourceLimits& limits) const noexcept {
  const std::uint32_t n = Cap(limits);
  return Align64(sizeof(OscInst) * n) + sizeof(std::uint32_t) * n;
}

bool OscillatorKind::Init(void* pool, std::size_t bytes, const SourceLimits& limits, const SourceServices&) noexcept {
  const std::uint32_t n = Cap(limits);
  if (pool == nullptr || bytes < PoolBytes(limits)) return false;
  auto* base = static_cast<std::uint8_t*>(pool);
  cap_ = n;
  sampleRate_ = limits.sampleRate == 0u ? 48000.0 : static_cast<double>(limits.sampleRate);
  fadeFrames_ = static_cast<std::uint32_t>(sampleRate_ * 0.005 + 0.5);  // D-200: 5 ms
  if (fadeFrames_ == 0u) fadeFrames_ = 1u;
  inst_ = reinterpret_cast<OscInst*>(base);
  freeInst_ = reinterpret_cast<std::uint32_t*>(base + Align64(sizeof(OscInst) * n));
  for (std::uint32_t i = 0; i < n; ++i) {
    new (&inst_[i]) OscInst();
    freeInst_[i] = n - 1u - i;
  }
  freeCount_ = n;
  live_.store(0, std::memory_order_relaxed);
  return true;
}

void OscillatorKind::Shutdown() noexcept {
  inst_ = nullptr;
  freeInst_ = nullptr;
  freeCount_ = 0;
  cap_ = 0;
  live_.store(0, std::memory_order_relaxed);
}

void OscillatorKind::Start(const SourceStartRequest* requests, std::uint32_t count, SourceStartResult* results) noexcept {
  for (std::uint32_t k = 0; k < count; ++k) {
    const SourceStartRequest& rq = requests[k];
    SourceStartResult& res = results[k];
    res = SourceStartResult{kInvalidSlot, SND_END_DETAIL_NONE, 0u, 0u};
    const auto* p = static_cast<const OscillatorParams*>(rq.params);
    if (p == nullptr || inst_ == nullptr) continue;  // core ends it
    std::uint32_t shape = p->waveform;
    const SourceAsset* wt = p->wavetable;
    std::uint32_t tableLength = 0, tableCount = 0;
    std::uint64_t origin = 0;
    if (wt != nullptr) {
      // D-192: resident PCM only.
      const std::uint64_t frames = wt->codec.total_frames;
      if (wt->form != LoadForm::PcmResident || wt->data == nullptr || wt->codec.channels == 0u ||
          (wt->codec.codec != SND_CODEC_PCM_F32 && wt->codec.codec != SND_CODEC_PCM_S16) || frames < 2u ||
          frames > 0xFFFFFFFFu || wt->dataBytes < frames * wt->codec.channels * (wt->codec.codec == SND_CODEC_PCM_F32 ? 4u : 2u)) {
        continue;
      }
      std::uint64_t len = 0;
      if (wt->loopEnd > wt->loopStart && wt->loopEnd <= frames) {
        len = wt->loopEnd - wt->loopStart;
        origin = wt->loopStart;  // D-198: table k starts at loopStart + k x len
      } else if (frames % kDefaultTableLength == 0u) {
        len = kDefaultTableLength;
      } else {
        len = frames;
      }
      if (len < 2u) continue;
      tableLength = static_cast<std::uint32_t>(len);
      tableCount = static_cast<std::uint32_t>((frames - origin) / len);
      shape = kShapeWavetable;
    } else if (shape > static_cast<std::uint32_t>(bf::Waveform::Pulse)) {
      continue;  // unknown waveform
    }
    if (freeCount_ == 0u) {
      res.endDetail = SND_END_DETAIL_VOICE_LIMIT;
      continue;
    }
    const std::uint32_t i = freeInst_[--freeCount_];
    OscInst& in = inst_[i];
    in.shape = shape;
    const double f = p->frequencyHz > 0.0f && std::isfinite(p->frequencyHz) ? static_cast<double>(p->frequencyHz) : 0.0;
    in.baseDt = f / sampleRate_;
    in.gainStarted = false;
    in.fadePos = 0;
    in.heldDt = in.baseDt > kMaxDt ? kMaxDt : in.baseDt;
    in.startPhase = StartPhaseOf(shape);
    in.phase = Wrap(in.startPhase + static_cast<double>(rq.startFrame) * in.baseDt);
    in.duty = std::isfinite(p->duty) ? p->duty : 0.5f;
    in.tablePosition = std::isfinite(p->tablePosition) ? p->tablePosition : 0.0f;
    in.table = wt != nullptr ? wt->data : nullptr;
    in.tableCodec = wt != nullptr ? wt->codec.codec : 0u;
    in.tableChannels = wt != nullptr ? wt->codec.channels : 0u;
    in.tableLength = tableLength;
    in.tableCount = tableCount;
    in.tableOrigin = static_cast<std::uint32_t>(origin);
    std::atomic_ref<std::uint32_t>(in.state).store(1u, std::memory_order_release);
    live_.fetch_add(1u, std::memory_order_relaxed);
    res.sourceSlot = i;
  }
}

void OscillatorKind::Release(const SlotIndex* sourceSlots, std::uint32_t count) noexcept {
  for (std::uint32_t k = 0; k < count; ++k) {
    const SlotIndex i = sourceSlots[k];
    if (i >= cap_ || inst_[i].state != 1u) continue;
    std::atomic_ref<std::uint32_t>(inst_[i].state).store(0u, std::memory_order_release);
    freeInst_[freeCount_++] = i;
    live_.fetch_sub(1u, std::memory_order_relaxed);
  }
}

void OscillatorKind::Control(const SourceControlOp* ops, std::uint32_t count) noexcept {
  for (std::uint32_t k = 0; k < count; ++k) {
    const SourceControlOp& op = ops[k];
    if (op.sourceSlot >= cap_ || inst_[op.sourceSlot].state != 1u) continue;
    OscInst& in = inst_[op.sourceSlot];
    if (op.op == ControlOp::Seek) in.phase = Wrap(in.startPhase + static_cast<double>(op.frame) * in.baseDt);
  }
}

void OscillatorKind::QueryReady(const SlotIndex*, std::uint32_t count, std::uint8_t* ready) const noexcept {
  for (std::uint32_t k = 0; k < count; ++k) ready[k] = 1u;
}

void OscillatorKind::RenderBatch(const source::RenderBatch& batch, VoiceRenderStatus* status) noexcept {
  const double blockFrames = batch.block.frames == 0u ? 1.0 : static_cast<double>(batch.block.frames);
  for (std::uint32_t k = 0; k < batch.count; ++k) {
    const RenderItem& it = batch.items[k];
    VoiceRenderStatus& st = status[k];
    st = VoiceRenderStatus{};
    if (it.sourceSlot >= cap_ || LoadState(inst_[it.sourceSlot]) != 1u) {
      st.ended = 1;
      continue;
    }
    OscInst& in = inst_[it.sourceSlot];
    const std::uint32_t n = it.framesRequested;
    const std::uint32_t off = it.frameOffset;
    const std::uint32_t ch = it.channels < kMaxChannels ? it.channels : kMaxChannels;
    if (ch == 0u || n == 0u) {
      st.framesRendered = n;
      continue;
    }
    float* out = it.out[0] + off;
    // Per-sample linear ramps over the whole block (frame f of the block at f / frames).
    // D-198: inputs that are not finite are made safe once per block (ramp ends): a pitch
    // -> 0 (silence), an offset -> 0.
    const bool pitchOk = std::isfinite(it.pitch.start) && std::isfinite(it.pitch.end);
    const double p0 = pitchOk ? it.pitch.start : 0.0;
    const double pd = pitchOk ? static_cast<double>(it.pitch.end) - it.pitch.start : 0.0;
    const Ramp duty = Finite(it.dutyOffset);
    const Ramp table = Finite(it.tablePositionOffset);
    const double d0 = static_cast<double>(in.duty) + duty.start;
    const double dd = static_cast<double>(duty.end) - duty.start;
    const double t0 = static_cast<double>(in.tablePosition) + table.start;
    const double td = static_cast<double>(table.end) - table.start;
    const double baseDt = in.baseDt;
    const OscLoop L{blockFrames, static_cast<double>(off), p0, pd, baseDt, fadeFrames_};
    if (in.shape == kShapeWavetable) {
      L.Run(in, out, n, [&in, t0, td](double ph, double, double a) { return Wavetable(in, ph, t0 + td * a); });
    } else {
      L.Run(in, out, n, [&](double ph, double dt, double a) { return Waveform(in.shape, ph, dt, d0 + dd * a); });
    }
    for (std::uint32_t c = 1; c < ch; ++c) std::memcpy(it.out[c] + off, out, sizeof(float) * n);
    it.playhead->frame += n;
    st.framesRendered = n;
  }
}

void OscillatorKind::AdvanceVirtual(const AdvanceItem* items, std::uint32_t count, VoiceRenderStatus* status) noexcept {
  for (std::uint32_t k = 0; k < count; ++k) {
    const AdvanceItem& a = items[k];
    status[k] = VoiceRenderStatus{};
    if (a.sourceSlot >= cap_ || inst_[a.sourceSlot].state != 1u) {
      status[k].ended = 1;
      continue;
    }
    OscInst& in = inst_[a.sourceSlot];
    double dt = in.baseDt * static_cast<double>(a.pitch > 0.0f && std::isfinite(a.pitch) ? a.pitch : 0.0f);
    dt = dt > kMaxDt ? kMaxDt : dt;  // 0 (and NaN) -> the phase waits (D-198)
    if (!a.outputFrames) continue;
    if (!in.gainStarted) {
      in.fadePos = dt > 0 ? fadeFrames_ : 0;
      in.gainStarted = true;
    }
    in.phase = Wrap(in.phase + dt * static_cast<double>(a.outputFrames));
    // D-200: the gain moves as it would have while rendering.
    if (dt > 0.0) {
      in.heldDt = dt;
      in.fadePos = fadeFrames_ - in.fadePos <= a.outputFrames ? fadeFrames_ : in.fadePos + a.outputFrames;
    } else {
      in.fadePos = in.fadePos <= a.outputFrames ? 0u : in.fadePos - a.outputFrames;
    }
    a.playhead->frame += a.outputFrames;
  }
}

}  // namespace snd::source::detail
