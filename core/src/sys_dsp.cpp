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
#include <bit>
#include <cmath>

#include "access.hpp"
#include "core_impl.hpp"
#include "params.hpp"

namespace snd::core {

namespace {
constexpr schema::SystemId kSys = schema::SystemId::EvaluateDspParams;
constexpr float kPi = 3.14159265358979f;

// Nearest valid listener of an emitter (listener mode Nearest; Weighted / Split are stage 3).
std::uint32_t NearestSpatial(const Impl& im, SlotIndex emitter) noexcept {
  const schema::Tables& t = im.tables;
  if (emitter == kNoSlot || emitter >= im.cfg.maxEmitters) return kNoSlot;
  std::uint32_t best = kNoSlot;
  float bestD = 0.0f;
  for (std::uint32_t l = 0; l < im.cfg.maxListeners; ++l) {
    const std::uint32_t s = emitter * im.cfg.maxListeners + l;
    if (!base::rows::IsLive(t.Spatial.rows, s) || SND_R(Spatial_valid)[s] == 0u) continue;
    const float dd = SND_R(Spatial_distance)[s];
    if (best == kNoSlot || dd < bestD) {
      best = s;
      bestD = dd;
    }
  }
  return best;
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
    const std::uint32_t spatial = (vs.flags & kVs3D) != 0u ? NearestSpatial(im, vem[d]) : kNoSlot;
    const float distance = spatial != kNoSlot ? SND_R(Spatial_distance)[spatial] : 0.0f;
    if (view != nullptr && vs.eventRow < view->Rows(bf::chunk::EVNT)) {
      const std::uint32_t row = vs.eventRow;
      // Parameter curves (4.3): volume dB add, pitch cents add, lowpass min, highpass max, others multiply.
      const std::uint32_t cf = view->Column<bk::col::EVNT_curveFirst>()[row];
      const std::uint32_t cc = cf == bf::kNone ? 0u : view->Column<bk::col::EVNT_curveCount>()[row];
      const SlotIndex emitter = pd != kNoSlot ? SND_R(Voice_emitterSlot)[d] : kNoSlot;
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
      // Basic 3D: distance and cone.
      if (spatial != kNoSlot) {
        const std::uint32_t cvol = view->Column<bk::col::EVNT_curveVolume>()[row];
        if (cvol != bf::kNone) {
          gainDb += EvaluateCurve(*view, cvol, distance);
        } else {
          const float maxD = view->Column<bk::col::EVNT_maxDistance>()[row];
          linear *= (maxD > 0.0f && distance > maxD) ? 0.0f : 1.0f / (distance > 1.0f ? distance : 1.0f);
        }
        const std::uint32_t clp = view->Column<bk::col::EVNT_curveLowpass>()[row];
        if (clp != bf::kNone) {
          const float y = EvaluateCurve(*view, clp, distance);
          lowpass = y < lowpass ? y : lowpass;
        }
        const float inner = view->Column<bk::col::EVNT_coneInnerDeg>()[row];
        const float outer = view->Column<bk::col::EVNT_coneOuterDeg>()[row];
        if (outer > 0.0f && outer >= inner) {
          float cc2 = SND_R(Spatial_coneCos)[spatial];
          cc2 = cc2 < -1.0f ? -1.0f : (cc2 > 1.0f ? 1.0f : cc2);
          const float angle = 2.0f * std::acos(cc2) * 180.0f / kPi;  // full cone angle
          const float outerDb = view->Column<bk::col::EVNT_coneOuterDb>()[row];
          if (angle > inner) {
            const float tt = angle >= outer ? 1.0f : (angle - inner) / (outer - inner > 1e-6f ? outer - inner : 1.0f);
            gainDb += outerDb * tt;
          }
        }
      }
    } else if (spatial != kNoSlot) {
      linear *= 1.0f / (distance > 1.0f ? distance : 1.0f);  // dynamic voices: plain inverse distance
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
    const SlotIndex bus = RoutedBus(im, vbus[d]);
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
