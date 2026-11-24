// System 6: EvaluateDspParams (4.3, 4.5 basic, 4.6). Per voice: gain, pitch
// and filter targets (clip values + random ranges + parameter curves composed
// per 4.3, distance attenuation and cone (basic 3D), category gain, layer
// crossfade, fades) and audibility (gain x previous bus gain incl. ducking,
// 4.4). Per bus: gain target (volume ramp, bank volume, snapshots, ducking)
// and the LOD effect bypass request. Per category: gain (volume ramp, mute,
// parents, "all categories" row).
//
// Basic 3D (stage 2): EVNT.curveVolume / curveLowpass (distance curves, dB /
// Hz) or, without a volume curve, inverse distance 1 / max(d, 1) cut to 0
// beyond EVNT.maxDistance (if set); the cone (EVNT.coneInnerDeg / OuterDeg /
// OuterDb). Occlusion, Doppler and rooms are stage 3.
#include <algorithm>
#include <bit>
#include <cmath>

#include "access.hpp"
#include "core_impl.hpp"
#include "params.hpp"

namespace snd::core {

namespace {
constexpr schema::SystemId kSys = schema::SystemId::EvaluateDspParams;
constexpr float kPi = 3.14159265358979f;

float Unit(float x) noexcept { return std::clamp(x, 0.0f, 1.0f); }

float Curve(const bk::BankView* view, std::uint32_t curve, float distance, float fallback) noexcept {
  return curve != bf::kNone && view ? EvaluateCurve(*view, curve, distance) : fallback;
}

struct SpatialTarget {
  float distanceDb;
  float coneDb;
  float inverse;
  float blend;
  float spread;
  float filter;
  float ratio;
  float send;
};

SpatialTarget SpatialAt(const Impl& im, SlotIndex spatial, const bk::BankView* view, std::uint32_t row) noexcept {
  const schema::Tables& t = im.tables;
  const float distance = SND_R(Spatial_distance)[spatial];
  SpatialTarget result{0, 0, 1 / std::max(distance, 1.0f), 1, SND_R(Spatial_shapeSpread)[spatial],
                       1.0e9f, SND_R(Spatial_dopplerRatio)[spatial], 0};
  if (!view) return result;
  const auto volume = view->Column<bk::col::EVNT_curveVolume>()[row];
  if (volume != bf::kNone) {
    result.inverse = 1;
    result.distanceDb = EvaluateCurve(*view, volume, distance);
  } else {
    const float maxD = view->Column<bk::col::EVNT_maxDistance>()[row];
    if (maxD > 0 && distance > maxD) result.inverse = 0;
  }
  const float inner = view->Column<bk::col::EVNT_coneInnerDeg>()[row];
  const float outer = view->Column<bk::col::EVNT_coneOuterDeg>()[row];
  if (outer > 0 && outer >= inner) {
    const float angle = 2.0f * std::acos(std::clamp(SND_R(Spatial_coneCos)[spatial], -1.0f, 1.0f)) * 180.0f / kPi;
    if (angle > inner) {
      const float f = angle >= outer ? 1.0f : (angle - inner) / std::max(outer - inner, 1e-6f);
      result.coneDb = view->Column<bk::col::EVNT_coneOuterDb>()[row] * f;
    }
  }
  result.blend = Unit(Curve(view, view->Column<bk::col::EVNT_curveBlend3D>()[row], distance, 1));
  result.spread = std::max(result.spread, Unit(Curve(view, view->Column<bk::col::EVNT_curveSpread>()[row], distance, 0)));
  result.filter = Curve(view, view->Column<bk::col::EVNT_curveLowpass>()[row], distance, 1.0e9f);
  result.send = Unit(Curve(view, view->Column<bk::col::EVNT_curveReverbSend>()[row], distance, 0));
  if (result.blend == 0) {
    result.ratio = 1;
    result.filter = 1.0e9f;
  } else if (result.blend < 1) {
    if (result.ratio != 1) result.ratio = std::pow(result.ratio, result.blend);
    if (result.filter < dsp::kVoiceLowpassOffHz && result.filter > 0)
      result.filter = dsp::kVoiceLowpassOffHz * std::pow(result.filter / dsp::kVoiceLowpassOffHz, result.blend);
  }
  return result;
}

struct SpatialGain {
  float distanceDb = 0, coneDb = 0, linear = 1, filter = 1e9f, ratio = 1, distance = 0;
};

SpatialGain ListenerMix(const Impl& im, SlotIndex em, const bk::BankView* view, std::uint32_t row,
                        VoiceSpatialMix& mix) noexcept {
  const schema::Tables& t = im.tables;
  SpatialGain out{};
  mix = VoiceSpatialMix{};
  if (em >= im.cfg.maxEmitters) return out;
  SlotIndex nearest = kNoSlot;
  float nearestD = 0;
  bool hasWeight = false;
  for (unsigned l = 0; l < im.cfg.maxListeners; ++l) {
    const SlotIndex s = em * im.cfg.maxListeners + l;
    if (!base::rows::IsLive(t.Spatial.rows, s) || !SND_R(Spatial_valid)[s]) continue;
    const float d = SND_R(Spatial_distance)[s];
    if (nearest == kNoSlot || d < nearestD) { nearest = s; nearestD = d; }
    const float weight = im.cfg.listenerMode == SND_LISTENER_WEIGHTED ? SND_R(Listener_weight)[l] : 1;
    mix.listeners[mix.count++] = ListenerContribution{s, std::max(0.0f, weight), 0, 1, 0};
    hasWeight |= weight > 0;
  }
  if (nearest == kNoSlot) return out;
  if (im.cfg.listenerMode == SND_LISTENER_NEAREST) {
    mix.count = 1;
    mix.listeners[0] = ListenerContribution{nearest, 1, 0, 1, 0};
  } else if (!hasWeight) {
    for (unsigned i = 0; i < mix.count; ++i) mix.listeners[i].weight = 1;
  }
  double sumWeight = 0, sumGain = 0, best = -1;
  double contribution[SND_MAX_LISTENERS]{};
  for (unsigned i = 0; i < mix.count; ++i) {
    auto& item = mix.listeners[i];
    const SpatialTarget target = SpatialAt(im, item.spatial, view, row);
    item.blend = target.blend;
    item.spread = target.spread;
    item.curveSend = target.send * target.blend;
    const float gain3 = target.inverse * DbToGain(target.distanceDb + target.coneDb);
    const float gain = (1 - target.blend) + target.blend * gain3;
    contribution[i] = static_cast<double>(item.weight) * gain;
    sumGain += contribution[i];
    sumWeight += item.weight;
    if (contribution[i] > best) {
      best = contribution[i];
      mix.dominant = item.spatial % im.cfg.maxListeners;
      out.filter = target.filter;
      out.ratio = target.ratio;
      out.distance = SND_R(Spatial_distance)[item.spatial];
    }
    if (mix.count == 1) {
      item.weight = 1;
      mix.curveSend = item.curveSend;
      // Preserve the original order of dB additions at full blend.
      if (item.blend == 1) {
        out.distanceDb = target.distanceDb;
        out.coneDb = target.coneDb;
        out.linear = target.inverse;
      } else out.linear = gain;
      return out;
    }
  }
  out.linear = static_cast<float>(sumGain / sumWeight);
  for (unsigned i = 0; i < mix.count; ++i) {
    auto& item = mix.listeners[i];
    item.weight = sumGain > 0 ? static_cast<float>(contribution[i] / sumGain) : 0;
    mix.curveSend += item.weight * item.curveSend;
  }
  return out;
}

}  // namespace

void EvaluateDspParams(Impl& im) noexcept {
  schema::Tables& t = im.tables;
  const ClockSample endClock = im.clock + im.cfg.blockFrames;
  const float rate = static_cast<float>(im.cfg.sampleRate);
  const float blockMs = 1000.0f * static_cast<float>(im.cfg.blockFrames) / rate;

  // --- categories: own volume x mute x parents x "all" (D-30) ---
  const schema::TableRows& cr = t.Category.rows;
  float* catGain = SND_W(Category_gain);
  for (std::uint32_t d = 0; d < cr.count; ++d) {
    SlotIndex s = base::rows::SlotAt(cr, d);
    float g = 1.0f;
    for (std::uint32_t guard = 0; guard < im.cfg.catCap && s != kNoSlot; ++guard) {
      const std::uint32_t cd = base::rows::DenseOf(cr, s);
      if (cd == kNoSlot) break;
      const CategoryRequest& rq = SND_R(Category_request)[cd];
      g *= rq.muted != 0u ? 0.0f : RampValue(rq.volume, endClock) * DbToGain(im.catDefs[s].baseVolumeDb);
      if (s == im.globalCategory) break;
      s = im.catDefs[s].parentSlot == kNoSlot ? im.globalCategory : im.catDefs[s].parentSlot;
    }
    catGain[d] = g;
  }
  for (std::uint32_t s = 0; s < im.cfg.catCap; ++s) im.dp.catPlaying[s] = 0;

  // --- voices ---
  const schema::TableRows& vr = t.Voice.rows;
  const SlotIndex* vpb = SND_R(Voice_playbackSlot);
  const SlotIndex* vbus = SND_R(Voice_busSlot);
  const SlotIndex* vem = SND_R(Voice_emitterSlot);
  const VoiceLifecycle* vlc = SND_R(Voice_lifecycle);
  const ClipRef* vclip = SND_R(Voice_clipRef);
  Ramp* vgain = SND_W(Voice_gain);
  Ramp* vpitch = SND_W(Voice_pitch);
  FilterTarget* vfilt = SND_W(Voice_filterTarget);
  float* vaud = SND_W(Voice_audibility);
  const float* busGainPrev = SND_R(Bus_gain);
  for (std::uint32_t d = 0; d < vr.count; ++d) {
    const SlotIndex v = base::rows::SlotAt(vr, d);
    const VoiceStatic& vs = im.ev.vs[v];
    const SlotIndex pb = vpb[d];
    const std::uint32_t pd = base::rows::DenseOf(t.Playback.rows, pb);
    float gainDb = vs.gainDb;
    float cents = vs.pitchCents;
    float pan = vs.pan;
    float lowpass = vs.lowpassHz > 0.0f ? vs.lowpassHz : 1.0e9f;
    float highpass = 0.0f;
    float linear = vs.linearGain * vs.musicGain;
    const bool ownPb = pb < im.cfg.maxPlaybacks;  // 1.2 (D-104): the playback's own volume / pitch
    if (ownPb) linear *= im.ap.playbackVolume[pb];
    float fadeEnd = 1.0f;
    if (pd != kNoSlot) fadeEnd = SND_R(Playback_logic)[pd].fadeGain;
    const BankRecord* br = RegisteredBank(im, vclip[d].bankSlot);
    const bk::BankView* view = br != nullptr ? br->view : nullptr;
    const bool event = view && vs.eventRow < view->Rows(bf::chunk::EVNT);
    const bool emitterLive = base::rows::IsCurrent(t.Emitter.rows, HandleRef{vem[d], vs.emitterGeneration});
    const SlotIndex em = (vs.flags & kVs3D) != 0u && emitterLive ? vem[d] : kNoSlot;
    VoiceSpatialMix& mix = im.dp.mix[v];
    const SpatialGain spatial = ListenerMix(im, em, event ? view : nullptr, vs.eventRow, mix);
    const float distance = spatial.distance;
    if (view != nullptr && vs.eventRow < view->Rows(bf::chunk::EVNT)) {
      const std::uint32_t row = vs.eventRow;
      // Parameter curves (4.3): volume dB add, pitch cents add, lowpass min, highpass max, others multiply.
      const std::uint32_t cf = view->Column<bk::col::EVNT_curveFirst>()[row];
      const std::uint32_t cc = cf == bf::kNone ? 0u : view->Column<bk::col::EVNT_curveCount>()[row];
      const SlotIndex emitter = pd != kNoSlot && emitterLive ? vem[d] : kNoSlot;
      for (std::uint32_t k = 0; k < cc; ++k) {
        const std::uint32_t cv = cf + k;
        const Id input = view->Column<bk::col::CURV_input>()[cv];
        float x = 0.0f;
        if (input == bf::kInputDistance) {
          x = distance;
        } else {
          x = ParamValueOf<kSys>(im, input, pb, emitter);
        }
        const float y = EvaluateCurve(*view, cv, x);
        switch (static_cast<bf::CurveTarget>(view->Column<bk::col::CURV_target>()[cv])) {
          case bf::CurveTarget::Volume: gainDb += y; break;
          case bf::CurveTarget::Pitch: cents += y; break;
          case bf::CurveTarget::Lowpass: lowpass = y < lowpass ? y : lowpass; break;
          case bf::CurveTarget::Highpass: highpass = y > highpass ? y : highpass; break;
          case bf::CurveTarget::Pan: pan += y; break;
          default: break;  // send / effect / spread / blend: stage 3 (Render reads spread & blend curves)
        }
      }
      // Layer crossfade (4.2): weight by the track's layer parameter.
      if (view->Column<bk::col::EVNT_sequenceKind>()[row] == static_cast<std::uint32_t>(bf::SequenceKind::LayerCrossfade) &&
          vs.trackRow < view->Rows(bf::chunk::TRAK) && vclip[d].clipIndex < view->Rows(bf::chunk::CLIP)) {
        const Id lp = view->Column<bk::col::TRAK_layerParam>()[vs.trackRow];
        if (lp != 0u) {
          const float x = ParamValueOf<kSys>(im, lp, pb, emitter);
          const float lo = view->Column<bk::col::CLIP_layerMin>()[vclip[d].clipIndex];
          const float hi = view->Column<bk::col::CLIP_layerMax>()[vclip[d].clipIndex];
          const float w = (hi - lo) * 0.1f;
          float g = 1.0f;
          if (x < lo) g = w > 0.0f ? 1.0f - (lo - x) / w : 0.0f;
          if (x > hi) g = w > 0.0f ? 1.0f - (x - hi) / w : 0.0f;
          linear *= g < 0.0f ? 0.0f : g;
        }
      }
    }
    if (mix.count != 0) {
      gainDb += spatial.distanceDb;
      gainDb += spatial.coneDb;
      linear *= spatial.linear;
      lowpass = std::min(lowpass, spatial.filter);
      const auto ed = base::rows::DenseOf(t.Emitter.rows, em);
      const Occlusion occ = SND_R(Emitter_occlusion)[ed];
      if (occ.occlusion > 0) gainDb -= 12 * occ.occlusion;
      mix.occlusion = occ.occlusion;
      mix.obstruction = occ.obstruction;
    }
    // Category gain and the "voices playing" count for rule ducking.
    float cat = 1.0f;
    if (vs.categorySlot < im.cfg.catCap) {
      const std::uint32_t cd = base::rows::DenseOf(cr, vs.categorySlot);
      if (cd != kNoSlot) cat = catGain[cd];
      if ((vlc[d].flags & kVoicePausedFlag) == 0u) {
        for (SlotIndex s = vs.categorySlot, guard = 0; s != kNoSlot && guard < im.cfg.catCap; ++guard) {
          ++im.dp.catPlaying[s];
          if (s == im.globalCategory) break;
          s = im.catDefs[s].parentSlot;
        }
      }
    }
    // Clip fade-in and voice release.
    float env = 1.0f;
    if (vs.fadeInSeconds > 0.0f && endClock > vlc[d].startClock) {
      const float el = static_cast<float>(endClock - vlc[d].startClock) / rate;
      env = el >= vs.fadeInSeconds ? 1.0f : el / vs.fadeInSeconds;
    }
    // Voice releases (stops, steals) are applied per sample by Render (sample accurate).
    const float gEnd = DbToGain(gainDb) * linear * cat * fadeEnd * env;
    float pEnd = std::exp2(cents / 1200.0f);
    pEnd *= spatial.ratio;
    if (ownPb) pEnd *= im.ap.playbackPitch[pb];
    // 1.2 (D-88): the voices of a music playback follow its transport's pitch (8.1).
    if (pd != kNoSlot) {
      const SlotIndex ts = SND_R(Playback_transportSlot)[pd];
      const std::uint32_t td = ts == kNoSlot ? kNoSlot : base::rows::DenseOf(t.Transport.rows, ts);
      if (td != kNoSlot) {
        const float tp = SND_R(Transport_pitch)[td];
        if (tp > 0.0f && std::isfinite(tp)) pEnd *= tp;
      }
    }
    const bool fresh = vlc[d].state == static_cast<std::uint8_t>(VoiceStateId::Starting);
    vgain[d] = Ramp{fresh ? gEnd : vgain[d].end, gEnd};
    vpitch[d] = Ramp{fresh ? pEnd : vpitch[d].end, pEnd};
    vfilt[d] = FilterTarget{lowpass, highpass};
    im.dp.pan[v] = pan < -1.0f ? -1.0f : (pan > 1.0f ? 1.0f : pan);
    const SlotIndex bus = vbus[d];
    const std::uint32_t bd = bus == kNoSlot ? kNoSlot : base::rows::DenseOf(t.Bus.rows, bus);
    vaud[d] = gEnd * (bd != kNoSlot ? busGainPrev[bd] : 1.0f);
  }

  // --- ducking rules (4.6): smoothed reduction in dB ---
  const BusPeak* peaks = SND_R(Bus_peak);
  for (std::uint32_t i = 0; i < im.cfg.duckCap; ++i) {
    const DuckRule& r = im.duckRules[i];
    if (r.refCount == 0u) continue;
    bool active = false;
    if (r.kind == 1u) {
      active = r.sourceSlot < im.cfg.catCap && im.dp.catPlaying[r.sourceSlot] != 0u;
    } else if (r.sourceSlot != kNoSlot) {
      const std::uint32_t bd = base::rows::DenseOf(t.Bus.rows, r.sourceSlot);
      if (bd != kNoSlot) {
        float pk = 0.0f;
        for (std::uint32_t c = 0; c < kMaxChannels; ++c) pk = peaks[bd].peak[c] > pk ? peaks[bd].peak[c] : pk;
        active = pk > DbToGain(r.thresholdDb);
      }
    }
    const float target = active ? -std::fabs(r.amountDb) : 0.0f;
    float& cur = im.dp.duckDb[i];
    const float tau = target < cur ? r.attackMs : r.releaseMs;
    const float k = tau > 0.0f ? 1.0f - std::exp(-blockMs / tau) : 1.0f;
    cur += (target - cur) * k;
  }

  // --- buses: volume ramp x bank volume x snapshots x ducking ---
  const schema::TableRows& br = t.Bus.rows;
  float* gt = SND_W(Bus_gainTarget);
  std::uint32_t* bypass = SND_W(Bus_effectBypass);
  float* busDb = im.frameArena.AllocateArray<float>(im.cfg.busCap);
  if (busDb == nullptr) return;
  for (std::uint32_t s = 0; s < im.cfg.busCap; ++s) busDb[s] = 0.0f;
  const schema::TableRows& sr = t.Snapshot.rows;
  float* sw = SND_W(Snapshot_weight);
  for (std::uint32_t d = 0; d < sr.count; ++d) {
    const SlotIndex s = base::rows::SlotAt(sr, d);
    const float w = RampValue(SND_R(Snapshot_request)[d].weight, endClock);
    sw[d] = w;
    if (w == 0.0f) continue;
    const SnapDef& def = im.snapDefs[s];
    const BankRecord* rec = RegisteredBank(im, def.defBank);
    if (rec == nullptr) continue;
    const bk::BankView& v = *rec->view;
    const std::uint32_t vf = v.Column<bk::col::SNAP_valueFirst>()[def.defRow];
    const std::uint32_t vc = v.Column<bk::col::SNAP_valueCount>()[def.defRow];
    for (std::uint32_t k = 0; k < vc; ++k) {
      const std::uint32_t row = vf + k;
      if (v.Column<bk::col::SNPV_targetKind>()[row] != 0u) continue;  // effect params / send levels: stage 3
      const std::uint32_t busRow = v.Column<bk::col::SNPV_targetRow>()[row];
      if (busRow < rec->busCount && rec->busSlot[busRow] != kNoSlot) {
        busDb[rec->busSlot[busRow]] += w * v.Column<bk::col::SNPV_value>()[row];
      }
    }
  }
  for (std::uint32_t i = 0; i < im.cfg.duckCap; ++i) {
    const DuckRule& r = im.duckRules[i];
    if (r.refCount != 0u && r.targetBus != kNoSlot && r.targetBus < im.cfg.busCap) busDb[r.targetBus] += im.dp.duckDb[i];
  }
  for (std::uint32_t d = 0; d < br.count; ++d) {
    const SlotIndex s = base::rows::SlotAt(br, d);
    const BusDef& def = im.busDefs[s];
    gt[d] = RampValue(SND_R(Bus_volume)[d], endClock) * DbToGain(def.baseVolumeDb + busDb[s]);
    bypass[d] = (im.rd.lod >= 3u && (def.flags & bf::kBusLodBypassable) != 0u) ? 1u : 0u;
  }
}

}  // namespace snd::core
