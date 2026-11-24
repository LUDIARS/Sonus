#include "patch_kind.hpp"

#include "noise_kind.hpp"
#include "waveform.hpp"

namespace snd::source::detail {
namespace {
constexpr double kTau = 6.2831853071795864769, kScale = 4294967296.0;
constexpr unsigned kModStep = 32;

std::uint64_t Plus(std::uint64_t t, std::uint64_t dt) noexcept {
  return dt > UINT64_MAX - t ? UINT64_MAX : t + dt;
}

std::uint32_t Increment(const PatchState& s, double pitch) noexcept {
  return static_cast<std::uint32_t>(std::round(std::clamp(s.baseDt * pitch, 0.0, 0.4999) * kScale));
}

std::uint64_t Duration(float seconds, unsigned rate) noexcept {
  return static_cast<std::uint64_t>(std::round(double(seconds) * rate));
}

double KeyboardLevel(const PatchNode& n, unsigned rate, std::uint64_t t) noexcept {
  const auto attack = Duration(n.arg[0], rate), decay = Duration(n.arg[1], rate);
  if (t < attack) return double(t) / double(attack);
  if (t - attack < decay) return 1 + (double(n.arg[2]) - 1) * double(t - attack) / double(decay);
  return n.arg[2];
}

float ModValue(const PatchVoice& v, unsigned i, unsigned rate, std::uint64_t t) noexcept {
  const auto& n = v.nodes[i];
  const auto& s = v.state[i];
  if (n.op == PatchOp::Adsr) {
    if (!v.gate || t < v.gate) return static_cast<float>(KeyboardLevel(n, rate, t));
    const auto release = Duration(n.arg[3], rate);
    const auto elapsed = t - v.gate;
    if (elapsed >= release) return 0;
    return static_cast<float>(KeyboardLevel(n, rate, v.gate) *
                              (1 - double(elapsed) / double(release)));
  }
  // Q32 cycles at sample t, modulo 2^32 whole cycles. Unsigned products wrap exactly.
  const auto cycles = t * static_cast<std::uint64_t>(std::round(double(n.arg[0]) * kScale / rate));
  if (n.op == PatchOp::RandomHold) return NoiseKind::White(s.key, cycles >> 32);
  return static_cast<float>(std::sin(kTau * static_cast<std::uint32_t>(cycles) / kScale));
}

void Advance(PatchVoice& v, std::uint64_t frames, double pitch, bool seek) noexcept {
  const auto start = seek ? 0 : v.frame;
  const auto end = v.finiteDuration ? std::min(Plus(start, frames), v.end) : Plus(start, frames);
  for (unsigned i = 0; i < v.count; ++i) {
    auto& s = v.state[i];
    if (v.nodes[i].op == PatchOp::Oscillator)
      s.phase = (seek ? s.initialPhase : s.phase) +
                static_cast<std::uint32_t>((end - start) * Increment(s, pitch));
    s.filter = 0;  // no unbounded filter-history replay on a virtual return
  }
  v.frame = end;
  v.refresh = true;
}

float Evaluate(PatchVoice& v, float* values, double pitch, unsigned rate) noexcept {
  const auto sub = static_cast<unsigned>(v.frame % kModStep);
  for (unsigned i = 0; i < v.count; ++i) {
    const auto& n = v.nodes[i];
    auto& s = v.state[i];
    float value = 0;
    switch (n.op) {
      case PatchOp::Constant:
        value = n.arg[0];
        break;
      case PatchOp::Oscillator: {
        const auto increment = Increment(s, pitch);
        if (increment != 0)
          value = static_cast<float>(
              Waveform(n.waveform, s.phase / kScale, increment / kScale, n.arg[1]));
        s.phase += increment;
        break;
      }
      case PatchOp::WhiteNoise:
        value = NoiseKind::White(s.key, v.frame);
        break;
      case PatchOp::Add:
        value = values[n.a] + values[n.b];
        break;
      case PatchOp::Multiply:
        value = values[n.a] * values[n.b];
        break;
      case PatchOp::Lowpass:
        s.filter += s.coefficient * (values[n.a] - s.filter);
        value = static_cast<float>(s.filter);
        break;
      case PatchOp::Adsr:
      case PatchOp::RandomHold:
      case PatchOp::Lfo: {
        if (sub == 0 || v.refresh) {
          const auto knot = v.frame - sub;
          s.modStart = ModValue(v, i, rate, knot);
          s.modEnd = ModValue(v, i, rate, Plus(knot, kModStep));
        }
        value = s.modStart + (s.modEnd - s.modStart) * (static_cast<float>(sub) / kModStep);
        break;
      }
      case PatchOp::Clip:
        value = std::clamp(values[n.a], -n.arg[0], n.arg[0]);
        break;
      case PatchOp::Output:
        value = std::clamp(values[n.a], -1.f, 1.f);
        break;
    }
    values[i] = std::clamp(value, -16.f, 16.f);
  }
  v.refresh = false;
  v.frame = Plus(v.frame, 1);
  return values[v.count - 1];
}
}  // namespace

bool PatchKind::Prepare(PatchVoice& v, const SourceStartRequest& rq, SourceStartResult&) noexcept {
  const auto& p = *static_cast<const PatchParams*>(rq.params);
  v.count = DecodePatch(p.program, p.programBytes, v.nodes);
  if (!v.count) return false;
  v.gate = p.gateFrames;
  for (unsigned i = 0; i < v.count; ++i) {
    const auto& n = v.nodes[i];
    auto& s = v.state[i];
    s.key = NoiseKind::ChannelKey(NoiseKind::VoiceKey(0, rq.seed), i);
    if (n.op == PatchOp::Oscillator) {
      s.baseDt = double(n.arg[0]) / rate_;
      if (n.waveform == static_cast<unsigned>(bank_format::Waveform::Saw))
        s.initialPhase = 0x80000000u;
      if (n.waveform == static_cast<unsigned>(bank_format::Waveform::Triangle))
        s.initialPhase = 0x40000000u;
    } else if (n.op == PatchOp::Lowpass) {
      s.coefficient = -std::expm1(-kTau * std::min(double(n.arg[0]) / rate_, 0.5));
    } else if (n.op == PatchOp::Adsr && v.gate) {
      v.finiteDuration = true;
      const auto end = Plus(v.gate, Duration(n.arg[3], rate_));
      v.end = std::max(v.end, Plus(end, (kModStep - end % kModStep) % kModStep));
    }
  }
  Advance(v, rq.startFrame, 1, true);
  return true;
}

void PatchKind::Control(const SourceControlOp* ops, std::uint32_t count) noexcept {
  for (unsigned i = 0; i < count; ++i) {
    auto* v = Get(ops[i].sourceSlot);
    if (v && ops[i].op == ControlOp::Seek) Advance(*v, ops[i].frame, 1, true);
  }
}
void PatchKind::AdvanceVirtual(const AdvanceItem* items, std::uint32_t count,
                               VoiceRenderStatus* status) noexcept {
  for (unsigned i = 0; i < count; ++i) {
    const auto& a = items[i];
    status[i] = {};
    auto* v = Get(a.sourceSlot);
    if (!v) {
      status[i].ended = 1;
      continue;
    }
    if (a.outputFrames) Advance(*v, a.outputFrames, std::isfinite(a.pitch) ? a.pitch : 0, false);
    a.playhead->frame = v->frame;
    status[i].ended = v->finiteDuration && v->frame == v->end;
  }
}
void PatchKind::RenderBatch(const source::RenderBatch& batch, VoiceRenderStatus* status) noexcept {
  for (unsigned k = 0; k < batch.count; ++k) {
    const auto& it = batch.items[k];
    auto& st = status[k];
    st = {};
    auto* v = Get(it.sourceSlot);
    if (!v) {
      st.ended = 1;
      continue;
    }
    const auto frames =
        v->finiteDuration
            ? static_cast<unsigned>(std::min<std::uint64_t>(it.framesRequested, v->end - v->frame))
            : it.framesRequested;
    if (frames != 0) {
      auto* scratch = batch.scratch;
      const auto mark = scratch ? scratch->used : 0;
      const auto bytes = sizeof(float) * v->count;
      auto* values = scratch && scratch->base && mark <= scratch->bytes &&
                             mark <= SIZE_MAX - bytes - alignof(float)
                         ? static_cast<float*>(scratch->Take(bytes, alignof(float)))
                         : nullptr;
      if (values) {
        for (unsigned f = 0; f < frames; ++f) {
          const auto at = it.frameOffset + f;
          const double position = double(at) / std::max(1u, batch.block.frames);
          const auto y = Evaluate(*v, values, Modulation(it.pitch, position), rate_);
          for (unsigned c = 0; c < std::min(it.channels, kMaxChannels); ++c) it.out[c][at] = y;
        }
        scratch->used = mark;
      } else {
        Advance(*v, frames, Modulation(it.pitch, 1), false);
        st.starved = 1;  // core zeroes all outputs before it calls a source
      }
    }
    st.ended = v->finiteDuration && v->frame == v->end;
    st.framesRendered = frames;
    it.playhead->frame = v->frame;
  }
}
}  // namespace snd::source::detail
