// The editable project (document.hpp).
#include "snd/tool/document.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <set>
#include <system_error>

#include "snd/platform/clock.hpp"

namespace snd::tool {
namespace {

namespace fs = std::filesystem;
using json::Value;

// --- spec member orders (project-format.md tables, buildtool/src/project.cpp schema) ----
#define SND_ORDER(name, ...) const char* const name[] = {__VA_ARGS__, nullptr};
#define SND_COMMON_KEYS "type", "guid", "name", "aliases", "bank", "notes"
SND_ORDER(kProjectO, SND_COMMON_KEYS, "formatVersion", "languages", "platforms")
SND_ORDER(kBankO, SND_COMMON_KEYS, "dependsOn", "language")
SND_ORDER(kBusO, SND_COMMON_KEYS, "parent", "master", "output", "lodBypassable", "meter", "volumeDb", "channels",
          "lodPriority", "sends", "effects")
SND_ORDER(kCategoryO, SND_COMMON_KEYS, "parent", "ignorePause", "muteOnFocusLoss", "volumeDb")
SND_ORDER(kParameterO, SND_COMMON_KEYS, "kind", "scope", "min", "max", "default", "rampRate")
SND_ORDER(kSnapshotO, SND_COMMON_KEYS, "values")
SND_ORDER(kGroupO, SND_COMMON_KEYS, "maxVoices", "steal")
SND_ORDER(kDuckingO, SND_COMMON_KEYS, "kind", "source", "target", "amountDb", "attackMs", "releaseMs", "thresholdDb")
SND_ORDER(kAssetO, SND_COMMON_KEYS, "file", "languages", "loadForm", "loop", "convert", "stallPolicy", "virtualReturn",
          "ioPriority", "preloadMs", "seekIntervalMs")
SND_ORDER(kEventO, SND_COMMON_KEYS, "bus", "category", "limitGroup", "priority", "sequence", "is3D", "ignorePause",
          "stinger", "maxInstances", "retriggerMs", "fadeOutMs", "comboWindowMs", "maxDistance", "cone",
          "virtualReturn", "distanceCurves", "curves", "tracks", "music")
SND_ORDER(kSegmentO, SND_COMMON_KEYS, "asset", "bars", "tempo", "entryFrame", "exitFrame", "markers", "stems",
          "entryPreload")
SND_ORDER(kPlaylistO, SND_COMMON_KEYS, "mode", "loopCount", "entries")
SND_ORDER(kMaterialO, SND_COMMON_KEYS, "materials", "single", "pairs", "footsteps", "defaults", "curves")
SND_ORDER(kPatchO, SND_COMMON_KEYS, "nodes", "connections", "output")
#undef SND_COMMON_KEYS
SND_ORDER(kSendO, "bus", "levelDb", "preFader")
SND_ORDER(kEffectO, "type", "params", "blob", "flags")
SND_ORDER(kSnapValueO, "kind", "bus", "value", "effect", "param", "send")
SND_ORDER(kConeO, "innerDeg", "outerDeg", "outerDb")
SND_ORDER(kDistanceO, "volume", "lowpass", "spread", "reverbSend", "blend3D")
SND_ORDER(kCurveO, "parameter", "target", "compose", "points")
SND_ORDER(kTrackO, "bus", "layerParameter", "flags", "clips", "actions")
SND_ORDER(kClipO, "asset", "event", "synth", "startMs", "weight", "volumeDb", "pitchCents", "random", "switchValue",
          "layer", "fadeInMs", "fadeOutMs", "loopCount")
SND_ORDER(kRandomO, "volumeDb", "pitchCents", "pan", "startMs", "filterHz")
SND_ORDER(kSynthO, "kind", "waveform", "frequencyHz", "duty", "noiseColor", "asset", "channels", "patch")
SND_ORDER(kActionO, "timeMs", "action", "target", "value")
SND_ORDER(kMusicO, "initialState", "states", "transitions", "stingers")
SND_ORDER(kStateO, "name", "playlist")
SND_ORDER(kTransitionO, "from", "to", "timing", "timingCount", "fadeOutBeats", "fadeInBeats", "bridge", "start",
          "startMarker", "interrupt", "late", "prepare")
SND_ORDER(kStingerO, "name", "event", "timing", "timingCount", "overlap")
SND_ORDER(kTempoO, "tick", "bpm", "beatsPerBar", "beatUnit")
SND_ORDER(kMarkerO, "name", "frame", "kind")
SND_ORDER(kStemO, "channelFirst", "channelCount", "volumeCurve", "asset")
SND_ORDER(kEntryO, "segment", "weight", "repeat")
SND_ORDER(kContactO, "impact", "slide", "roll", "break")
SND_ORDER(kPairO, "a", "b", "event")
SND_ORDER(kPhysCurveO, "impulseToVelocity", "massToPitchCents", "speedToVolumeDb")
SND_ORDER(kLoopO, "start", "end")
SND_ORDER(kNodeO, "id", "kind", "params", "position")
SND_ORDER(kConnO, "from", "to", "input")
#undef SND_ORDER

struct OrderEntry {
  const char* kind;     // "" = any kind
  const char* pattern;  // path with indices / free keys removed: "tracks[].clips[]"
  const char* const* order;
};
const OrderEntry kOrders[] = {
    {"project", "", kProjectO},
    {"bank", "", kBankO},
    {"bus", "", kBusO},
    {"category", "", kCategoryO},
    {"parameter", "", kParameterO},
    {"snapshot", "", kSnapshotO},
    {"voiceLimitGroup", "", kGroupO},
    {"ducking", "", kDuckingO},
    {"asset", "", kAssetO},
    {"event", "", kEventO},
    {"segment", "", kSegmentO},
    {"playlist", "", kPlaylistO},
    {"materialTable", "", kMaterialO},
    {"patch", "", kPatchO},
    {"bus", "sends[]", kSendO},
    {"bus", "effects[]", kEffectO},
    {"snapshot", "values[]", kSnapValueO},
    {"event", "cone", kConeO},
    {"event", "distanceCurves", kDistanceO},
    {"event", "distanceCurves.*", kCurveO},
    {"event", "curves[]", kCurveO},
    {"event", "tracks[]", kTrackO},
    {"event", "tracks[].clips[]", kClipO},
    {"event", "tracks[].clips[].random", kRandomO},
    {"event", "tracks[].clips[].synth", kSynthO},
    {"event", "tracks[].actions[]", kActionO},
    {"event", "music", kMusicO},
    {"event", "music.states[]", kStateO},
    {"event", "music.transitions[]", kTransitionO},
    {"event", "music.stingers[]", kStingerO},
    {"segment", "tempo[]", kTempoO},
    {"segment", "markers[]", kMarkerO},
    {"segment", "stems[]", kStemO},
    {"segment", "stems[].volumeCurve", kCurveO},
    {"playlist", "entries[]", kEntryO},
    {"materialTable", "single.*", kContactO},
    {"materialTable", "defaults", kContactO},
    {"materialTable", "pairs[]", kPairO},
    {"materialTable", "curves.*", kPhysCurveO},
    {"asset", "loop", kLoopO},
    {"patch", "nodes[]", kNodeO},
    {"patch", "connections[]", kConnO},
};

// Path parsing: "a.b[2].c" -> segments.
struct Seg {
  std::string key;
  std::size_t index = 0;
  bool isIndex = false;
};
bool ParsePath(std::string_view path, std::vector<Seg>* out) {
  out->clear();
  std::size_t i = 0;
  while (i < path.size()) {
    if (path[i] == '.') {
      ++i;
      continue;
    }
    if (path[i] == '[') {
      const std::size_t e = path.find(']', i);
      if (e == std::string_view::npos || e == i + 1) return false;
      std::size_t v = 0;
      for (std::size_t k = i + 1; k < e; ++k) {
        if (path[k] < '0' || path[k] > '9') return false;
        v = v * 10 + static_cast<std::size_t>(path[k] - '0');
      }
      Seg s;
      s.isIndex = true;
      s.index = v;
      out->push_back(s);
      i = e + 1;
      continue;
    }
    std::size_t e = i;
    while (e < path.size() && path[e] != '.' && path[e] != '[') ++e;
    Seg s;
    s.key = std::string(path.substr(i, e - i));
    out->push_back(std::move(s));
    i = e;
  }
  return true;
}
std::string JoinPath(const std::vector<Seg>& segs, std::size_t n) {
  std::string s;
  for (std::size_t i = 0; i < n && i < segs.size(); ++i) {
    if (segs[i].isIndex) {
      s += "[" + std::to_string(segs[i].index) + "]";
    } else {
      if (!s.empty()) s += ".";
      s += segs[i].key;
    }
  }
  return s;
}

Value* Member(Value& o, std::string_view key) {
  if (!o.IsObject()) return nullptr;
  for (json::Member& m : o.members) {
    if (m.first == key) return &m.second;
  }
  return nullptr;
}

bool HasControl(const std::string& s) {
  for (char c : s) {
    if (static_cast<unsigned char>(c) < 0x20 || c == 0x7F) return true;
  }
  return false;
}

std::string Lower(std::string s) {
  for (char& c : s) {
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
  }
  return s;
}

bool ReadText(const fs::path& p, std::string* out) {
  std::FILE* f = std::fopen(p.string().c_str(), "rb");
  if (f == nullptr) return false;
  out->clear();
  char buf[8192];
  for (;;) {
    const std::size_t n = std::fread(buf, 1, sizeof(buf), f);
    out->append(buf, n);
    if (n < sizeof(buf)) break;
  }
  const bool ok = std::ferror(f) == 0;
  std::fclose(f);
  return ok;
}
bool WriteText(const fs::path& p, const std::string& text) {
  std::error_code ec;
  fs::create_directories(p.parent_path(), ec);
  std::FILE* f = std::fopen(p.string().c_str(), "wb");
  if (f == nullptr) return false;
  const bool ok = text.empty() || std::fwrite(text.data(), 1, text.size(), f) == text.size();
  return std::fclose(f) == 0 && ok;
}

// File-name part from a name part (see document.hpp).
std::string SafePart(const std::string& part) {
  std::string s;
  for (char c : part) {
    const unsigned char u = static_cast<unsigned char>(c);
    const bool bad = u < 0x20 || u == 0x7F || c == '<' || c == '>' || c == ':' || c == '"' || c == '\\' || c == '|' ||
                     c == '?' || c == '*' || c == ' ';
    s.push_back(bad ? '_' : c);
  }
  if (s.empty() || s == "." || s == "..") s = "_";
  if (s[0] == '.') s = "_" + s;
  while (!s.empty() && (s.back() == '.')) s.back() = '_';  // Windows drops trailing dots
  // Windows reserved device names (D-176f): the part before the first '.', any case.
  std::string stem = s.substr(0, s.find('.'));
  for (char& c : stem) c = (c >= 'a' && c <= 'z') ? static_cast<char>(c - 'a' + 'A') : c;
  bool reserved = stem == "CON" || stem == "PRN" || stem == "AUX" || stem == "NUL";
  if (stem.size() == 4 && (stem.compare(0, 3, "COM") == 0 || stem.compare(0, 3, "LPT") == 0) && stem[3] >= '1' &&
      stem[3] <= '9') {
    reserved = true;
  }
  if (reserved) s = "_" + s;
  return s;
}

// --- references ---------------------------------------------------------------------------
struct RefRule {
  const char* kind;     // object kind ("*" = every kind)
  const char* pattern;  // "tracks[].clips[].asset", "*" = any member key
  const char* target;   // kind, or "@action" / "@duckSource" / "@curveParameter"
};
const RefRule kRefRules[] = {
    {"*", "bank", "bank"},
    {"bank", "dependsOn[]", "bank"},
    {"bus", "parent", "bus"},
    {"bus", "sends[].bus", "bus"},
    {"category", "parent", "category"},
    {"snapshot", "values[].bus", "bus"},
    {"ducking", "source", "@duckSource"},
    {"ducking", "target", "bus"},
    {"event", "bus", "bus"},
    {"event", "category", "category"},
    {"event", "limitGroup", "voiceLimitGroup"},
    {"event", "tracks[].bus", "bus"},
    {"event", "tracks[].layerParameter", "parameter"},
    {"event", "tracks[].clips[].asset", "asset"},
    {"event", "tracks[].clips[].event", "event"},
    {"event", "tracks[].clips[].synth.asset", "asset"},
    {"event", "tracks[].clips[].synth.patch", "patch"},
    {"event", "tracks[].actions[].target", "@action"},
    {"event", "curves[].parameter", "@curveParameter"},
    {"event", "music.states[].playlist", "playlist"},
    {"event", "music.transitions[].bridge", "segment"},
    {"event", "music.stingers[].event", "event"},
    {"segment", "asset", "asset"},
    {"segment", "stems[].asset", "asset"},
    {"segment", "stems[].volumeCurve.parameter", "@curveParameter"},
    {"playlist", "entries[].segment", "segment"},
    {"materialTable", "single.*.*", "event"},
    {"materialTable", "pairs[].event", "event"},
    {"materialTable", "footsteps.*", "event"},
    {"materialTable", "defaults.*", "event"},
    {"patch", "nodes[].params.parameter", "parameter"},
};

// Walks `v` along pattern segments, calling fn(path, value, parentObject) at the end.
template <class F>
void WalkPattern(const Value& v, const Value* parent, const std::vector<std::string>& pat, std::size_t i,
                 const std::string& path, F& fn) {
  if (i == pat.size()) {
    fn(path, v, parent);
    return;
  }
  const std::string& p = pat[i];
  if (p == "[]") {
    if (!v.IsArray()) return;
    for (std::size_t k = 0; k < v.items.size(); ++k) {
      WalkPattern(v.items[k], &v, pat, i + 1, path + "[" + std::to_string(k) + "]", fn);
    }
    return;
  }
  if (!v.IsObject()) return;
  for (const json::Member& m : v.members) {
    if (m.first == "notes") continue;
    if (p == "*" || m.first == p) {
      WalkPattern(m.second, &v, pat, i + 1, path.empty() ? m.first : path + "." + m.first, fn);
    }
  }
}
std::vector<std::string> SplitPattern(const char* pattern) {
  std::vector<std::string> out;
  std::string cur;
  for (const char* c = pattern; *c != '\0'; ++c) {
    if (*c == '.') {
      if (!cur.empty()) out.push_back(cur);
      cur.clear();
    } else if (*c == '[') {
      if (!cur.empty()) out.push_back(cur);
      cur.clear();
      out.push_back("[]");
      ++c;  // ']'
    } else {
      cur.push_back(*c);
    }
  }
  if (!cur.empty()) out.push_back(cur);
  return out;
}

int HexDigit(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

const std::vector<std::string> kKinds = {"project", "bank",  "bus",   "category", "parameter", "snapshot",
                                         "voiceLimitGroup", "ducking", "asset", "event", "segment", "playlist",
                                         "materialTable", "patch"};

std::uint64_t SplitMix(std::uint64_t& s) {
  std::uint64_t z = (s += 0x9E3779B97F4A7C15ull);
  z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
  z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
  return z ^ (z >> 31);
}

}  // namespace

// --- JSON helpers ---------------------------------------------------------------------------
bool JsonEqual(const Value& a, const Value& b) {
  if (a.type != b.type) return false;
  switch (a.type) {
    case json::Type::Null: return true;
    case json::Type::Bool: return a.boolean == b.boolean;
    case json::Type::Number:
    case json::Type::String: return a.text == b.text;
    case json::Type::Array:
      if (a.items.size() != b.items.size()) return false;
      for (std::size_t i = 0; i < a.items.size(); ++i) {
        if (!JsonEqual(a.items[i], b.items[i])) return false;
      }
      return true;
    case json::Type::Object:
      if (a.members.size() != b.members.size()) return false;
      for (std::size_t i = 0; i < a.members.size(); ++i) {
        if (a.members[i].first != b.members[i].first || !JsonEqual(a.members[i].second, b.members[i].second)) {
          return false;
        }
      }
      return true;
  }
  return false;
}

std::string FormatNumber(double v) {
  if (!std::isfinite(v)) return "0";  // last guard (D-176c): edits refuse non-finite numbers before this
  if (v == 0.0) return std::signbit(v) ? "-0" : "0";  // D-176b: the sign of zero survives
  if (std::fabs(v) < 9007199254740992.0 && v == std::trunc(v)) {
    char b[32];
    std::snprintf(b, sizeof b, "%.0f", v);
    return b;
  }
  char b[40];
  for (int prec = 1; prec <= 17; ++prec) {
    std::snprintf(b, sizeof b, "%.*g", prec, v);
    if (std::strtod(b, nullptr) == v) break;
  }
  return b;
}

Value Number(double v) {
  // A non-finite value becomes a literal JSON cannot hold; every edit refuses it (D-176c).
  if (!std::isfinite(v)) return json::MakeNumber(std::isnan(v) ? "nan" : (v > 0 ? "inf" : "-inf"));
  return json::MakeNumber(FormatNumber(v));
}

// Like JsonEqual, but numbers compare by value (and the sign of zero): "2", "2.0" and "2e0" are the same.
bool JsonSameMeaning(const Value& a, const Value& b) {
  if (a.type != b.type) return false;
  switch (a.type) {
    case json::Type::Number: {
      if (a.text == b.text) return true;
      const double x = std::strtod(a.text.c_str(), nullptr), y = std::strtod(b.text.c_str(), nullptr);
      return x == y && std::signbit(x) == std::signbit(y);
    }
    case json::Type::Array:
      if (a.items.size() != b.items.size()) return false;
      for (std::size_t i = 0; i < a.items.size(); ++i) {
        if (!JsonSameMeaning(a.items[i], b.items[i])) return false;
      }
      return true;
    case json::Type::Object:
      if (a.members.size() != b.members.size()) return false;
      for (std::size_t i = 0; i < a.members.size(); ++i) {
        if (a.members[i].first != b.members[i].first || !JsonSameMeaning(a.members[i].second, b.members[i].second)) return false;
      }
      return true;
    default: return JsonEqual(a, b);
  }
}

static bool HasNonFinite(const Value& v) {
  switch (v.type) {
    case json::Type::Number: return v.text == "nan" || v.text == "inf" || v.text == "-inf";
    case json::Type::Array:
      for (const Value& x : v.items) {
        if (HasNonFinite(x)) return true;
      }
      return false;
    case json::Type::Object:
      for (const json::Member& m : v.members) {
        if (HasNonFinite(m.second)) return true;
      }
      return false;
    default: return false;
  }
}
Value String(std::string s) { return json::MakeString(std::move(s)); }
Value Bool(bool b) { return json::MakeBool(b); }

Value* FindPath(Value& root, std::string_view path) {
  std::vector<Seg> segs;
  if (!ParsePath(path, &segs)) return nullptr;
  Value* v = &root;
  for (const Seg& s : segs) {
    if (s.isIndex) {
      if (!v->IsArray() || s.index >= v->items.size()) return nullptr;
      v = &v->items[s.index];
    } else {
      v = Member(*v, s.key);
      if (v == nullptr) return nullptr;
    }
  }
  return v;
}
const Value* FindPath(const Value& root, std::string_view path) { return FindPath(const_cast<Value&>(root), path); }

double NumberAt(const Value& root, std::string_view path, double def) {
  const Value* v = FindPath(root, path);
  double d = 0;
  return (v != nullptr && v->IsNumber() && v->AsDouble(&d)) ? d : def;
}
std::string StringAt(const Value& root, std::string_view path, const std::string& def) {
  const Value* v = FindPath(root, path);
  return (v != nullptr && v->IsString()) ? v->text : def;
}
bool BoolAt(const Value& root, std::string_view path, bool def) {
  const Value* v = FindPath(root, path);
  return (v != nullptr && v->IsBool()) ? v->boolean : def;
}

const char* const* SpecMemberOrder(std::string_view kind, std::string_view path) {
  // Pattern: indices -> "[]"; members of free-key objects (material names) -> "*".
  std::vector<Seg> segs;
  if (!ParsePath(path, &segs)) return nullptr;
  std::string pat;
  std::string patFree;
  for (std::size_t i = 0; i < segs.size(); ++i) {
    if (segs[i].isIndex) {
      pat += "[]";
      patFree += "[]";
    } else {
      pat += (pat.empty() ? "" : ".") + segs[i].key;
      patFree += std::string(patFree.empty() ? "" : ".") + (i + 1 == segs.size() ? "*" : segs[i].key);
    }
  }
  for (const OrderEntry& e : kOrders) {
    if (kind == e.kind && (pat == e.pattern || patFree == e.pattern)) return e.order;
  }
  return nullptr;
}

void SetMember(Value& object, const std::string& key, Value value, const char* const* order) {
  if (Value* m = Member(object, key)) {
    *m = std::move(value);
    return;
  }
  std::size_t at = object.members.size();
  auto rankOf = [&](const std::string& k) -> std::int64_t {
    if (order == nullptr || (k == "notes" && key != "notes")) return -1;  // notes may sit anywhere
    for (std::int64_t i = 0; order[i] != nullptr; ++i) {
      if (k == order[i]) return i;
    }
    return -1;
  };
  const std::int64_t rank = rankOf(key);
  if (rank >= 0) {
    // After the last member the spec puts before it; failing that, before the first member
    // the spec puts after it; failing that, last. Members the spec does not list stay put.
    bool found = false;
    for (std::size_t m = 0; m < object.members.size(); ++m) {
      const std::int64_t r = rankOf(object.members[m].first);
      if (r >= 0 && r < rank) {
        at = m + 1;
        found = true;
      }
    }
    if (!found) {
      for (std::size_t m = 0; m < object.members.size(); ++m) {
        if (rankOf(object.members[m].first) > rank) {
          at = m;
          break;
        }
      }
    }
  }
  object.members.insert(object.members.begin() + static_cast<std::ptrdiff_t>(at), json::Member{key, std::move(value)});
}

const std::vector<std::string>& ObjectKinds() { return kKinds; }
bool IsObjectKind(std::string_view kind) {
  for (const std::string& k : kKinds) {
    if (k == kind) return true;
  }
  return false;
}
const char* KindFolder(std::string_view kind) {
  if (kind == "project") return "";
  if (kind == "bank") return "banks";
  if (kind == "bus") return "buses";
  if (kind == "category") return "categories";
  if (kind == "parameter") return "parameters";
  if (kind == "snapshot") return "snapshots";
  if (kind == "voiceLimitGroup") return "voiceLimitGroups";
  if (kind == "ducking") return "duckings";
  if (kind == "asset") return "assets";
  if (kind == "event") return "events";
  if (kind == "segment") return "segments";
  if (kind == "playlist") return "playlists";
  if (kind == "materialTable") return "materials";
  if (kind == "patch") return "patches";
  return "objects";
}

std::string DocObject::kind() const { return StringAt(doc, "type"); }
std::string DocObject::name() const { return StringAt(doc, "name"); }

std::string NormalizeGuid(std::string_view text) {
  std::string t(text);
  if (t.size() == 38 && t.front() == '{' && t.back() == '}') t = t.substr(1, 36);
  if (t.size() != 36) return std::string();
  for (std::size_t i = 0; i < t.size(); ++i) {
    if (i == 8 || i == 13 || i == 18 || i == 23) {
      if (t[i] != '-') return std::string();
    } else if (HexDigit(t[i]) < 0) {
      return std::string();
    }
  }
  return Lower(t);
}
bool LooksLikeGuid(std::string_view text) { return !NormalizeGuid(text).empty(); }

// --- ProjectDocument --------------------------------------------------------------------------
ProjectDocument::ProjectDocument() {
  random_ = [state = platform::MonotonicNanoseconds() ^ reinterpret_cast<std::uintptr_t>(this)]() mutable {
    return SplitMix(state);
  };
}

void ProjectDocument::Reset() {
  objects_.clear();
  saved_.clear();
  savedText_.clear();
  otherFiles_.clear();
  loadIssues_.clear();
  ClearHistory();
  ++revision_;
}

bool ProjectDocument::AddLoaded(const std::string& file, const std::string& textIn) {
  std::string text = textIn;
  if (text.size() >= 3 && static_cast<unsigned char>(text[0]) == 0xEF && static_cast<unsigned char>(text[1]) == 0xBB &&
      static_cast<unsigned char>(text[2]) == 0xBF) {
    text.erase(0, 3);
  }
  DocObject o;
  o.file = file;
  json::ParseError pe;
  if (!json::Parse(text, &o.doc, &pe)) {
    loadIssues_.push_back(DocLoadIssue{file, std::to_string(pe.line) + ":" + std::to_string(pe.column) + ": " + pe.message});
    otherFiles_.push_back(file);
    return false;
  }
  if (!o.doc.IsObject()) {
    loadIssues_.push_back(DocLoadIssue{file, "a project file must hold one JSON object"});
    otherFiles_.push_back(file);
    return false;
  }
  const Value* g = o.doc.Find("guid");
  o.guid = g != nullptr && g->IsString() ? NormalizeGuid(g->text) : std::string();
  if (o.guid.empty()) {
    loadIssues_.push_back(DocLoadIssue{file, "missing or malformed \"guid\""});
    otherFiles_.push_back(file);
    return false;
  }
  if (Find(o.guid) != nullptr) {
    loadIssues_.push_back(DocLoadIssue{file, "duplicate guid " + o.guid + " (also in " + Find(o.guid)->file + ")"});
    otherFiles_.push_back(file);
    return false;
  }
  saved_[file] = o.doc;
  savedText_[file] = textIn;
  objects_.push_back(std::move(o));
  return true;
}

bool ProjectDocument::Load(const std::string& dir) {
  Reset();
  dir_ = dir;
  std::error_code ec;
  const fs::path root(dir);
  if (!fs::is_directory(root, ec)) {
    loadIssues_.push_back(DocLoadIssue{"", "cannot read project directory '" + dir + "'"});
    return false;
  }
  std::vector<std::string> files;
  fs::recursive_directory_iterator it(root, ec), end;
  if (ec) {
    loadIssues_.push_back(DocLoadIssue{"", "cannot list '" + dir + "': " + ec.message()});
    return false;
  }
  while (it != end) {
    const fs::path p = it->path();
    const std::string leaf = p.filename().string();
    std::error_code e2;
    if (it->is_directory(e2)) {
      if (!leaf.empty() && leaf[0] == '.') it.disable_recursion_pending();
    } else if (p.extension() == ".json") {
      files.push_back(fs::relative(p, root, e2).generic_string());
    }
    it.increment(ec);
    if (ec) {
      loadIssues_.push_back(DocLoadIssue{"", "cannot list '" + dir + "': " + ec.message()});
      return false;
    }
  }
  std::sort(files.begin(), files.end());
  for (const std::string& f : files) {
    std::string text;
    if (!ReadText(root / f, &text)) {
      loadIssues_.push_back(DocLoadIssue{f, "cannot read file"});
      otherFiles_.push_back(f);
      continue;
    }
    AddLoaded(f, text);
  }
  return true;
}

bool ProjectDocument::LoadFiles(const std::map<std::string, std::string>& files, const std::string& dir) {
  Reset();
  dir_ = dir;
  for (const auto& [f, text] : files) AddLoaded(f, text);
  return true;
}

void ProjectDocument::SetBinaryFile(const std::string& path, std::vector<std::uint8_t> bytes) {
  binary_[path] = std::move(bytes);
}

bool ProjectDocument::ReadBinaryFile(const std::string& path, std::vector<std::uint8_t>* out) const {
  auto it = binary_.find(path);
  if (it != binary_.end()) {
    *out = it->second;
    return true;
  }
  if (dir_.empty()) return false;
  std::string text;
  std::error_code ec;
  const fs::path p = fs::path(dir_) / path;
  if (!fs::is_regular_file(p, ec) || !ReadText(p, &text)) return false;
  out->assign(text.begin(), text.end());
  return true;
}

const DocObject* ProjectDocument::Find(std::string_view guid) const {
  const std::string g = NormalizeGuid(guid);
  if (g.empty()) return nullptr;
  for (const DocObject& o : objects_) {
    if (o.guid == g) return &o;
  }
  return nullptr;
}
DocObject* ProjectDocument::FindMutable(std::string_view guid) { return const_cast<DocObject*>(Find(guid)); }

const DocObject* ProjectDocument::FindByName(std::string_view kind, std::string_view name) const {
  for (const DocObject& o : objects_) {
    if (StringAt(o.doc, "type") == kind && StringAt(o.doc, "name") == name) return &o;
  }
  return nullptr;
}

const DocObject* ProjectDocument::Resolve(std::string_view kind, std::string_view ref) const {
  if (ref.empty()) return nullptr;
  if (const DocObject* o = Find(ref); o != nullptr) return o->kind() == kind ? o : nullptr;
  return FindByName(kind, ref);
}

std::vector<const DocObject*> ProjectDocument::OfKind(std::string_view kind) const {
  std::vector<const DocObject*> out;
  for (const DocObject& o : objects_) {
    if (o.kind() == kind) out.push_back(&o);
  }
  std::sort(out.begin(), out.end(), [](const DocObject* a, const DocObject* b) {
    const std::string an = a->name(), bn = b->name();
    return an != bn ? an < bn : a->guid < b->guid;
  });
  return out;
}

const DocObject* ProjectDocument::projectObject() const {
  for (const DocObject& o : objects_) {
    if (o.kind() == "project") return &o;
  }
  return nullptr;
}

bool ProjectDocument::IsUnchanged(const DocObject& o) const {
  auto it = saved_.find(o.file);
  return it != saved_.end() && JsonEqual(it->second, o.doc);
}

std::string ProjectDocument::TextOf(const DocObject& o) const {
  if (IsUnchanged(o)) return savedText_.at(o.file);
  return json::Write(o.doc);
}

bool ProjectDocument::NameTaken(std::string_view kind, const std::string& name, std::string_view exceptGuid) const {
  for (const DocObject& o : objects_) {
    if (o.guid != exceptGuid && o.kind() == kind && o.name() == name) return true;
  }
  return false;
}

bool ProjectDocument::FileTaken(const std::string& file, std::string_view exceptGuid) const {
  const std::string lf = Lower(file);
  for (const DocObject& o : objects_) {
    if (o.guid != exceptGuid && Lower(o.file) == lf) return true;
  }
  for (const std::string& f : otherFiles_) {
    if (Lower(f) == lf) return true;
  }
  // A file still on disk that a removed or moved object leaves behind is not given to
  // another object before Save (Save deletes it after writing); its own object may return.
  for (const auto& [f, v] : saved_) {
    if (Lower(f) != lf) continue;
    bool used = false;
    for (const DocObject& o : objects_) used = used || o.file == f;
    if (!used && NormalizeGuid(StringAt(v, "guid")) != exceptGuid) return true;
  }
  return false;
}

std::string ProjectDocument::FileNameFor(std::string_view kind, const std::string& name, std::string_view exceptGuid) const {
  std::string base;
  if (kind == "project") {
    base = "project";
  } else {
    std::string rel = KindFolder(kind);
    std::string part;
    std::string body;
    for (char c : name) {
      if (c == '/') {
        body += (body.empty() ? "" : "/") + SafePart(part);
        part.clear();
      } else {
        part.push_back(c);
      }
    }
    body += (body.empty() ? "" : "/") + SafePart(part);
    base = rel.empty() ? body : rel + "/" + body;
  }
  std::string file = base + ".json";
  for (int n = 2; FileTaken(file, exceptGuid); ++n) file = base + "_" + std::to_string(n) + ".json";
  return file;
}

std::string ProjectDocument::NewGuid() {
  for (int attempt = 0; attempt < 16; ++attempt) {  // D-177c: give up after 16 collisions
    std::uint64_t hi = random_ ? random_() : 0, lo = random_ ? random_() : 0;
    hi = (hi & ~0xF000ull) | 0x4000ull;                                  // version 4
    lo = (lo & 0x3FFFFFFFFFFFFFFFull) | 0x8000000000000000ull;           // variant 10
    char b[40];
    std::snprintf(b, sizeof b, "%08x-%04x-%04x-%04x-%012llx", static_cast<unsigned>(hi >> 32),
                  static_cast<unsigned>((hi >> 16) & 0xFFFF), static_cast<unsigned>(hi & 0xFFFF),
                  static_cast<unsigned>(lo >> 48), static_cast<unsigned long long>(lo & 0xFFFFFFFFFFFFull));
    if (Find(b) == nullptr) return b;
  }
  return std::string();
}

// --- transactions --------------------------------------------------------------------------
void ProjectDocument::Open(std::uint32_t mergeKey) {
  opOpen_ = true;
  openKey_ = mergeKey;
  if (groupDepth_ == 0u) open_ = Step{};
}

void ProjectDocument::Touch(const std::string& guid) {
  for (const ObjChange& c : open_.changes) {
    if (c.guid == guid) return;
  }
  ObjChange c;
  c.guid = guid;
  if (const DocObject* o = Find(guid)) {
    c.before.exists = true;
    c.before.object = *o;
  }
  open_.changes.push_back(std::move(c));
}

void ProjectDocument::Cancel() {
  // Undo whatever the cancelled operation touched (only when not in a group: inside a
  // group the earlier operations' changes stay).
  opOpen_ = false;
  if (groupDepth_ == 0u) {
    for (auto it = open_.changes.rbegin(); it != open_.changes.rend(); ++it) ApplySnapshot(it->guid, it->before);
    open_ = Step{};
  }
}

void ProjectDocument::Close() {
  opOpen_ = false;
  for (ObjChange& c : open_.changes) {
    c.after = Snapshot{};
    if (const DocObject* o = Find(c.guid)) {
      c.after.exists = true;
      c.after.object = *o;
    }
  }
  if (groupDepth_ != 0u) return;  // EndGroup files the step
  auto same = [](const Snapshot& a, const Snapshot& b) {
    return a.exists == b.exists && (!a.exists || (a.object.file == b.object.file && JsonEqual(a.object.doc, b.object.doc)));
  };
  Step step = std::move(open_);
  open_ = Step{};
  step.mergeKey = openKey_;
  std::vector<ObjChange> kept;
  for (ObjChange& c : step.changes) {
    if (!same(c.before, c.after)) kept.push_back(std::move(c));
  }
  step.changes = std::move(kept);
  if (step.changes.empty()) return;
  ++revision_;
  lastChanged_.clear();
  for (const ObjChange& c : step.changes) lastChanged_.push_back(c.guid);
  undone_.clear();
  if (step.mergeKey != 0u && !closedLast_ && !done_.empty() && done_.back().mergeKey == step.mergeKey) {
    Step& last = done_.back();
    for (ObjChange& c : step.changes) {
      bool merged = false;
      for (ObjChange& e : last.changes) {
        if (e.guid == c.guid) {
          e.after = c.after;
          merged = true;
        }
      }
      if (!merged) last.changes.push_back(std::move(c));
    }
    std::vector<ObjChange> keep;
    for (ObjChange& e : last.changes) {
      if (!same(e.before, e.after)) keep.push_back(std::move(e));
    }
    last.changes = std::move(keep);
    if (last.changes.empty()) done_.pop_back();
    return;
  }
  done_.push_back(std::move(step));
  if (done_.size() > maxSteps_) done_.erase(done_.begin());
  closedLast_ = done_.back().mergeKey == 0u;
}

void ProjectDocument::BeginGroup() {
  if (groupDepth_++ == 0u) open_ = Step{};
}

void ProjectDocument::EndGroup() {
  if (groupDepth_ == 0u) return;
  if (--groupDepth_ != 0u) return;
  openKey_ = 0;
  Close();
  closedLast_ = true;
}

void ProjectDocument::AbortGroup() {
  if (groupDepth_ == 0u) return;
  groupDepth_ = 0;
  opOpen_ = false;
  openKey_ = 0;
  bool changed = false;
  for (auto it = open_.changes.rbegin(); it != open_.changes.rend(); ++it) {
    const DocObject* now = Find(it->guid);
    const bool differs = (now != nullptr) != it->before.exists ||
                         (now != nullptr && (now->file != it->before.object.file || !JsonEqual(now->doc, it->before.object.doc)));
    if (differs) changed = true;
    ApplySnapshot(it->guid, it->before);
  }
  if (changed) {
    ++revision_;  // what the group showed is gone: observers look again
    lastChanged_.clear();
    for (const ObjChange& c : open_.changes) lastChanged_.push_back(c.guid);
  }
  open_ = Step{};
}

void ProjectDocument::ApplySnapshot(const std::string& guid, const Snapshot& s) {
  for (std::size_t i = 0; i < objects_.size(); ++i) {
    if (objects_[i].guid == guid) {
      if (s.exists) {
        objects_[i] = s.object;
      } else {
        objects_.erase(objects_.begin() + static_cast<std::ptrdiff_t>(i));
      }
      return;
    }
  }
  if (s.exists) objects_.push_back(s.object);
}

bool ProjectDocument::Undo() {
  if (groupDepth_ != 0u || done_.empty()) return false;
  Step s = std::move(done_.back());
  done_.pop_back();
  lastChanged_.clear();
  for (auto it = s.changes.rbegin(); it != s.changes.rend(); ++it) {
    ApplySnapshot(it->guid, it->before);
    lastChanged_.push_back(it->guid);
  }
  undone_.push_back(std::move(s));
  closedLast_ = true;
  ++revision_;
  return true;
}

bool ProjectDocument::Redo() {
  if (groupDepth_ != 0u || undone_.empty()) return false;
  Step s = std::move(undone_.back());
  undone_.pop_back();
  lastChanged_.clear();
  for (const ObjChange& c : s.changes) {
    ApplySnapshot(c.guid, c.after);
    lastChanged_.push_back(c.guid);
  }
  done_.push_back(std::move(s));
  closedLast_ = true;
  ++revision_;
  return true;
}

void ProjectDocument::ClearHistory() {
  done_.clear();
  undone_.clear();
  open_ = Step{};
  groupDepth_ = 0;
  opOpen_ = false;
  closedLast_ = true;
}

// --- edits ----------------------------------------------------------------------------------
std::string ProjectDocument::Create(std::string_view kind, const std::string& name, const Value* init) {
  if (!IsObjectKind(kind)) return std::string();
  if ((name.empty() && kind != "ducking") || HasControl(name) || (!name.empty() && NameTaken(kind, name, {}))) {
    return std::string();
  }
  if (kind == "project" && projectObject() != nullptr) return std::string();
  if (init != nullptr && HasNonFinite(*init)) return std::string();  // D-176c
  DocObject o;
  o.guid = NewGuid();
  if (o.guid.empty()) return std::string();
  o.doc = json::MakeObject();
  const char* const* order = SpecMemberOrder(kind, "");
  SetMember(o.doc, "type", String(std::string(kind)), order);
  SetMember(o.doc, "guid", String(o.guid), order);
  if (!name.empty()) SetMember(o.doc, "name", String(name), order);
  if (init != nullptr && init->IsObject()) {
    for (const json::Member& m : init->members) {
      if (m.first == "type" || m.first == "guid" || m.first == "name") continue;
      SetMember(o.doc, m.first, m.second, order);
    }
  }
  o.file = FileNameFor(kind, name.empty() ? o.guid.substr(0, 8) : name);
  Open(0);
  Touch(o.guid);
  objects_.push_back(o);
  Close();
  return o.guid;
}

std::string ProjectDocument::Duplicate(std::string_view guid, const std::string& newName) {
  const DocObject* src = Find(guid);
  if (src == nullptr) return std::string();
  const Value copy = src->doc;
  const std::string kind = src->kind();
  Value init = copy;
  // Aliases belong to the original.
  for (std::size_t i = 0; i < init.members.size(); ++i) {
    if (init.members[i].first == "aliases") {
      init.members.erase(init.members.begin() + static_cast<std::ptrdiff_t>(i));
      break;
    }
  }
  return Create(kind, newName, &init);
}

bool ProjectDocument::Rename(std::string_view guid, const std::string& newName, RenameOptions options) {
  DocObject* o = FindMutable(guid);
  if (o == nullptr) return false;
  const std::string kind = o->kind();
  const std::string oldName = o->name();
  if (newName == oldName) return true;  // D-177b: no change is success, no step
  if ((newName.empty() && kind != "ducking") || HasControl(newName) || NameTaken(kind, newName, o->guid)) return false;
  // An old name of another object cannot become a current name (build rule, 10.1).
  for (const DocObject& other : objects_) {
    if (other.guid == o->guid) continue;
    if (const Value* al = other.doc.Find("aliases"); al != nullptr && al->IsArray()) {
      for (const Value& a : al->items) {
        if (a.IsString() && a.text == newName) return false;
      }
    }
  }
  const std::string g = o->guid;
  // References by the old name: rewritten to the GUID (the build does not resolve aliases).
  std::vector<Reference> byName;
  if (!oldName.empty()) {
    for (const Reference& r : ReferencesTo(g)) {
      if (NormalizeGuid(r.text).empty()) byName.push_back(r);
    }
  }
  Open(0);
  Touch(g);
  o = FindMutable(g);
  const char* const* order = SpecMemberOrder(kind, "");
  const std::string oldPath = FileNameFor(kind, oldName.empty() ? g.substr(0, 8) : oldName, g);
  SetMember(o->doc, "name", String(newName), order);
  Value* al = Member(o->doc, "aliases");
  if (al != nullptr && al->IsArray()) {  // renaming back to an old name: no longer an alias
    for (std::size_t i = 0; i < al->items.size(); ++i) {
      if (al->items[i].IsString() && al->items[i].text == newName) {
        al->items.erase(al->items.begin() + static_cast<std::ptrdiff_t>(i));
        break;
      }
    }
  }
  if (options.keepAlias && !oldName.empty()) {
    if (al == nullptr) {
      SetMember(o->doc, "aliases", json::MakeArray(), order);
      al = Member(o->doc, "aliases");
    }
    bool have = false;
    for (const Value& a : al->items) have = have || (a.IsString() && a.text == oldName);
    if (!have) al->items.push_back(String(oldName));
  } else if (al != nullptr && al->items.empty()) {
    for (std::size_t i = 0; i < o->doc.members.size(); ++i) {
      if (o->doc.members[i].first == "aliases") {
        o->doc.members.erase(o->doc.members.begin() + static_cast<std::ptrdiff_t>(i));
        break;
      }
    }
  }
  if (options.moveFile && o->file == oldPath) o->file = FileNameFor(kind, newName.empty() ? g.substr(0, 8) : newName, g);
  for (const Reference& r : byName) {
    Touch(r.fromGuid);
    if (DocObject* from = FindMutable(r.fromGuid)) {
      if (Value* v = FindPath(from->doc, r.path)) *v = String(g);
    }
  }
  Close();
  return true;
}

bool ProjectDocument::Remove(std::string_view guid, bool force) {
  const DocObject* o = Find(guid);
  if (o == nullptr) return false;
  const std::string g = o->guid;
  if (!force) {
    for (const Reference& r : ReferencesTo(g)) {
      if (r.fromGuid != g) return false;
    }
  }
  Open(0);
  Touch(g);
  ApplySnapshot(g, Snapshot{});
  Close();
  return true;
}

bool ProjectDocument::Modify(std::string_view guid, const std::function<bool(Value&)>& fn, std::uint32_t mergeKey) {
  DocObject* o = FindMutable(guid);
  if (o == nullptr) return false;
  const std::string g = o->guid;
  const std::string type = o->kind();
  const std::string oldName = o->name();
  Open(mergeKey);
  Touch(g);
  o = FindMutable(g);
  const Value before = o->doc;
  if (!fn(o->doc) || !o->doc.IsObject() || StringAt(o->doc, "type") != type || NormalizeGuid(StringAt(o->doc, "guid")) != g ||
      HasNonFinite(o->doc)) {
    Cancel();
    return false;
  }
  // A name change through Modify keeps the rules of Rename.
  const std::string name = o->name();
  if (name != oldName && ((name.empty() && type != "ducking") || HasControl(name) || NameTaken(type, name, g))) {
    Cancel();
    return false;
  }
  // D-177b: an edit that leaves every value as it was (a number written another way) changes
  // nothing: the text stays as it is.
  if (JsonSameMeaning(before, o->doc)) o->doc = before;
  Close();
  return true;
}

bool ProjectDocument::Set(std::string_view guid, std::string_view path, Value value, std::uint32_t mergeKey) {
  const DocObject* o = Find(guid);
  if (o == nullptr) return false;
  const std::string kind = o->kind();
  std::vector<Seg> segs;
  if (!ParsePath(path, &segs) || segs.empty()) return false;
  if (!segs[0].isIndex && (segs[0].key == "guid" || segs[0].key == "type")) return false;
  if (!segs[0].isIndex && segs[0].key == "name" && segs.size() == 1) {
    if (!value.IsString()) return false;
    return Rename(guid, value.text, RenameOptions{false, false});
  }
  if (HasNonFinite(value)) return false;  // D-176c
  if (const Value* cur = FindPath(o->doc, path); cur != nullptr && JsonEqual(*cur, value)) return true;  // D-177b
  return Modify(
      guid,
      [&](Value& doc) {
        Value* v = &doc;
        for (std::size_t i = 0; i < segs.size(); ++i) {
          const bool last = i + 1 == segs.size();
          const Seg& s = segs[i];
          if (s.isIndex) {
            if (!v->IsArray()) return false;
            if (s.index == v->items.size()) {
              v->items.push_back(last ? value : (segs[i + 1].isIndex ? json::MakeArray() : json::MakeObject()));
            } else if (s.index > v->items.size()) {
              return false;
            } else if (last) {
              v->items[s.index] = value;
            }
            v = &v->items[s.index];
            continue;
          }
          if (!v->IsObject()) return false;
          Value* m = Member(*v, s.key);
          if (m == nullptr || last) {
            const char* const* order = SpecMemberOrder(kind, JoinPath(segs, i));
            SetMember(*v, s.key, last ? value : (segs[i + 1].isIndex ? json::MakeArray() : json::MakeObject()), order);
            m = Member(*v, s.key);
          }
          v = m;
        }
        return true;
      },
      mergeKey);
}

bool ProjectDocument::SetDefaulted(std::string_view guid, std::string_view path, Value value, const Value& def,
                                   std::uint32_t mergeKey) {
  const DocObject* o = Find(guid);
  if (o == nullptr || HasNonFinite(value)) return false;
  const Value* cur = FindPath(o->doc, path);
  if (JsonSameMeaning(cur != nullptr ? *cur : def, value)) return true;  // D-177b
  return Set(guid, path, std::move(value), mergeKey);
}

bool ProjectDocument::Erase(std::string_view guid, std::string_view path) {
  std::vector<Seg> segs;
  if (!ParsePath(path, &segs) || segs.empty()) return false;
  if (!segs[0].isIndex && segs.size() == 1 && (segs[0].key == "guid" || segs[0].key == "type" || segs[0].key == "name")) {
    return false;
  }
  const DocObject* o = Find(guid);
  if (o == nullptr || FindPath(o->doc, path) == nullptr) return false;
  const std::string parent = JoinPath(segs, segs.size() - 1);
  return Modify(guid, [&](Value& doc) {
    Value* p = FindPath(doc, parent);
    if (p == nullptr) return false;
    const Seg& s = segs.back();
    if (s.isIndex) {
      if (!p->IsArray() || s.index >= p->items.size()) return false;
      p->items.erase(p->items.begin() + static_cast<std::ptrdiff_t>(s.index));
      return true;
    }
    for (std::size_t i = 0; i < p->members.size(); ++i) {
      if (p->members[i].first == s.key) {
        p->members.erase(p->members.begin() + static_cast<std::ptrdiff_t>(i));
        return true;
      }
    }
    return false;
  });
}

bool ProjectDocument::Insert(std::string_view guid, std::string_view arrayPath, std::size_t index, Value value) {
  const DocObject* o = Find(guid);
  if (o == nullptr || HasNonFinite(value)) return false;
  const Value* arr = FindPath(o->doc, arrayPath);
  if (arr == nullptr) {
    if (index != 0) return false;
    Value a = json::MakeArray();
    a.items.push_back(std::move(value));
    return Set(guid, arrayPath, std::move(a));
  }
  if (!arr->IsArray() || index > arr->items.size()) return false;
  const std::string ap(arrayPath);
  return Modify(guid, [&](Value& doc) {
    Value* a = FindPath(doc, ap);
    if (a == nullptr) return false;
    a->items.insert(a->items.begin() + static_cast<std::ptrdiff_t>(index), value);
    return true;
  });
}

bool ProjectDocument::MoveElement(std::string_view guid, std::string_view arrayPath, std::size_t from, std::size_t to) {
  const DocObject* o = Find(guid);
  if (o == nullptr) return false;
  const Value* arr = FindPath(o->doc, arrayPath);
  if (arr == nullptr || !arr->IsArray() || from >= arr->items.size() || to >= arr->items.size()) return false;
  if (from == to) return true;  // D-177b: a move to the same place is success and changes nothing
  const std::string ap(arrayPath);
  return Modify(guid, [&](Value& doc) {
    Value* a = FindPath(doc, ap);
    if (a == nullptr) return false;
    Value v = std::move(a->items[from]);
    a->items.erase(a->items.begin() + static_cast<std::ptrdiff_t>(from));
    a->items.insert(a->items.begin() + static_cast<std::ptrdiff_t>(to), std::move(v));
    return true;
  });
}

// --- references -----------------------------------------------------------------------------
std::vector<Reference> ProjectDocument::ReferencesOf(std::string_view guid) const {
  std::vector<Reference> out;
  const DocObject* o = Find(guid);
  if (o == nullptr) return out;
  const std::string kind = o->kind();
  for (const RefRule& r : kRefRules) {
    if (std::string_view(r.kind) != "*" && kind != r.kind) continue;
    if (std::string_view(r.kind) == "*" && kind == "bank") continue;
    const std::vector<std::string> pat = SplitPattern(r.pattern);
    auto fn = [&](const std::string& path, const Value& v, const Value* parent) {
      if (!v.IsString()) return;
      std::string target = r.target;
      if (target == "@action") {
        const std::string a = parent != nullptr ? StringAt(*parent, "action", "play") : "play";
        target = a == "setParameter" ? "parameter" : a == "applySnapshot" ? "snapshot" : "event";
      } else if (target == "@duckSource") {
        target = StringAt(o->doc, "kind", "sidechain") == "rule" ? "category" : "bus";
      } else if (target == "@curveParameter") {
        if (v.text == "distance") return;
        target = "parameter";
      }
      Reference ref;
      ref.fromGuid = o->guid;
      ref.path = path;
      ref.targetKind = target;
      ref.text = v.text;
      const DocObject* t = Resolve(target, v.text);
      ref.targetGuid = t != nullptr ? t->guid : std::string();
      out.push_back(std::move(ref));
    };
    WalkPattern(o->doc, nullptr, pat, 0, std::string(), fn);
  }
  return out;
}

std::vector<Reference> ProjectDocument::ReferencesTo(std::string_view guid) const {
  std::vector<Reference> out;
  const std::string g = NormalizeGuid(guid);
  for (const DocObject& o : objects_) {
    for (Reference& r : ReferencesOf(o.guid)) {
      if (r.targetGuid == g) out.push_back(std::move(r));
    }
  }
  return out;
}

std::vector<Reference> ProjectDocument::BrokenReferences() const {
  std::vector<Reference> out;
  for (const DocObject& o : objects_) {
    for (Reference& r : ReferencesOf(o.guid)) {
      if (r.targetGuid.empty()) out.push_back(std::move(r));
    }
  }
  return out;
}

// --- saving ---------------------------------------------------------------------------------
std::vector<FileChange> ProjectDocument::PendingChanges() const {
  std::vector<FileChange> out;
  std::set<std::string> used;
  for (const DocObject& o : objects_) {
    used.insert(o.file);
    if (!IsUnchanged(o)) out.push_back(FileChange{FileChange::Op::Write, o.file, json::Write(o.doc)});
  }
  for (const auto& [file, v] : saved_) {
    if (used.count(file) == 0u) out.push_back(FileChange{FileChange::Op::Delete, file, std::string()});
  }
  std::sort(out.begin(), out.end(), [](const FileChange& a, const FileChange& b) { return a.file < b.file; });
  return out;
}

void ProjectDocument::MarkSaved() {
  saved_.clear();
  savedText_.clear();
  for (const DocObject& o : objects_) {
    saved_[o.file] = o.doc;
    savedText_[o.file] = json::Write(o.doc);
  }
}

bool ProjectDocument::Save(std::string* error) { return SaveTo(dir_, error); }

namespace {
// Writes `bytes` next to `target` and renames it over (D-176d). false and `why` on failure.
bool WriteAtomic(const fs::path& target, const std::string& bytes, std::string* why) {
  std::error_code ec;
  fs::create_directories(target.parent_path(), ec);
  const fs::file_status st = fs::symlink_status(target, ec);
  if (fs::exists(st) && !fs::is_regular_file(st)) {
    *why = "exists and is not a regular file";
    return false;
  }
  const fs::path tmp = target.string() + ".sonus-tmp";
  if (!WriteText(tmp, bytes)) {
    fs::remove(tmp, ec);
    *why = "cannot write";
    return false;
  }
  fs::rename(tmp, target, ec);
  if (ec) {
    fs::remove(tmp, ec);
    *why = "cannot replace (" + ec.message() + ")";
    return false;
  }
  return true;
}
// A project-relative path that stays inside the project ('/' separated, no "..", not absolute).
bool InsideProject(const std::string& rel) {
  if (rel.empty() || rel[0] == '/' || rel[0] == '\\' || rel.find(':') != std::string::npos) return false;
  std::size_t i = 0;
  while (i <= rel.size()) {
    const std::size_t e = rel.find_first_of("/\\", i);
    const std::string part = rel.substr(i, e == std::string::npos ? std::string::npos : e - i);
    if (part == "..") return false;
    if (e == std::string::npos) break;
    i = e + 1;
  }
  return true;
}
}  // namespace

std::vector<std::string> ProjectDocument::ReferencedFiles() const {
  std::set<std::string> files;
  for (const DocObject& o : objects_) {
    const std::string kind = o.kind();
    if (kind == "asset") {
      if (const Value* f = o.doc.Find("file"); f != nullptr && f->IsString() && !f->text.empty()) files.insert(f->text);
      if (const Value* l = o.doc.Find("languages"); l != nullptr && l->IsObject()) {
        for (const json::Member& m : l->members) {
          if (m.first != "notes" && m.second.IsString() && !m.second.text.empty()) files.insert(m.second.text);
        }
      }
    } else if (kind == "bus") {
      if (const Value* fx = o.doc.Find("effects"); fx != nullptr && fx->IsArray()) {
        for (const Value& e : fx->items) {
          if (const Value* b = e.Find("blob"); b != nullptr && b->IsString() && !b->text.empty()) files.insert(b->text);
        }
      }
    }
  }
  return std::vector<std::string>(files.begin(), files.end());
}

bool ProjectDocument::SaveTo(const std::string& dir, std::string* error) {
  lastSave_ = SaveResult{};
  SaveResult& res = lastSave_;
  auto fail = [&](const std::string& path, const std::string& why) { res.failures.push_back(SaveFailure{path, why}); };
  auto finish = [&]() {
    res.ok = res.failures.empty();
    if (!res.ok && error != nullptr) {
      *error = res.failures[0].path + ": " + res.failures[0].reason;
      if (res.failures.size() > 1) *error += " (+" + std::to_string(res.failures.size() - 1) + " more)";
    }
    return res.ok;
  };
  if (dir.empty()) {
    fail("", "no project directory");
    return finish();
  }
  std::error_code ec;
  // D-176e (review 2): the same directory is compared as a directory, not as text.
  auto norm = [](const std::string& d) {
    std::error_code e;
    fs::path p = fs::weakly_canonical(fs::absolute(fs::path(d), e), e);
    return p.lexically_normal();
  };
  bool same = dir == dir_;
  if (!same && !dir_.empty()) {
    std::error_code e1;
    same = fs::exists(dir, e1) && fs::exists(dir_, e1) ? fs::equivalent(dir, dir_, e1) : norm(dir) == norm(dir_);
  }
  if (!same && !dir_.empty()) {
    // A destination inside the project (its copy would be read with the project) or a directory
    // that contains the project (the project would be read with the copy): refused.
    const fs::path cur = norm(dir_), dst = norm(dir);
    auto under = [](const fs::path& inner, const fs::path& outer) {
      auto i = inner.begin();
      for (auto o = outer.begin(); o != outer.end(); ++o, ++i) {
        if (o->empty()) continue;
        if (i == inner.end() || *i != *o) return false;
      }
      return true;
    };
    if (under(dst, cur) || under(cur, dst)) {
      fail(dir, under(dst, cur) ? "the destination is inside the project directory"
                                : "the destination contains the project directory");
      return finish();
    }
    // *.json files already there that this save does not write would mix into the project.
    std::set<std::string> writes;
    for (const DocObject& o : objects_) writes.insert(o.file);
    for (const std::string& f : otherFiles_) writes.insert(f);
    if (fs::is_directory(dst, ec)) {
      for (auto it = fs::recursive_directory_iterator(dst, ec); !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
        const std::string leaf = it->path().filename().string();
        std::error_code e2;
        if (it->is_directory(e2)) {
          if (!leaf.empty() && leaf[0] == '.') it.disable_recursion_pending();
          continue;
        }
        if (it->path().extension() != ".json") continue;
        const std::string rel = fs::relative(it->path(), dst, e2).generic_string();
        if (writes.count(rel) == 0u) fail(rel, "a project file already in the destination that this save does not write");
      }
    }
    if (!res.failures.empty()) return finish();
  }
  if (same) {
    // 1. New and changed files: temporary file + rename. 2. Only if all of them were written:
    // files no object uses any more are deleted; a deletion that fails stays pending.
    for (const DocObject& o : objects_) {
      if (IsUnchanged(o)) continue;
      const std::string text = json::Write(o.doc);
      std::string why;
      if (!WriteAtomic(fs::path(dir_) / o.file, text, &why)) {
        fail(o.file, why);
        continue;
      }
      saved_[o.file] = o.doc;
      savedText_[o.file] = text;
    }
    if (!res.failures.empty()) return finish();  // keep the old files of moved objects
    std::set<std::string> used;
    for (const DocObject& o : objects_) used.insert(o.file);
    std::vector<std::string> gone;
    for (const auto& [file, v] : saved_) {
      if (used.count(file) == 0u) gone.push_back(file);
    }
    for (const std::string& file : gone) {
      const fs::path p = fs::path(dir_) / file;
      const bool removed = fs::remove(p, ec);
      if (!removed && fs::exists(fs::symlink_status(p, ec))) {
        fail(file, "cannot delete" + (ec ? " (" + ec.message() + ")" : std::string()));
        continue;
      }
      saved_.erase(file);
      savedText_.erase(file);
    }
    return finish();
  }
  // "Save as" (D-176e): every object, the files the project reads from inside its directory,
  // and the *.json files that could not be read; the document then lives in `dir`.
  for (const DocObject& o : objects_) {
    std::string why;
    if (!WriteAtomic(fs::path(dir) / o.file, TextOf(o), &why)) fail(o.file, why);
  }
  std::vector<std::string> copies = ReferencedFiles();
  for (const std::string& f : otherFiles_) copies.push_back(f);
  for (const std::string& rel : copies) {
    if (!InsideProject(rel)) {
      res.outside.push_back(rel);
      continue;
    }
    std::vector<std::uint8_t> bytes;
    if (!ReadBinaryFile(rel, &bytes)) {
      res.missing.push_back(rel);
      continue;
    }
    std::string why;
    if (!WriteAtomic(fs::path(dir) / rel, std::string(bytes.begin(), bytes.end()), &why)) fail(rel, why);
  }
  if (!res.failures.empty()) return finish();
  std::map<std::string, Value> saved;
  std::map<std::string, std::string> text;
  for (const DocObject& o : objects_) {
    saved[o.file] = o.doc;
    text[o.file] = TextOf(o);
  }
  saved_ = std::move(saved);
  savedText_ = std::move(text);
  dir_ = dir;
  return finish();
}

}  // namespace snd::tool
