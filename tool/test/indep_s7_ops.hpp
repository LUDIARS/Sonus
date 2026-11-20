// Random edit sequences across every editor (property-based tests of group M, fixed seed).
#ifndef SND_TOOL_TEST_INDEP_S7_OPS_HPP
#define SND_TOOL_TEST_INDEP_S7_OPS_HPP

#include <cmath>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

#include "indep_s7_fixture.hpp"
#include "snd/tool/curve_editor.hpp"
#include "snd/tool/event_editor.hpp"
#include "snd/tool/material_editor.hpp"
#include "snd/tool/mixer_editor.hpp"
#include "snd/tool/music_editor.hpp"
#include "snd/tool/patch_editor.hpp"

namespace indep_s7 {

struct Rng {
  std::uint64_t s;
  explicit Rng(std::uint64_t seed) : s(seed ? seed : 1u) {}
  std::uint64_t Next() {
    s ^= s << 13;
    s ^= s >> 7;
    s ^= s << 17;
    return s;
  }
  std::uint32_t Below(std::uint32_t n) { return n == 0 ? 0u : static_cast<std::uint32_t>(Next() % n); }
  double Value() {
    static const double kValues[] = {0.0, 1.0, -1.0, 0.1, 1e-7, 12.5, -3.0, 100.0, 255.0, 256.0, 1e9, -0.0,
                                     std::numeric_limits<double>::quiet_NaN(), std::numeric_limits<double>::infinity(),
                                     0.5, 2.0, 3.0, 7.0, 360.0, 48000.0};
    if (Below(3) == 0) return static_cast<double>(Below(2000)) / 8.0;
    return kValues[Below(sizeof kValues / sizeof kValues[0])];
  }
  double Finite() {
    double v;
    do {
      v = Value();
    } while (!std::isfinite(v));
    return v;
  }
};

struct OpLog {
  std::uint32_t applied = 0;
  std::uint32_t refused = 0;
  std::uint32_t undos = 0;
  std::uint32_t redos = 0;
};

// One random operation. Returns the operation's result (false: refused). `name` gets a
// short label for messages. `allowUndo`: may pick Undo / Redo.
inline bool RandomOp(snd::tool::ProjectDocument& doc, Rng& r, std::string* name, bool allowUndo, std::uint32_t serial) {
  using namespace snd::tool;
  const std::string n = std::to_string(serial);
  const std::uint32_t mk = r.Below(3) == 0 ? 7u : 0u;
  switch (r.Below(allowUndo ? 26u : 24u)) {
    case 0: {
      *name = "Set bus volumeDb";
      return doc.SetNumber(kSfx, "volumeDb", r.Finite(), mk);
    }
    case 1: {
      *name = "Event SetClipNumber startMs";
      EventEditor e(doc, kShot);
      return e.SetClipNumber(0, r.Below(e.ClipCount(0) + 1), ClipNumber::StartMs, r.Value(), mk);
    }
    case 2: {
      *name = "Event AddAssetClip";
      EventEditor e(doc, kShot);
      return e.AddAssetClip(r.Below(e.TrackCount() + 1), kTone, r.Value());
    }
    case 3: {
      *name = "Event RemoveClip";
      EventEditor e(doc, kShot);
      return e.RemoveClip(0, r.Below(e.ClipCount(0) + 1));
    }
    case 4: {
      *name = "Mixer SetVolume";
      MixerEditor m(doc);
      return m.SetVolume(kMusicBus, r.Value(), mk);
    }
    case 5: {
      *name = "Mixer AddBus";
      MixerEditor m(doc);
      return !m.AddBus("Bus" + n, r.Below(2) ? kMaster : kSfx).empty();
    }
    case 6: {
      *name = "Remove bus";
      auto buses = doc.OfKind("bus");
      if (buses.empty()) return false;
      const std::string g = buses[r.Below(static_cast<std::uint32_t>(buses.size()))]->guid;
      return doc.Remove(g, false);
    }
    case 7: {
      *name = "Rename";
      static const char* const kTargets[] = {kHit, kCatUi, kSpeed, kSegBridge};
      return doc.Rename(kTargets[r.Below(4)], "Renamed" + n, RenameOptions{r.Below(2) == 0, r.Below(2) == 0});
    }
    case 8: {
      *name = "Material AddMaterial";
      MaterialEditor m(doc, kPhys);
      return m.AddMaterial("mat" + n, r.Below(static_cast<std::uint32_t>(m.Materials().size()) + 2));
    }
    case 9: {
      *name = "Material RemoveMaterial";
      MaterialEditor m(doc, kPhys);
      auto mats = m.Materials();
      if (mats.empty()) return false;
      return m.RemoveMaterial(mats[r.Below(static_cast<std::uint32_t>(mats.size()))]);
    }
    case 10: {
      *name = "Patch MoveNode";
      PatchEditor p(doc, kPatch);
      return p.MoveNode("osc", static_cast<float>(r.Finite()), static_cast<float>(r.Finite()), mk);
    }
    case 11: {
      *name = "Music AddRule";
      MusicEditor m(doc, kBgm);
      TransitionRule rule;
      rule.from = r.Below(2) ? "Battle" : "*";
      rule.to = r.Below(2) ? "Explore" : "Battle";
      rule.timing = static_cast<Timing>(r.Below(7));
      rule.fadeOutBeats = r.Value();
      return m.AddRule(rule, r.Below(static_cast<std::uint32_t>(m.Rules().size()) + 2));
    }
    case 12: {
      *name = "Music MoveRule";
      MusicEditor m(doc, kBgm);
      const auto c = static_cast<std::uint32_t>(m.Rules().size());
      return m.MoveRule(r.Below(c + 1), r.Below(c + 1));
    }
    case 13: {
      *name = "Curve AddPoint";
      CurveEditor c(doc, kShot, "curves[0]");
      return c.AddPoint(r.Value(), r.Value()) >= 0;
    }
    case 14: {
      *name = "Set notes";
      const auto& objs = doc.objects();
      if (objs.empty()) return false;
      const std::string g = objs[r.Below(static_cast<std::uint32_t>(objs.size()))].guid;
      return doc.Set(g, "notes", String("note " + n), mk);
    }
    case 15: {
      *name = "Duplicate";
      return !doc.Duplicate(kHit, "Copy" + n).empty();
    }
    case 16: {
      *name = "Insert track";
      json::Value t = json::MakeObject();
      json::Add(t, "clips", json::MakeArray());
      EventEditor e(doc, kShot);
      return doc.Insert(kShot, "tracks", r.Below(e.TrackCount() + 2), t);
    }
    case 17: {
      *name = "Erase member";
      static const char* const kPaths[] = {"fadeOutMs", "retriggerMs", "tracks[0].clips[0].random", "curves[0]", "priority"};
      return doc.Erase(kShot, kPaths[r.Below(5)]);
    }
    case 18: {
      *name = "Group of two";
      doc.BeginGroup();
      MixerEditor m(doc);
      const bool a = m.SetVolume(kSfx, r.Finite());
      const bool b = doc.SetNumber(kHit, "priority", static_cast<double>(r.Below(256)));
      doc.EndGroup();
      return a || b;
    }
    case 19: {
      *name = "Parameter SetRampRate";
      ParameterEditor p(doc, kSpeed);
      return p.SetRampRate(r.Value());
    }
    case 20: {
      *name = "Segment AddMarker";
      SegmentEditor s(doc, kSegA);
      MarkerDesc md;
      md.name = "mk" + n;
      md.frame = r.Below(70000);
      return s.AddMarker(md);
    }
    case 21: {
      *name = "Create category";
      json::Value init = json::MakeObject();
      json::Add(init, "parent", json::MakeString(kCatUi));
      return !doc.Create("category", "Cat" + n, &init).empty();
    }
    case 22: {
      *name = "MoveElement clips";
      EventEditor e(doc, kShot);
      const auto c = e.ClipCount(0);
      return doc.MoveElement(kShot, "tracks[0].clips", r.Below(c + 1), r.Below(c + 1));
    }
    case 23: {
      *name = "Material SetPair";
      MaterialEditor m(doc, kPhys);
      auto mats = m.Materials();
      if (mats.size() < 2) return false;
      const auto a = r.Below(static_cast<std::uint32_t>(mats.size()));
      const auto b = r.Below(static_cast<std::uint32_t>(mats.size()));
      return m.SetPair(mats[a], mats[b], r.Below(3) == 0 ? std::string() : std::string(kHit));
    }
    case 24:
      *name = "Undo";
      return doc.Undo();
    default:
      *name = "Redo";
      return doc.Redo();
  }
}

}  // namespace indep_s7

#endif  // SND_TOOL_TEST_INDEP_S7_OPS_HPP
