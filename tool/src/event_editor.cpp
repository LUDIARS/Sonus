// Event editor model (event_editor.hpp).
#include "snd/tool/event_editor.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <set>

#include "snd/buildtool/wav.hpp"

namespace snd::tool {
namespace {

using json::Value;
constexpr double kInf = std::numeric_limits<double>::infinity();

const char* const kSeqNames[] = {"polyphonic", "random", "randomNoRepeat", "sequential",
                                 "shuffle",    "switch", "comboSequential", "layerCrossfade"};
const char* const kActionNames[] = {"play", "stop", "setParameter", "applySnapshot"};
const char* const kVirtualNames[] = {"continue", "restart", "never"};

const char* ClipKey(ClipNumber n) {
  switch (n) {
    case ClipNumber::StartMs: return "startMs";
    case ClipNumber::Weight: return "weight";
    case ClipNumber::VolumeDb: return "volumeDb";
    case ClipNumber::PitchCents: return "pitchCents";
    case ClipNumber::FadeInMs: return "fadeInMs";
    case ClipNumber::FadeOutMs: return "fadeOutMs";
    case ClipNumber::LoopCount: return "loopCount";
  }
  return "";
}
double ClipDefault(ClipNumber n) {
  return n == ClipNumber::Weight ? 1.0 : n == ClipNumber::LoopCount ? 1.0 : 0.0;
}
const char* RandomKey(RandomField f) {
  switch (f) {
    case RandomField::VolumeDb: return "volumeDb";
    case RandomField::PitchCents: return "pitchCents";
    case RandomField::Pan: return "pan";
    case RandomField::StartMs: return "startMs";
    case RandomField::FilterHz: return "filterHz";
  }
  return "";
}
const char* EventKey(EventNumber n) {
  switch (n) {
    case EventNumber::MaxInstances: return "maxInstances";
    case EventNumber::RetriggerMs: return "retriggerMs";
    case EventNumber::FadeOutMs: return "fadeOutMs";
    case EventNumber::ComboWindowMs: return "comboWindowMs";
    case EventNumber::MaxDistance: return "maxDistance";
    case EventNumber::Priority: return "priority";
  }
  return "";
}
const char* FlagKey(EventFlag f) {
  switch (f) {
    case EventFlag::Is3D: return "is3D";
    case EventFlag::IgnorePause: return "ignorePause";
    case EventFlag::Stinger: return "stinger";
  }
  return "";
}

bool IsInteger(double v) { return std::isfinite(v) && v == std::floor(v); }

std::string TrackPath(std::uint32_t t) { return "tracks[" + std::to_string(t) + "]"; }

Value MakeSynth(const SynthDesc& s) {
  Value v = json::MakeObject();
  json::Add(v, "kind", String(s.kind));
  if (s.kind == "oscillator" || s.kind == "wavetable") {
    json::Add(v, "waveform", String(s.waveform));
    json::Add(v, "frequencyHz", Number(s.frequencyHz));
    if (s.waveform == "pulse") json::Add(v, "duty", Number(s.duty));
  }
  if (s.kind == "noise") json::Add(v, "noiseColor", String(s.noiseColor));
  if (!s.asset.empty()) json::Add(v, "asset", String(s.asset));
  if (s.channels != 1u) json::Add(v, "channels", Number(s.channels));
  if (s.kind == "patch") json::Add(v, "patch", String(s.patch));
  return v;
}

}  // namespace

const char* SequenceName(SequenceKind k) { return kSeqNames[static_cast<int>(k)]; }
bool ParseSequence(const std::string& s, SequenceKind* out) {
  for (int i = 0; i < 8; ++i) {
    if (s == kSeqNames[i]) {
      *out = static_cast<SequenceKind>(i);
      return true;
    }
  }
  return false;
}

SequenceUse UseOf(SequenceKind k) {
  SequenceUse u;
  u.weight = k == SequenceKind::Random || k == SequenceKind::RandomNoRepeat || k == SequenceKind::Shuffle;
  u.switchValue = k == SequenceKind::Switch;
  u.layer = k == SequenceKind::LayerCrossfade;
  u.comboWindowMs = k == SequenceKind::ComboSequential;
  u.allClipsPlay = k == SequenceKind::Polyphonic || k == SequenceKind::LayerCrossfade;
  return u;
}

bool ReadAssetLength(const ProjectDocument& doc, const DocObject& asset, AssetLength* out) {
  *out = AssetLength{};
  const std::string file = StringAt(asset.doc, "file");
  std::vector<std::uint8_t> bytes;
  if (file.empty() || !doc.ReadBinaryFile(file, &bytes)) return false;
  buildtool::PcmData pcm;
  std::string err;
  if (!buildtool::ReadWav(bytes, &pcm, &err) || pcm.sampleRate == 0) return false;
  out->frames = static_cast<double>(pcm.frames);
  out->sampleRate = pcm.sampleRate;
  if (pcm.hasLoop) {
    out->loopStart = static_cast<double>(pcm.loopStart);
    out->loopEnd = static_cast<double>(pcm.loopEnd);
  }
  if (const Value* lp = asset.doc.Find("loop"); lp != nullptr && lp->IsObject()) {
    out->loopStart = NumberAt(*lp, "start", 0);
    out->loopEnd = NumberAt(*lp, "end", out->frames);
  }
  if (out->loopEnd > out->frames) out->loopEnd = out->frames;
  if (out->loopEnd <= out->loopStart) out->loopEnd = 0;
  return true;
}

EventEditor::EventEditor(ProjectDocument& doc, std::string guid) : doc_(doc), guid_(NormalizeGuid(guid)) {}

const Value* EventEditor::event() const {
  const DocObject* o = doc_.Find(guid_);
  return (o != nullptr && o->kind() == "event") ? &o->doc : nullptr;
}
bool EventEditor::valid() const { return event() != nullptr; }

bool EventEditor::ValidRef(const std::string& guid, const char* kind) const {
  const DocObject* o = doc_.Find(guid);
  return o != nullptr && o->kind() == kind;
}

// True if `nestedGuid` is this event or reaches it through nested clips.
bool EventEditor::WouldCycle(const std::string& nestedGuid) const {
  std::vector<std::string> work{NormalizeGuid(nestedGuid)};
  std::set<std::string> seen;
  while (!work.empty()) {
    const std::string g = work.back();
    work.pop_back();
    if (g == guid_) return true;
    if (!seen.insert(g).second) continue;
    const DocObject* o = doc_.Find(g);
    if (o == nullptr) continue;
    const Value* tracks = o->doc.Find("tracks");
    if (tracks == nullptr || !tracks->IsArray()) continue;
    for (const Value& tr : tracks->items) {
      const Value* clips = tr.Find("clips");
      if (clips == nullptr || !clips->IsArray()) continue;
      for (const Value& c : clips->items) {
        const Value* e = c.Find("event");
        if (e != nullptr && e->IsString()) {
          const DocObject* n = doc_.Resolve("event", e->text);
          if (n != nullptr) work.push_back(n->guid);
        }
      }
    }
  }
  return false;
}

std::string EventEditor::ClipPath(std::uint32_t t, std::uint32_t c) const {
  return TrackPath(t) + ".clips[" + std::to_string(c) + "]";
}

// --- event members ------------------------------------------------------------------------
SequenceKind EventEditor::sequence() const {
  SequenceKind k = SequenceKind::Polyphonic;
  if (const Value* e = event()) ParseSequence(StringAt(*e, "sequence", "polyphonic"), &k);
  return k;
}
bool EventEditor::SetSequence(SequenceKind k) {
  if (!valid()) return false;
  if (k == SequenceKind::Polyphonic) return event()->Find("sequence") == nullptr || doc_.Erase(guid_, "sequence");
  return doc_.Set(guid_, "sequence", String(SequenceName(k)));
}

static bool SetRef(ProjectDocument& doc, const std::string& guid, const char* member, const std::string& ref,
                   const char* kind) {
  if (ref.empty()) {  // D-177b: already absent is success
    const DocObject* self = doc.Find(guid);
    return self != nullptr && (FindPath(self->doc, member) == nullptr || doc.Erase(guid, member));
  }
  const DocObject* o = doc.Find(ref);
  if (o == nullptr || o->kind() != kind) return false;
  return doc.Set(guid, member, String(o->guid));
}
bool EventEditor::SetBus(const std::string& g) { return valid() && SetRef(doc_, guid_, "bus", g, "bus"); }
bool EventEditor::SetCategory(const std::string& g) { return valid() && SetRef(doc_, guid_, "category", g, "category"); }
bool EventEditor::SetLimitGroup(const std::string& g) {
  return valid() && SetRef(doc_, guid_, "limitGroup", g, "voiceLimitGroup");
}

bool EventEditor::SetNumber(EventNumber n, double v, std::uint32_t mergeKey) {
  if (!valid() || !std::isfinite(v) || v < 0) return false;
  if ((n == EventNumber::MaxInstances || n == EventNumber::Priority) && !IsInteger(v)) return false;
  if (n == EventNumber::Priority && v > 255) return false;
  if (n == EventNumber::MaxInstances && v > 4294967295.0) return false;
  return doc_.Set(guid_, EventKey(n), snd::tool::Number(v), mergeKey);
}
double EventEditor::NumberOf(EventNumber n) const {
  const Value* e = event();
  return e != nullptr ? NumberAt(*e, EventKey(n), n == EventNumber::Priority ? 128.0 : 0.0) : 0.0;
}
bool EventEditor::SetFlag(EventFlag f, bool on) {
  if (!valid()) return false;
  if (!on && event()->Find(FlagKey(f)) != nullptr) return doc_.Erase(guid_, FlagKey(f));
  if (!on) return true;  // already off
  return doc_.Set(guid_, FlagKey(f), Bool(true));
}
bool EventEditor::Flag(EventFlag f) const {
  const Value* e = event();
  return e != nullptr && BoolAt(*e, FlagKey(f), false);
}
bool EventEditor::SetCone(double innerDeg, double outerDeg, double outerDb) {
  if (!valid() || !std::isfinite(innerDeg) || !std::isfinite(outerDeg) || !std::isfinite(outerDb)) return false;
  if (innerDeg < 0 || innerDeg > outerDeg || outerDeg > 360) return false;
  Value cone = json::MakeObject();
  json::Add(cone, "innerDeg", snd::tool::Number(innerDeg));
  json::Add(cone, "outerDeg", snd::tool::Number(outerDeg));
  json::Add(cone, "outerDb", snd::tool::Number(outerDb));
  return doc_.Set(guid_, "cone", cone);
}
bool EventEditor::SetVirtualReturn(VirtualReturn r) {
  return valid() && doc_.Set(guid_, "virtualReturn", String(kVirtualNames[static_cast<int>(r)]));
}

std::vector<std::string> EventEditor::UnusedMembers() const {
  std::vector<std::string> out;
  const Value* e = event();
  if (e == nullptr) return out;
  const SequenceUse u = UseOf(sequence());
  if (!u.comboWindowMs && e->Find("comboWindowMs") != nullptr) out.push_back("comboWindowMs");
  const Value* tracks = e->Find("tracks");
  if (tracks == nullptr || !tracks->IsArray()) return out;
  for (std::size_t t = 0; t < tracks->items.size(); ++t) {
    const Value& tr = tracks->items[t];
    if (!u.layer && tr.Find("layerParameter") != nullptr) out.push_back(TrackPath(static_cast<std::uint32_t>(t)) + ".layerParameter");
    const Value* clips = tr.Find("clips");
    if (clips == nullptr || !clips->IsArray()) continue;
    for (std::size_t c = 0; c < clips->items.size(); ++c) {
      const std::string p = ClipPath(static_cast<std::uint32_t>(t), static_cast<std::uint32_t>(c));
      const Value& cl = clips->items[c];
      if (!u.weight && cl.Find("weight") != nullptr) out.push_back(p + ".weight");
      if (!u.switchValue && cl.Find("switchValue") != nullptr) out.push_back(p + ".switchValue");
      if (!u.layer && cl.Find("layer") != nullptr) out.push_back(p + ".layer");
    }
  }
  return out;
}

// --- tracks -------------------------------------------------------------------------------
std::uint32_t EventEditor::TrackCount() const {
  const Value* e = event();
  const Value* t = e != nullptr ? e->Find("tracks") : nullptr;
  return t != nullptr && t->IsArray() ? static_cast<std::uint32_t>(t->items.size()) : 0u;
}
bool EventEditor::AddTrack(std::uint32_t at) {
  if (!valid() || at > TrackCount()) return false;
  Value tr = json::MakeObject();
  json::Add(tr, "clips", json::MakeArray());
  return doc_.Insert(guid_, "tracks", at, tr);
}
bool EventEditor::RemoveTrack(std::uint32_t t) { return t < TrackCount() && doc_.Erase(guid_, TrackPath(t)); }
bool EventEditor::MoveTrack(std::uint32_t from, std::uint32_t to) { return doc_.MoveElement(guid_, "tracks", from, to); }
bool EventEditor::SetTrackBus(std::uint32_t t, const std::string& g) {
  return t < TrackCount() && SetRef(doc_, guid_, (TrackPath(t) + ".bus").c_str(), g, "bus");
}
bool EventEditor::SetTrackLayerParameter(std::uint32_t t, const std::string& g) {
  return t < TrackCount() && SetRef(doc_, guid_, (TrackPath(t) + ".layerParameter").c_str(), g, "parameter");
}
bool EventEditor::SetTrackFlags(std::uint32_t t, std::uint32_t flags) {
  return t < TrackCount() && doc_.Set(guid_, TrackPath(t) + ".flags", snd::tool::Number(flags));
}

// --- clips --------------------------------------------------------------------------------
std::uint32_t EventEditor::ClipCount(std::uint32_t t) const {
  const Value* e = event();
  const Value* c = e != nullptr ? FindPath(*e, TrackPath(t) + ".clips") : nullptr;
  return c != nullptr && c->IsArray() ? static_cast<std::uint32_t>(c->items.size()) : 0u;
}
bool EventEditor::AddClip(std::uint32_t t, Value clip) {
  if (t >= TrackCount()) return false;
  return doc_.Insert(guid_, TrackPath(t) + ".clips", ClipCount(t), std::move(clip));
}
bool EventEditor::AddAssetClip(std::uint32_t t, const std::string& assetGuid, double startMs) {
  if (!ValidRef(assetGuid, "asset") || !std::isfinite(startMs) || startMs < 0) return false;
  Value c = json::MakeObject();
  json::Add(c, "asset", String(NormalizeGuid(assetGuid)));
  if (startMs != 0) json::Add(c, "startMs", snd::tool::Number(startMs));
  return AddClip(t, std::move(c));
}
bool EventEditor::AddEventClip(std::uint32_t t, const std::string& eventGuid, double startMs) {
  if (!ValidRef(eventGuid, "event") || WouldCycle(eventGuid) || !std::isfinite(startMs) || startMs < 0) return false;
  Value c = json::MakeObject();
  json::Add(c, "event", String(NormalizeGuid(eventGuid)));
  if (startMs != 0) json::Add(c, "startMs", snd::tool::Number(startMs));
  return AddClip(t, std::move(c));
}
bool EventEditor::AddSynthClip(std::uint32_t t, const SynthDesc& s, double startMs) {
  if (!std::isfinite(startMs) || startMs < 0) return false;
  static const char* const kinds[] = {"oscillator", "noise", "granular", "patch", "wavetable"};
  bool ok = false;
  for (const char* k : kinds) ok = ok || s.kind == k;
  if (!ok) return false;
  if (s.kind == "patch" && !ValidRef(s.patch, "patch")) return false;
  if ((s.kind == "granular" || s.kind == "wavetable") && !ValidRef(s.asset, "asset")) return false;
  if (!std::isfinite(s.frequencyHz) || s.frequencyHz <= 0 || s.channels == 0u || s.channels > 8u) return false;
  Value c = json::MakeObject();
  SynthDesc n = s;
  n.asset = NormalizeGuid(s.asset);
  n.patch = NormalizeGuid(s.patch);
  json::Add(c, "synth", MakeSynth(n));
  if (startMs != 0) json::Add(c, "startMs", snd::tool::Number(startMs));
  return AddClip(t, std::move(c));
}
bool EventEditor::SetSynth(std::uint32_t t, std::uint32_t c, const SynthDesc& s) {
  if (c >= ClipCount(t)) return false;
  const std::string p = ClipPath(t, c);
  if (FindPath(*event(), p + ".synth") == nullptr) return false;  // only synth clips
  if (s.kind == "patch" && !ValidRef(s.patch, "patch")) return false;
  if ((s.kind == "granular" || s.kind == "wavetable") && !ValidRef(s.asset, "asset")) return false;
  SynthDesc n = s;
  n.asset = NormalizeGuid(s.asset);
  n.patch = NormalizeGuid(s.patch);
  return doc_.Set(guid_, p + ".synth", MakeSynth(n));
}
bool EventEditor::RemoveClip(std::uint32_t t, std::uint32_t c) {
  return c < ClipCount(t) && doc_.Erase(guid_, ClipPath(t, c));
}
bool EventEditor::MoveClip(std::uint32_t t, std::uint32_t c, double startMs, std::uint32_t mergeKey) {
  return SetClipNumber(t, c, ClipNumber::StartMs, startMs, mergeKey);
}
bool EventEditor::MoveClipToTrack(std::uint32_t t, std::uint32_t c, std::uint32_t t2, double startMs) {
  if (c >= ClipCount(t) || t2 >= TrackCount() || !std::isfinite(startMs) || startMs < 0) return false;
  if (t == t2) return MoveClip(t, c, startMs);
  return doc_.Modify(guid_, [&](Value& e) {
    Value* from = FindPath(e, TrackPath(t) + ".clips");
    Value* to = FindPath(e, TrackPath(t2));
    if (from == nullptr || to == nullptr || !to->IsObject()) return false;
    Value clip = from->items[c];
    from->items.erase(from->items.begin() + static_cast<std::ptrdiff_t>(c));
    SetMember(clip, "startMs", snd::tool::Number(startMs), SpecMemberOrder("event", "tracks[0].clips[0]"));
    Value* clips = FindPath(*to, "clips");
    if (clips == nullptr) {
      SetMember(*to, "clips", json::MakeArray(), SpecMemberOrder("event", "tracks[0]"));
      clips = FindPath(*to, "clips");
    }
    if (!clips->IsArray()) return false;
    clips->items.push_back(std::move(clip));
    return true;
  });
}
bool EventEditor::SetClipNumber(std::uint32_t t, std::uint32_t c, ClipNumber n, double v, std::uint32_t mergeKey) {
  if (c >= ClipCount(t) || !std::isfinite(v)) return false;
  if (v < 0 && n != ClipNumber::VolumeDb && n != ClipNumber::PitchCents) return false;
  if (n == ClipNumber::LoopCount && (!IsInteger(v) || v > 4294967295.0)) return false;
  return doc_.Set(guid_, ClipPath(t, c) + "." + ClipKey(n), snd::tool::Number(v), mergeKey);
}
double EventEditor::ClipNumberOf(std::uint32_t t, std::uint32_t c, ClipNumber n) const {
  const Value* e = event();
  return e != nullptr ? NumberAt(*e, ClipPath(t, c) + "." + ClipKey(n), ClipDefault(n)) : 0.0;
}
bool EventEditor::SetClipRandom(std::uint32_t t, std::uint32_t c, RandomField f, double width, std::uint32_t mergeKey) {
  if (c >= ClipCount(t) || !std::isfinite(width) || width < 0) return false;
  if (f == RandomField::Pan && width > 2) return false;  // pan spans -1 .. 1
  return doc_.Set(guid_, ClipPath(t, c) + ".random." + RandomKey(f), snd::tool::Number(width), mergeKey);
}
double EventEditor::ClipRandom(std::uint32_t t, std::uint32_t c, RandomField f) const {
  const Value* e = event();
  return e != nullptr ? NumberAt(*e, ClipPath(t, c) + ".random." + RandomKey(f), 0.0) : 0.0;
}
bool EventEditor::SetSwitchValue(std::uint32_t t, std::uint32_t c, const std::string& name) {
  if (c >= ClipCount(t) || name.empty()) return false;
  return doc_.Set(guid_, ClipPath(t, c) + ".switchValue", String(name));
}
bool EventEditor::SetSwitchValue(std::uint32_t t, std::uint32_t c, std::uint32_t value) {
  if (c >= ClipCount(t)) return false;
  return doc_.Set(guid_, ClipPath(t, c) + ".switchValue", snd::tool::Number(value));
}
bool EventEditor::SetLayer(std::uint32_t t, std::uint32_t c, double minValue, double maxValue) {
  if (c >= ClipCount(t) || !std::isfinite(minValue) || !std::isfinite(maxValue) || minValue > maxValue) return false;
  Value l = json::MakeArray();
  l.items.push_back(snd::tool::Number(minValue));
  l.items.push_back(snd::tool::Number(maxValue));
  return doc_.Set(guid_, ClipPath(t, c) + ".layer", l);
}

// --- actions ------------------------------------------------------------------------------
std::uint32_t EventEditor::ActionCount(std::uint32_t t) const {
  const Value* e = event();
  const Value* a = e != nullptr ? FindPath(*e, TrackPath(t) + ".actions") : nullptr;
  return a != nullptr && a->IsArray() ? static_cast<std::uint32_t>(a->items.size()) : 0u;
}
static bool MakeAction(const ProjectDocument& doc, const ActionDesc& a, Value* out) {
  if (!std::isfinite(a.timeMs) || a.timeMs < 0 || !std::isfinite(a.value)) return false;
  const char* kind = a.action == ActionKind::SetParameter    ? "parameter"
                     : a.action == ActionKind::ApplySnapshot ? "snapshot"
                                                             : "event";
  const DocObject* t = doc.Find(a.target);
  if (t == nullptr || t->kind() != kind) return false;
  *out = json::MakeObject();
  json::Add(*out, "timeMs", Number(a.timeMs));
  json::Add(*out, "action", String(kActionNames[static_cast<int>(a.action)]));
  json::Add(*out, "target", String(t->guid));
  if (a.action == ActionKind::SetParameter || a.action == ActionKind::ApplySnapshot) json::Add(*out, "value", Number(a.value));
  return true;
}
bool EventEditor::AddAction(std::uint32_t t, const ActionDesc& a) {
  Value v;
  if (t >= TrackCount() || !MakeAction(doc_, a, &v)) return false;
  return doc_.Insert(guid_, TrackPath(t) + ".actions", ActionCount(t), v);
}
bool EventEditor::SetAction(std::uint32_t t, std::uint32_t i, const ActionDesc& a) {
  Value v;
  if (i >= ActionCount(t) || !MakeAction(doc_, a, &v)) return false;
  const std::string p = TrackPath(t) + ".actions[" + std::to_string(i) + "]";
  // Keep members the tool does not know (notes ...).
  Value merged = *FindPath(*event(), p);
  for (const json::Member& m : v.members) SetMember(merged, m.first, m.second, SpecMemberOrder("event", "tracks[0].actions[0]"));
  if (a.action != ActionKind::SetParameter && a.action != ActionKind::ApplySnapshot) {
    for (std::size_t k = 0; k < merged.members.size(); ++k) {
      if (merged.members[k].first == "value") merged.members.erase(merged.members.begin() + static_cast<std::ptrdiff_t>(k));
    }
  }
  return doc_.Set(guid_, p, merged);
}
bool EventEditor::RemoveAction(std::uint32_t t, std::uint32_t i) {
  return i < ActionCount(t) && doc_.Erase(guid_, TrackPath(t) + ".actions[" + std::to_string(i) + "]");
}
ActionDesc EventEditor::Action(std::uint32_t t, std::uint32_t i) const {
  ActionDesc a;
  const Value* e = event();
  if (e == nullptr) return a;
  const std::string p = TrackPath(t) + ".actions[" + std::to_string(i) + "]";
  a.timeMs = NumberAt(*e, p + ".timeMs", 0);
  const std::string k = StringAt(*e, p + ".action", "play");
  for (int n = 0; n < 4; ++n) {
    if (k == kActionNames[n]) a.action = static_cast<ActionKind>(n);
  }
  const char* kind = a.action == ActionKind::SetParameter ? "parameter" : a.action == ActionKind::ApplySnapshot ? "snapshot" : "event";
  const DocObject* t2 = doc_.Resolve(kind, StringAt(*e, p + ".target"));
  a.target = t2 != nullptr ? t2->guid : StringAt(*e, p + ".target");
  a.value = NumberAt(*e, p + ".value", 0);
  return a;
}

// --- timeline -----------------------------------------------------------------------------
namespace {

struct LengthCtx {
  const ProjectDocument& doc;
  std::set<std::string> stack;
};
Timeline Compute(LengthCtx& ctx, const DocObject& ev);

// One clip: pass length and total.
void ClipLength(LengthCtx& ctx, const Value& c, TimelineClip* out) {
  const double cents = NumberAt(c, "pitchCents", 0);
  const double speed = std::exp2(cents / 1200.0);
  double loops = NumberAt(c, "loopCount", 1);
  if (!(loops >= 0)) loops = 1;
  if (const Value* a = c.Find("asset"); a != nullptr && a->IsString()) {
    out->source = "asset";
    const DocObject* asset = ctx.doc.Resolve("asset", a->text);
    AssetLength len;
    if (asset == nullptr || !ReadAssetLength(ctx.doc, *asset, &len)) {
      out->sourceGuid = asset != nullptr ? asset->guid : std::string();
      out->lengthKnown = false;
      out->infinite = loops == 0;
      out->endMs = out->startMs;
      return;
    }
    out->sourceGuid = asset->guid;
    const double frameMs = 1000.0 / len.sampleRate / speed;
    out->passMs = len.frames * frameMs;
    if (loops == 0) {
      out->infinite = true;
      out->endMs = kInf;
    } else if (len.loopEnd > 0) {
      // Intro up to the loop end, the loop region again (loops - 1) times, then the tail.
      const double region = (len.loopEnd - len.loopStart) * frameMs;
      out->endMs = out->startMs + out->passMs + region * (loops - 1);
    } else {
      out->endMs = out->startMs + out->passMs * loops;
    }
    return;
  }
  if (const Value* e = c.Find("event"); e != nullptr && e->IsString()) {
    out->source = "event";
    const DocObject* nested = ctx.doc.Resolve("event", e->text);
    if (nested == nullptr || ctx.stack.count(nested->guid) != 0u) {
      out->lengthKnown = false;
      out->endMs = out->startMs;
      if (nested != nullptr) out->sourceGuid = nested->guid;
      return;
    }
    out->sourceGuid = nested->guid;
    const Timeline inner = Compute(ctx, *nested);
    out->passMs = inner.lengthMs / speed;
    out->infinite = inner.infinite || loops == 0;
    out->endMs = out->infinite ? kInf : out->startMs + out->passMs * loops;
    return;
  }
  if (c.Find("synth") != nullptr) {
    out->source = "synth";
    out->infinite = true;  // a synthesised source sounds until it is stopped
    out->endMs = kInf;
    return;
  }
  out->lengthKnown = false;
  out->endMs = out->startMs;
}

Timeline Compute(LengthCtx& ctx, const DocObject& ev) {
  Timeline tl;
  ctx.stack.insert(ev.guid);
  SequenceKind k = SequenceKind::Polyphonic;
  ParseSequence(StringAt(ev.doc, "sequence", "polyphonic"), &k);
  tl.oneClipPerPlay = !UseOf(k).allClipsPlay;
  const Value* tracks = ev.doc.Find("tracks");
  if (tracks != nullptr && tracks->IsArray()) {
    for (std::size_t t = 0; t < tracks->items.size(); ++t) {
      const Value& tr = tracks->items[t];
      const std::size_t first = tl.clips.size();
      if (const Value* clips = tr.Find("clips"); clips != nullptr && clips->IsArray()) {
        for (std::size_t c = 0; c < clips->items.size(); ++c) {
          const Value& cv = clips->items[c];
          TimelineClip tc;
          tc.track = static_cast<std::uint32_t>(t);
          tc.clip = static_cast<std::uint32_t>(c);
          tc.startMs = std::max(0.0, NumberAt(cv, "startMs", 0));
          tc.fadeInMs = NumberAt(cv, "fadeInMs", 0);
          tc.fadeOutMs = NumberAt(cv, "fadeOutMs", 0);
          ClipLength(ctx, cv, &tc);
          if (tc.infinite) {
            tl.infinite = true;
          } else {
            tl.lengthMs = std::max(tl.lengthMs, tc.endMs);
          }
          tl.clips.push_back(tc);
        }
      }
      // Overlaps on this track.
      for (std::size_t a = first; a < tl.clips.size(); ++a) {
        for (std::size_t b = a + 1; b < tl.clips.size(); ++b) {
          const TimelineClip& x = tl.clips[a];
          const TimelineClip& y = tl.clips[b];
          const double from = std::max(x.startMs, y.startMs);
          const double to = std::min(x.endMs, y.endMs);
          if (to > from) tl.overlaps.push_back(TimelineOverlap{static_cast<std::uint32_t>(t), x.clip, y.clip, from, to});
        }
      }
      if (const Value* acts = tr.Find("actions"); acts != nullptr && acts->IsArray()) {
        for (std::size_t i = 0; i < acts->items.size(); ++i) {
          const double ms = NumberAt(acts->items[i], "timeMs", 0);
          tl.actions.push_back(TimelineAction{static_cast<std::uint32_t>(t), static_cast<std::uint32_t>(i), ms});
          tl.lengthMs = std::max(tl.lengthMs, ms);
        }
      }
    }
  }
  ctx.stack.erase(ev.guid);
  return tl;
}

}  // namespace

Timeline EventEditor::ComputeTimeline() const {
  const DocObject* o = doc_.Find(guid_);
  if (o == nullptr) return Timeline{};
  LengthCtx ctx{doc_, {}};
  return Compute(ctx, *o);
}

}  // namespace snd::tool
