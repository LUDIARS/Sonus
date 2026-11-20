// In-memory banks for the core tests, written with the build tool's
// low-level writer (bank format 2.0). Rows are added in the order the format
// requires (buses: inputs first; nested events after their owner; category
// parents after the child).
#ifndef SND_CORE_TEST_TEST_BANK_HPP
#define SND_CORE_TEST_TEST_BANK_HPP

#include <cstdint>
#include <cstring>
#include <vector>

#include "snd/buildtool/bank_writer.hpp"
#include "snd/format/bank_format.hpp"
#include "snd/id.hpp"

namespace coretest {

namespace bf = snd::bank_format;
namespace bt = snd::buildtool;
inline constexpr std::uint32_t kNone = bf::kNone;

struct TClip {
  std::uint32_t kind = 0;  // 0 asset, 1 nested event, 2 synth
  std::uint32_t target = 0;
  float volumeDb = 0, pitchCents = 0, randVolumeDb = 0, randPitchCents = 0, randPan = 0, randStartMs = 0;
  float randFilterHz = 0, layerMin = 0, layerMax = 0, fadeInMs = 0, weight = 1, startMs = 0;
  std::uint32_t switchValue = 0, loopCount = 1;
  float fadeOutMs = 0;  // stage 3 (group 4)
};
struct TAction {
  float timeMs = 0;
  std::uint32_t action = 0;
  std::uint32_t target = 0;
  float value = 0;
};
struct TTrack {
  std::vector<TClip> clips;
  std::uint32_t bus = kNone;
  std::uint32_t layerParam = 0;
  std::vector<TAction> actions;
};
struct TEvent {
  snd::Id id = 0;
  std::uint32_t sequence = 0, priority = 128, bus = 0, category = kNone, group = kNone, maxInstances = 0, flags = 0;
  float retriggerMs = 0, fadeOutMs = 0, comboWindowMs = 0, maxDistance = 0, coneInner = 0, coneOuter = 0, coneOuterDb = 0;
  std::uint32_t curveVolume = kNone, curveLowpass = kNone, curveSpread = kNone, curveBlend = kNone;
  std::vector<std::uint32_t> curves;  // CURV rows (must be contiguous)
  std::vector<TTrack> tracks;
  std::uint32_t virtualReturn = 0;  // stage 3 (group 4): EVNT.virtualReturn
};
struct TBus {
  snd::Id id = 0;
  std::uint32_t parent = kNone, channels = 0, flags = 0;
  float volumeDb = 0;
  std::vector<std::uint32_t> effects;  // EFFX rows (contiguous)
  struct Send {
    std::uint32_t target;
    float levelDb;
    std::uint32_t preFader;
  };
  std::vector<Send> sends;
};
struct TAsset {
  snd::Id id = 0;
  std::uint32_t channels = 1, rate = 48000;
  std::vector<float> data;  // interleaved PCMF
  std::uint64_t loopStart = 0, loopEnd = 0;
  std::uint32_t flags = 0;  // stage 3 (group 4): ASST.flags
};
struct TSynth {
  std::uint32_t kind = 0, waveform = 0, channels = 1;
  float frequency = 440;
};
struct TCurve {
  std::uint32_t input = 0, target = 0, compose = 0;
  std::vector<std::pair<float, float>> points;
};
struct TParam {
  snd::Id id = 0;
  std::uint32_t kind = 0;
  float minValue = 0, maxValue = 1, defaultValue = 0, rampRate = 0;
};
struct TCategory {
  snd::Id id = 0;
  std::uint32_t parent = kNone, flags = 0;
  float volumeDb = 0;
};
struct TGroup {
  snd::Id id = 0;
  std::uint32_t maxVoices = 0, stealMode = 0;
};
struct TSnapshot {
  snd::Id id = 0;
  struct Value {
    std::uint32_t kind, row, param;
    float value;
  };
  std::vector<Value> values;
};
struct TDuck {
  std::uint32_t kind = 1, source = 0, target = 0;
  float amountDb = 6, attackMs = 10, releaseMs = 200, thresholdDb = 0;
};
struct TEffect {
  snd::Id type = 0;
  std::vector<float> params;
};

struct TestBank {
  std::vector<TEvent> events;
  std::vector<TBus> buses;
  std::vector<TAsset> assets;
  std::vector<TSynth> synths;
  std::vector<TCurve> curves;
  std::vector<TParam> params;
  std::vector<TCategory> categories;
  std::vector<TGroup> groups;
  std::vector<TSnapshot> snapshots;
  std::vector<TDuck> ducks;
  std::vector<TEffect> effects;

  std::vector<std::uint8_t> Build(snd::Id bankId, std::uint32_t languageId = 0,
                                  std::vector<bf::Dependency> deps = {}) const {
    namespace c = bf::col;
    bt::BankWriter w;
    bt::TableBuilder ev(bf::chunk::EVNT), tr(bf::chunk::TRAK), cl(bf::chunk::CLIP), ac(bf::chunk::ACTN);
    for (const TEvent& e : events) {
      const std::uint32_t r = ev.AddRow();
      ev.U32(c::EVNT_id, r, e.id);
      ev.U32(c::EVNT_trackFirst, r, tr.rows());
      ev.U32(c::EVNT_trackCount, r, static_cast<std::uint32_t>(e.tracks.size()));
      ev.U32(c::EVNT_sequenceKind, r, e.sequence);
      ev.U32(c::EVNT_priority, r, e.priority);
      ev.U32(c::EVNT_bus, r, e.bus);
      ev.U32(c::EVNT_category, r, e.category);
      ev.U32(c::EVNT_limitGroup, r, e.group);
      ev.U32(c::EVNT_maxInstances, r, e.maxInstances);
      ev.F32(c::EVNT_retriggerMs, r, e.retriggerMs);
      ev.U32(c::EVNT_flags, r, e.flags);
      ev.U32(c::EVNT_curveVolume, r, e.curveVolume);
      ev.U32(c::EVNT_curveLowpass, r, e.curveLowpass);
      ev.U32(c::EVNT_curveSpread, r, e.curveSpread);
      ev.U32(c::EVNT_curveBlend3D, r, e.curveBlend);
      ev.F32(c::EVNT_maxDistance, r, e.maxDistance);
      ev.F32(c::EVNT_coneInnerDeg, r, e.coneInner);
      ev.F32(c::EVNT_coneOuterDeg, r, e.coneOuter);
      ev.F32(c::EVNT_coneOuterDb, r, e.coneOuterDb);
      ev.F32(c::EVNT_fadeOutMs, r, e.fadeOutMs);
      ev.F32(c::EVNT_comboWindowMs, r, e.comboWindowMs);
      if (e.virtualReturn != 0u) ev.U32(c::EVNT_virtualReturn, r, e.virtualReturn);
      if (!e.curves.empty()) {
        ev.U32(c::EVNT_curveFirst, r, e.curves.front());
        ev.U32(c::EVNT_curveCount, r, static_cast<std::uint32_t>(e.curves.size()));
      }
      for (const TTrack& t : e.tracks) {
        const std::uint32_t tr0 = tr.AddRow();
        tr.U32(c::TRAK_event, tr0, r);
        tr.U32(c::TRAK_clipFirst, tr0, cl.rows());
        tr.U32(c::TRAK_clipCount, tr0, static_cast<std::uint32_t>(t.clips.size()));
        tr.U32(c::TRAK_bus, tr0, t.bus);
        tr.U32(c::TRAK_layerParam, tr0, t.layerParam);
        if (!t.actions.empty()) {
          tr.U32(c::TRAK_actionFirst, tr0, ac.rows());
          tr.U32(c::TRAK_actionCount, tr0, static_cast<std::uint32_t>(t.actions.size()));
          for (const TAction& a : t.actions) {
            const std::uint32_t ar = ac.AddRow();
            ac.F32(c::ACTN_timeMs, ar, a.timeMs);
            ac.U32(c::ACTN_action, ar, a.action);
            ac.U32(c::ACTN_target, ar, a.target);
            ac.F32(c::ACTN_value, ar, a.value);
          }
        }
        for (const TClip& k : t.clips) {
          const std::uint32_t cr = cl.AddRow();
          cl.U32(c::CLIP_kind, cr, k.kind);
          cl.U32(c::CLIP_target, cr, k.target);
          cl.F32(c::CLIP_startMs, cr, k.startMs);
          cl.F32(c::CLIP_weight, cr, k.weight);
          cl.F32(c::CLIP_volumeDb, cr, k.volumeDb);
          cl.F32(c::CLIP_pitchCents, cr, k.pitchCents);
          cl.F32(c::CLIP_randVolumeDb, cr, k.randVolumeDb);
          cl.F32(c::CLIP_randPitchCents, cr, k.randPitchCents);
          cl.F32(c::CLIP_randPan, cr, k.randPan);
          cl.F32(c::CLIP_randStartMs, cr, k.randStartMs);
          cl.F32(c::CLIP_randFilterHz, cr, k.randFilterHz);
          cl.U32(c::CLIP_switchValue, cr, k.switchValue);
          cl.F32(c::CLIP_layerMin, cr, k.layerMin);
          cl.F32(c::CLIP_layerMax, cr, k.layerMax);
          cl.F32(c::CLIP_fadeInMs, cr, k.fadeInMs);
          cl.U32(c::CLIP_loopCount, cr, k.loopCount);
          if (k.fadeOutMs != 0.0f) cl.F32(c::CLIP_fadeOutMs, cr, k.fadeOutMs);
        }
      }
    }
    w.AddTable(ev);
    w.AddTable(tr);
    w.AddTable(cl);
    w.AddTable(ac);
    // Buses, sends, effects.
    bt::TableBuilder bu(bf::chunk::BUSS), se(bf::chunk::SEND), fx(bf::chunk::EFFX), pv(bf::chunk::EFPV);
    for (const TBus& b : buses) {
      const std::uint32_t r = bu.AddRow();
      bu.U32(c::BUSS_id, r, b.id);
      bu.U32(c::BUSS_parent, r, b.parent);
      bu.F32(c::BUSS_volumeDb, r, b.volumeDb);
      bu.U32(c::BUSS_channels, r, b.channels);
      bu.U32(c::BUSS_flags, r, b.flags);
      if (!b.effects.empty()) {
        bu.U32(c::BUSS_effectFirst, r, b.effects.front());
        bu.U32(c::BUSS_effectCount, r, static_cast<std::uint32_t>(b.effects.size()));
      }
      if (!b.sends.empty()) {
        bu.U32(c::BUSS_sendFirst, r, se.rows());
        bu.U32(c::BUSS_sendCount, r, static_cast<std::uint32_t>(b.sends.size()));
        for (const TBus::Send& s : b.sends) {
          const std::uint32_t sr = se.AddRow();
          se.U32(c::SEND_targetBus, sr, s.target);
          se.F32(c::SEND_levelDb, sr, s.levelDb);
          se.U32(c::SEND_preFader, sr, s.preFader);
        }
      }
    }
    for (const TEffect& e : effects) {
      const std::uint32_t r = fx.AddRow();
      fx.U32(c::EFFX_effectType, r, e.type);
      if (!e.params.empty()) {
        fx.U32(c::EFFX_paramFirst, r, pv.rows());
        fx.U32(c::EFFX_paramCount, r, static_cast<std::uint32_t>(e.params.size()));
        for (float v : e.params) pv.F32(c::EFPV_value, pv.AddRow(), v);
      }
    }
    w.AddTable(bu);
    w.AddTable(se);
    w.AddTable(fx);
    w.AddTable(pv);
    // Assets (PCMF resident, each 64-byte aligned in WAVE).
    bt::TableBuilder as(bf::chunk::ASST);
    std::vector<std::uint8_t> wave;
    for (const TAsset& a : assets) {
      const std::uint32_t r = as.AddRow();
      while (wave.size() % 64u != 0u) wave.push_back(0);
      const std::uint64_t off = wave.size();
      const std::size_t bytes = a.data.size() * sizeof(float);
      wave.resize(wave.size() + bytes);
      if (bytes != 0u) std::memcpy(wave.data() + off, a.data.data(), bytes);
      as.U32(c::ASST_id, r, a.id);
      as.U32(c::ASST_loadForm, r, 0);
      as.U32(c::ASST_codec, r, SND_CODEC_PCM_F32);
      as.U32(c::ASST_channels, r, a.channels);
      as.U32(c::ASST_sampleRate, r, a.rate);
      as.U64(c::ASST_totalFrames, r, a.data.size() / a.channels);
      as.U32(c::ASST_blockAlign, r, 4u * a.channels);
      as.U32(c::ASST_framesPerPacket, r, 1u);
      as.U64(c::ASST_dataOffset, r, off);
      as.U64(c::ASST_dataBytes, r, bytes);
      as.U64(c::ASST_loopStart, r, a.loopStart);
      as.U64(c::ASST_loopEnd, r, a.loopEnd);
      if (a.flags != 0u) as.U32(c::ASST_flags, r, a.flags);
    }
    w.AddTable(as);
    if (!wave.empty()) w.AddBlob(bf::chunk::WAVE, wave);
    bt::TableBuilder sy(bf::chunk::SYNP);
    for (const TSynth& s : synths) {
      const std::uint32_t r = sy.AddRow();
      sy.U32(c::SYNP_kind, r, s.kind);
      sy.U32(c::SYNP_waveform, r, s.waveform);
      sy.F32(c::SYNP_frequencyHz, r, s.frequency);
      sy.U32(c::SYNP_channels, r, s.channels);
    }
    w.AddTable(sy);
    bt::TableBuilder cu(bf::chunk::CURV), cp(bf::chunk::CPNT);
    for (const TCurve& k : curves) {
      const std::uint32_t r = cu.AddRow();
      cu.U32(c::CURV_pointFirst, r, cp.rows());
      cu.U32(c::CURV_pointCount, r, static_cast<std::uint32_t>(k.points.size()));
      cu.U32(c::CURV_input, r, k.input);
      cu.U32(c::CURV_target, r, k.target);
      cu.U32(c::CURV_compose, r, k.compose);
      for (const auto& p : k.points) {
        const std::uint32_t pr = cp.AddRow();
        cp.F32(c::CPNT_x, pr, p.first);
        cp.F32(c::CPNT_y, pr, p.second);
      }
    }
    w.AddTable(cu);
    w.AddTable(cp);
    bt::TableBuilder pa(bf::chunk::PARM);
    for (const TParam& p : params) {
      const std::uint32_t r = pa.AddRow();
      pa.U32(c::PARM_id, r, p.id);
      pa.U32(c::PARM_kind, r, p.kind);
      pa.F32(c::PARM_minValue, r, p.minValue);
      pa.F32(c::PARM_maxValue, r, p.maxValue);
      pa.F32(c::PARM_defaultValue, r, p.defaultValue);
      pa.F32(c::PARM_rampRate, r, p.rampRate);
    }
    w.AddTable(pa);
    bt::TableBuilder ca(bf::chunk::CATG);
    for (const TCategory& k : categories) {
      const std::uint32_t r = ca.AddRow();
      ca.U32(c::CATG_id, r, k.id);
      ca.U32(c::CATG_parent, r, k.parent);
      ca.U32(c::CATG_flags, r, k.flags);
      ca.F32(c::CATG_volumeDb, r, k.volumeDb);
    }
    w.AddTable(ca);
    bt::TableBuilder vg(bf::chunk::VLGR);
    for (const TGroup& g : groups) {
      const std::uint32_t r = vg.AddRow();
      vg.U32(c::VLGR_id, r, g.id);
      vg.U32(c::VLGR_maxVoices, r, g.maxVoices);
      vg.U32(c::VLGR_stealMode, r, g.stealMode);
    }
    w.AddTable(vg);
    bt::TableBuilder sn(bf::chunk::SNAP), sv(bf::chunk::SNPV);
    for (const TSnapshot& s : snapshots) {
      const std::uint32_t r = sn.AddRow();
      sn.U32(c::SNAP_id, r, s.id);
      sn.U32(c::SNAP_valueFirst, r, sv.rows());
      sn.U32(c::SNAP_valueCount, r, static_cast<std::uint32_t>(s.values.size()));
      for (const TSnapshot::Value& v : s.values) {
        const std::uint32_t vr = sv.AddRow();
        sv.U32(c::SNPV_targetKind, vr, v.kind);
        sv.U32(c::SNPV_targetRow, vr, v.row);
        sv.U32(c::SNPV_paramIndex, vr, v.param);
        sv.F32(c::SNPV_value, vr, v.value);
      }
    }
    w.AddTable(sn);
    w.AddTable(sv);
    bt::TableBuilder du(bf::chunk::DUCK);
    for (const TDuck& d : ducks) {
      const std::uint32_t r = du.AddRow();
      du.U32(c::DUCK_kind, r, d.kind);
      du.U32(c::DUCK_source, r, d.source);
      du.U32(c::DUCK_targetBus, r, d.target);
      du.F32(c::DUCK_amountDb, r, d.amountDb);
      du.F32(c::DUCK_attackMs, r, d.attackMs);
      du.F32(c::DUCK_releaseMs, r, d.releaseMs);
      du.F32(c::DUCK_thresholdDb, r, d.thresholdDb);
    }
    w.AddTable(du);
    bt::BankHeaderInfo hi;
    hi.bankId = bankId;
    hi.languageId = languageId;
    if (languageId != 0u) hi.flags |= bf::kFileLanguageBank;
    hi.dependencies = std::move(deps);
    return w.Finish(hi);
  }
};

// Mono PCMF data helpers.
inline std::vector<float> Constant(std::size_t frames, float v) { return std::vector<float>(frames, v); }
inline std::vector<float> Sine(std::size_t frames, double hz, double rate, float amp = 1.0f) {
  std::vector<float> d(frames);
  for (std::size_t i = 0; i < frames; ++i) d[i] = static_cast<float>(amp * std::sin(6.283185307179586 * hz * double(i) / rate));
  return d;
}
inline std::vector<float> Impulse(std::size_t frames, std::size_t at = 0, float v = 1.0f) {
  std::vector<float> d(frames, 0.0f);
  if (at < frames) d[at] = v;
  return d;
}

}  // namespace coretest

#endif  // SND_CORE_TEST_TEST_BANK_HPP
