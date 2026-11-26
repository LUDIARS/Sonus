#include "project.hpp"

#include "detail/file_io.hpp"
#include "snd/buildtool/schema.hpp"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <system_error>

namespace snd::buildtool::detail {
namespace fs = std::filesystem;

namespace {

const char* const kKindNames[] = {"project", "bank",  "bus",     "category", "parameter",
                                  "snapshot", "voiceLimitGroup", "ducking", "asset", "event",
                                  "segment",  "playlist", "materialTable", "patch"};
static_assert(sizeof(kKindNames) / sizeof(kKindNames[0]) == static_cast<std::size_t>(Kind::Count));

// Members each kind understands, at every level (project-format.md §1:
// unknown members are warned about so that typos do not fall back to defaults).
// A Schema lists the allowed keys of an object; a Field's `child` describes the
// value of the key (object, or array of objects). `freeKeys` objects take any
// key whose value follows `freeChild` (material names, languages, ...).
struct Schema;
struct Field {
  const char* key;
  const Schema* child;  // nullptr: scalar / array of scalars / anything (no check)
};
struct Schema {
  const Field* fields;
  bool freeKeys;
  const Schema* freeChild;
};
#define SND_FIELDS(name, ...) constexpr Field name[] = {__VA_ARGS__, {nullptr, nullptr}};

SND_FIELDS(kLoopF, {"start", nullptr}, {"end", nullptr})
constexpr Schema kLoop{kLoopF, false, nullptr};
SND_FIELDS(kOpusF, {"bitrate", nullptr}, {"complexity", nullptr}, {"vbr", nullptr}, {"signal", nullptr},
           {"independentFrames", nullptr})
constexpr Schema kOpus{kOpusF, false, nullptr};
SND_FIELDS(kConvertSetF, {"codec", nullptr}, {"sampleRate", nullptr}, {"channels", nullptr}, {"opus", &kOpus})
constexpr Schema kConvertSet{kConvertSetF, false, nullptr};
SND_FIELDS(kPlatformSetF, {"codec", nullptr}, {"sampleRate", nullptr}, {"channels", nullptr}, {"sectorAlign", nullptr},
           {"opus", &kOpus})
constexpr Schema kPlatformSet{kPlatformSetF, false, nullptr};
SND_FIELDS(kConvertF, {"default", &kConvertSet}, {"ANY_", &kConvertSet}, {"WIN_", &kConvertSet}, {"LNX_", &kConvertSet},
           {"MAC_", &kConvertSet}, {"IOS_", &kConvertSet}, {"AND_", &kConvertSet})
constexpr Schema kConvert{kConvertF, false, nullptr};
SND_FIELDS(kPlatformsF, {"default", &kPlatformSet}, {"ANY_", &kPlatformSet}, {"WIN_", &kPlatformSet},
           {"LNX_", &kPlatformSet}, {"MAC_", &kPlatformSet}, {"IOS_", &kPlatformSet}, {"AND_", &kPlatformSet})
constexpr Schema kPlatforms{kPlatformsF, false, nullptr};
constexpr Field kNoFields[] = {{nullptr, nullptr}};
constexpr Schema kFreeScalars{kNoFields, true, nullptr};
SND_FIELDS(kCurveF, {"parameter", nullptr}, {"target", nullptr}, {"compose", nullptr}, {"points", nullptr})
constexpr Schema kCurve{kCurveF, false, nullptr};
SND_FIELDS(kDistanceF, {"volume", &kCurve}, {"lowpass", &kCurve}, {"spread", &kCurve}, {"reverbSend", &kCurve},
           {"blend3D", &kCurve})
constexpr Schema kDistance{kDistanceF, false, nullptr};
SND_FIELDS(kConeF, {"innerDeg", nullptr}, {"outerDeg", nullptr}, {"outerDb", nullptr})
constexpr Schema kCone{kConeF, false, nullptr};
SND_FIELDS(kRandomF, {"volumeDb", nullptr}, {"pitchCents", nullptr}, {"pan", nullptr}, {"startMs", nullptr},
           {"filterHz", nullptr})
constexpr Schema kRandom{kRandomF, false, nullptr};
SND_FIELDS(kSynthF, {"kind", nullptr}, {"waveform", nullptr}, {"frequencyHz", nullptr}, {"duty", nullptr},
           {"noiseColor", nullptr}, {"asset", nullptr}, {"channels", nullptr}, {"patch", nullptr})
constexpr Schema kSynth{kSynthF, false, nullptr};
SND_FIELDS(kClipF, {"asset", nullptr}, {"event", nullptr}, {"synth", &kSynth}, {"startMs", nullptr}, {"weight", nullptr},
           {"volumeDb", nullptr}, {"pitchCents", nullptr}, {"random", &kRandom}, {"switchValue", nullptr},
           {"layer", nullptr}, {"fadeInMs", nullptr}, {"fadeOutMs", nullptr}, {"loopCount", nullptr})
constexpr Schema kClip{kClipF, false, nullptr};
SND_FIELDS(kActionF, {"timeMs", nullptr}, {"action", nullptr}, {"target", nullptr}, {"value", nullptr})
constexpr Schema kAction{kActionF, false, nullptr};
SND_FIELDS(kTrackF, {"bus", nullptr}, {"layerParameter", nullptr}, {"flags", nullptr}, {"clips", &kClip},
           {"actions", &kAction})
constexpr Schema kTrack{kTrackF, false, nullptr};
SND_FIELDS(kStateF, {"name", nullptr}, {"playlist", nullptr})
constexpr Schema kState{kStateF, false, nullptr};
SND_FIELDS(kTransitionF, {"from", nullptr}, {"to", nullptr}, {"timing", nullptr}, {"timingCount", nullptr},
           {"fadeOutBeats", nullptr}, {"fadeInBeats", nullptr}, {"bridge", nullptr}, {"start", nullptr},
           {"startMarker", nullptr}, {"interrupt", nullptr}, {"late", nullptr}, {"prepare", nullptr})
constexpr Schema kTransition{kTransitionF, false, nullptr};
SND_FIELDS(kStingerF, {"name", nullptr}, {"event", nullptr}, {"timing", nullptr}, {"timingCount", nullptr},
           {"overlap", nullptr})
constexpr Schema kStinger{kStingerF, false, nullptr};
SND_FIELDS(kMusicF, {"initialState", nullptr}, {"states", &kState}, {"transitions", &kTransition},
           {"stingers", &kStinger})
constexpr Schema kMusic{kMusicF, false, nullptr};
SND_FIELDS(kSendF, {"bus", nullptr}, {"levelDb", nullptr}, {"preFader", nullptr})
constexpr Schema kSend{kSendF, false, nullptr};
SND_FIELDS(kEffectF, {"type", nullptr}, {"params", nullptr}, {"blob", nullptr}, {"flags", nullptr})
constexpr Schema kEffect{kEffectF, false, nullptr};
SND_FIELDS(kSnapValueF, {"kind", nullptr}, {"bus", nullptr}, {"value", nullptr}, {"effect", nullptr},
           {"param", nullptr}, {"send", nullptr})
constexpr Schema kSnapValue{kSnapValueF, false, nullptr};
SND_FIELDS(kTempoF, {"tick", nullptr}, {"bpm", nullptr}, {"beatsPerBar", nullptr}, {"beatUnit", nullptr})
constexpr Schema kTempo{kTempoF, false, nullptr};
SND_FIELDS(kMarkerF, {"name", nullptr}, {"frame", nullptr}, {"kind", nullptr})
constexpr Schema kMarker{kMarkerF, false, nullptr};
SND_FIELDS(kStemF, {"channelFirst", nullptr}, {"channelCount", nullptr}, {"volumeCurve", &kCurve}, {"asset", nullptr})
constexpr Schema kStem{kStemF, false, nullptr};
SND_FIELDS(kEntryF, {"segment", nullptr}, {"weight", nullptr}, {"repeat", nullptr})
constexpr Schema kEntry{kEntryF, false, nullptr};
SND_FIELDS(kContactF, {"impact", nullptr}, {"slide", nullptr}, {"roll", nullptr}, {"break", nullptr})
constexpr Schema kContact{kContactF, false, nullptr};
constexpr Schema kPerMaterialContact{kNoFields, true, &kContact};
SND_FIELDS(kPairF, {"a", nullptr}, {"b", nullptr}, {"event", nullptr})
constexpr Schema kPair{kPairF, false, nullptr};
SND_FIELDS(kPhysCurveF, {"impulseToVelocity", nullptr}, {"massToPitchCents", nullptr}, {"speedToVolumeDb", nullptr})
constexpr Schema kPhysCurve{kPhysCurveF, false, nullptr};
constexpr Schema kPerMaterialCurve{kNoFields, true, &kPhysCurve};

SND_FIELDS(kPatchNodeF, {"id", nullptr}, {"kind", nullptr}, {"params", &kFreeScalars}, {"position", nullptr})
constexpr Schema kPatchNode{kPatchNodeF, false, nullptr};
SND_FIELDS(kPatchConnF, {"from", nullptr}, {"to", nullptr}, {"input", nullptr})
constexpr Schema kPatchConn{kPatchConnF, false, nullptr};

#define SND_COMMON \
  {"type", nullptr}, {"guid", nullptr}, {"name", nullptr}, {"aliases", nullptr}, {"bank", nullptr}, {"notes", nullptr}
SND_FIELDS(kProjectF, SND_COMMON, {"formatVersion", nullptr}, {"languages", nullptr}, {"platforms", &kPlatforms})
SND_FIELDS(kBankF, SND_COMMON, {"dependsOn", nullptr}, {"language", nullptr})
SND_FIELDS(kBusF, SND_COMMON, {"parent", nullptr}, {"master", nullptr}, {"output", nullptr}, {"lodBypassable", nullptr},
           {"meter", nullptr}, {"volumeDb", nullptr}, {"channels", nullptr}, {"lodPriority", nullptr},
           {"sends", &kSend}, {"effects", &kEffect})
SND_FIELDS(kCategoryF, SND_COMMON, {"parent", nullptr}, {"ignorePause", nullptr}, {"muteOnFocusLoss", nullptr},
           {"volumeDb", nullptr})
SND_FIELDS(kParameterF, SND_COMMON, {"kind", nullptr}, {"scope", nullptr}, {"min", nullptr}, {"max", nullptr},
           {"default", nullptr}, {"rampRate", nullptr})
SND_FIELDS(kSnapshotF, SND_COMMON, {"values", &kSnapValue})
SND_FIELDS(kGroupF, SND_COMMON, {"maxVoices", nullptr}, {"steal", nullptr})
SND_FIELDS(kDuckingF, SND_COMMON, {"kind", nullptr}, {"source", nullptr}, {"target", nullptr}, {"amountDb", nullptr},
           {"attackMs", nullptr}, {"releaseMs", nullptr}, {"thresholdDb", nullptr})
SND_FIELDS(kAssetF, SND_COMMON, {"file", nullptr}, {"languages", &kFreeScalars}, {"loadForm", nullptr},
           {"loop", &kLoop}, {"convert", &kConvert}, {"stallPolicy", nullptr}, {"virtualReturn", nullptr},
           {"ioPriority", nullptr}, {"preloadMs", nullptr}, {"seekIntervalMs", nullptr})
SND_FIELDS(kEventF, SND_COMMON, {"bus", nullptr}, {"category", nullptr}, {"limitGroup", nullptr},
           {"maxInstances", nullptr}, {"retriggerMs", nullptr}, {"priority", nullptr}, {"sequence", nullptr},
           {"is3D", nullptr}, {"ignorePause", nullptr}, {"stinger", nullptr}, {"virtualReturn", nullptr},
           {"fadeOutMs", nullptr}, {"comboWindowMs", nullptr}, {"maxDistance", nullptr}, {"cone", &kCone},
           {"distanceCurves", &kDistance}, {"curves", &kCurve}, {"tracks", &kTrack}, {"music", &kMusic})
SND_FIELDS(kSegmentF, SND_COMMON, {"asset", nullptr}, {"bars", nullptr}, {"tempo", &kTempo}, {"entryFrame", nullptr},
           {"exitFrame", nullptr}, {"markers", &kMarker}, {"stems", &kStem}, {"entryPreload", nullptr})
SND_FIELDS(kPlaylistF, SND_COMMON, {"mode", nullptr}, {"loopCount", nullptr}, {"entries", &kEntry})
SND_FIELDS(kMaterialF, SND_COMMON, {"materials", nullptr}, {"single", &kPerMaterialContact}, {"pairs", &kPair},
           {"footsteps", &kFreeScalars}, {"defaults", &kContact}, {"curves", &kPerMaterialCurve})
SND_FIELDS(kPatchF, SND_COMMON, {"nodes", &kPatchNode}, {"connections", &kPatchConn}, {"output", nullptr})
#undef SND_COMMON
#undef SND_FIELDS

constexpr Schema kKindSchemas[] = {
    {kProjectF, false, nullptr},  {kBankF, false, nullptr},      {kBusF, false, nullptr},
    {kCategoryF, false, nullptr}, {kParameterF, false, nullptr}, {kSnapshotF, false, nullptr},
    {kGroupF, false, nullptr},    {kDuckingF, false, nullptr},   {kAssetF, false, nullptr},
    {kEventF, false, nullptr},    {kSegmentF, false, nullptr},   {kPlaylistF, false, nullptr},
    {kMaterialF, false, nullptr}, {kPatchF, false, nullptr},
};
static_assert(sizeof(kKindSchemas) / sizeof(kKindSchemas[0]) == static_cast<std::size_t>(Kind::Count));

void CheckMembers(const std::string& file, const json::Value& v, const Schema* schema, const std::string& where,
                  Diagnostics& diag) {
  if (schema == nullptr) return;
  if (v.IsArray()) {
    for (const json::Value& item : v.items) CheckMembers(file, item, schema, where + "[]", diag);
    return;
  }
  if (!v.IsObject()) return;
  for (const auto& m : v.members) {
    const std::string path = where.empty() ? m.first : where + "." + m.first;
    if (m.first == "notes") continue;  // free-form text allowed in every object (project-format.md §1)
    if (schema->freeKeys) {
      CheckMembers(file, m.second, schema->freeChild, path, diag);
      continue;
    }
    const Field* f = schema->fields;
    while (f->key != nullptr && m.first != f->key) ++f;
    if (f->key == nullptr) {
      diag.Warning(file, m.second.line, m.second.column, "unknown member \"" + path + "\" ignored");
      continue;
    }
    if (m.first == "notes") continue;  // free-form
    CheckMembers(file, m.second, f->child, path, diag);
  }
}

int HexDigit(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

std::string Describe(const json::Value& v) {
  return v.IsString() ? "'" + v.text + "'" : std::string("(non-string)");
}

// Members whose value is an array (of objects or scalars) in the schema.
bool IsArrayKey(const std::string& k) {
  static const char* const kArrays[] = {"tracks", "clips", "actions", "curves", "sends", "effects", "values", "states",
                                        "transitions", "stingers", "tempo", "markers", "stems", "entries", "pairs",
                                        "nodes", "connections", "aliases", "dependsOn", "languagesList", "params",
                                        "points", "layer", "materials", "scope", "position"};
  for (const char* a : kArrays) {
    if (k == a) return true;
  }
  return false;
}

void Walk(const Schema* s, const std::string& prefix, bool topLevel, std::vector<std::string>* out) {
  if (s == nullptr) return;
  if (s->freeKeys) {
    const std::string p = prefix.empty() ? "*" : prefix + ".*";
    out->push_back(p);
    Walk(s->freeChild, p, false, out);
    return;
  }
  for (const Field* f = s->fields; f->key != nullptr; ++f) {
    std::string key = f->key;
    std::string p = prefix.empty() ? key : prefix + "." + key;
    // "languages" is an array of names in project, a free-key object in asset; "params" an
    // array in effects, an object in patch nodes: decided by the child schema.
    const bool array = IsArrayKey(key) && !(key == "params" && f->child != nullptr) && !(key == "languages" && f->child != nullptr);
    if (key == "languages" && topLevel && f->child == nullptr) {
      out->push_back(p);
      out->push_back(p + "[]");
      continue;
    }
    out->push_back(p);
    if (array) {
      p += "[]";
      out->push_back(p);
    }
    Walk(f->child, p, false, out);
  }
}

}  // namespace

std::vector<std::string> SchemaKindList() {
  std::vector<std::string> out;
  for (int i = 0; i < static_cast<int>(Kind::Count); ++i) out.push_back(kKindNames[i]);
  return out;
}

std::vector<std::string> SchemaMemberList(const std::string& kind) {
  std::vector<std::string> out;
  for (int i = 0; i < static_cast<int>(Kind::Count); ++i) {
    if (kind == kKindNames[i]) Walk(&kKindSchemas[i], std::string(), true, &out);
  }
  return out;
}

const char* KindName(Kind k) { return kKindNames[static_cast<int>(k)]; }

bool ParseKind(const std::string& s, Kind* out) {
  for (int i = 0; i < static_cast<int>(Kind::Count); ++i) {
    if (s == kKindNames[i]) {
      *out = static_cast<Kind>(i);
      return true;
    }
  }
  return false;
}

std::string Hex32(std::uint32_t v) {
  char b[16];
  std::snprintf(b, sizeof(b), "0x%08X", v);
  return b;
}

// "xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx", optionally in braces. hi = first 16 hex digits.
bool ParseGuid(const std::string& text, std::uint64_t* lo, std::uint64_t* hi) {
  std::string t = text;
  if (t.size() == 38 && t.front() == '{' && t.back() == '}') t = t.substr(1, 36);
  if (t.size() != 36) return false;
  std::uint64_t parts[2] = {0, 0};
  int n = 0;
  for (std::size_t i = 0; i < t.size(); ++i) {
    if (i == 8 || i == 13 || i == 18 || i == 23) {
      if (t[i] != '-') return false;
      continue;
    }
    const int d = HexDigit(t[i]);
    if (d < 0) return false;
    parts[n / 16] = (parts[n / 16] << 4) | static_cast<std::uint64_t>(d);
    ++n;
  }
  *hi = parts[0];
  *lo = parts[1];
  return true;
}

bool LooksLikeGuid(const std::string& text) {
  std::uint64_t a, b;
  return ParseGuid(text, &a, &b);
}

bool Project::Load(const std::string& root, const std::vector<std::string>& skipDirs, Diagnostics& diag) {
  std::error_code ec;
  root_ = fs::weakly_canonical(fs::path(root), ec).string();
  if (ec || !fs::is_directory(root_, ec)) {
    diag.Error(root, 0, 0, "project directory not found");
    return false;
  }
  std::vector<fs::path> skip;
  for (const std::string& s : skipDirs) {
    std::error_code e2;
    skip.push_back(fs::weakly_canonical(fs::path(s), e2));
  }
  std::vector<std::string> files;
  for (fs::recursive_directory_iterator it(root_, fs::directory_options::skip_permission_denied, ec), end;
       !ec && it != end; it.increment(ec)) {
    const fs::path p = it->path();
    if (it->is_directory(ec)) {
      const fs::path canon = fs::weakly_canonical(p, ec);
      bool skipIt = p.filename().string().rfind('.', 0) == 0;  // hidden (.git, ...)
      for (const fs::path& s : skip) skipIt = skipIt || canon == s;
      if (skipIt) it.disable_recursion_pending();
      continue;
    }
    if (p.extension() == ".json") files.push_back(fs::relative(p, root_, ec).generic_string());
  }
  source_ = nullptr;
  return LoadFiles(files, diag);
}

bool Project::Load(const ProjectSource& source, const std::string& name, Diagnostics& diag) {
  root_ = name;
  source_ = &source;
  std::vector<std::string> files;
  source.ListObjectFiles(&files);
  std::vector<std::string> json;
  for (const std::string& f : files) {
    if (f.size() > 5 && f.compare(f.size() - 5, 5, ".json") == 0) json.push_back(f);
  }
  return LoadFiles(json, diag);
}

bool Project::ReadProjectFile(const std::string& rel, std::vector<std::uint8_t>* out) const {
  if (source_ != nullptr) return source_->ReadFile(rel, out);
  return ReadFileBytes(fs::path(root_) / rel, out);
}

bool Project::LoadFiles(const std::vector<std::string>& unsorted, Diagnostics& diag) {
  std::vector<std::string> files = unsorted;
  std::sort(files.begin(), files.end());
  files.erase(std::unique(files.begin(), files.end()), files.end());
  objects_.clear();
  objects_.reserve(files.size());
  for (const std::string& rel : files) {
    std::string text;
    std::vector<std::uint8_t> bytes;
    if (!ReadProjectFile(rel, &bytes)) {
      diag.Error(rel, 0, 0, "cannot read file");
      continue;
    }
    text.assign(bytes.begin(), bytes.end());
    Object o;
    o.file = rel;
    json::ParseError pe;
    if (!json::Parse(text, &o.doc, &pe)) {
      diag.Error(rel, pe.line, pe.column, pe.message);
      continue;
    }
    if (!o.doc.IsObject()) {
      diag.Error(rel, o.doc.line, o.doc.column, "a project file must hold one JSON object");
      continue;
    }
    const json::Value* type = o.doc.Find("type");
    if (type == nullptr || !type->IsString() || !ParseKind(type->text, &o.kind)) {
      diag.Error(rel, type != nullptr ? type->line : 1, type != nullptr ? type->column : 1,
                 "missing or unknown \"type\"" + (type != nullptr ? " " + Describe(*type) : std::string()));
      continue;
    }
    const json::Value* guid = o.doc.Find("guid");
    if (guid == nullptr || !guid->IsString() || !ParseGuid(guid->text, &o.guidLo, &o.guidHi)) {
      diag.Error(rel, guid != nullptr ? guid->line : 1, guid != nullptr ? guid->column : 1,
                 "missing or malformed \"guid\" (xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx)");
      continue;
    }
    o.guidText = guid->text;
    std::transform(o.guidText.begin(), o.guidText.end(), o.guidText.begin(),
                   [](char c) { return static_cast<char>(c >= 'A' && c <= 'F' ? c - 'A' + 'a' : c); });
    if (o.guidText.front() == '{') o.guidText = o.guidText.substr(1, 36);
    const json::Value* name = o.doc.Find("name");
    if (name != nullptr && !name->IsString()) {
      diag.Error(rel, name->line, name->column, "\"name\" must be a string");
      continue;
    }
    if (name != nullptr) o.name = name->text;
    if (o.kind != Kind::Ducking && o.name.empty()) {
      diag.Error(rel, 1, 1, std::string("a ") + KindName(o.kind) + " needs a non-empty \"name\"");
      continue;
    }
    o.id = o.name.empty() ? 0 : HashId(o.name);
    // Names: no control characters (U+0000..U+001F, U+007F): they break the
    // NUL-terminated string pool and the generated C++ header.
    bool badName = false;
    auto checkName = [&](const json::Value& v) {
      for (char ch : v.text) {
        if (static_cast<unsigned char>(ch) < 0x20 || ch == 0x7F) {
          diag.Error(rel, v.line, v.column, "a name or alias must not contain control characters");
          badName = true;
          return;
        }
      }
    };
    if (name != nullptr) checkName(*name);
    if (const json::Value* al = o.doc.Find("aliases"); al != nullptr && al->IsArray()) {
      for (const json::Value& a : al->items) {
        if (a.IsString()) checkName(a);
      }
    }
    if (badName) continue;
    // Unknown members at every level: warn (typos would otherwise fall back to defaults).
    CheckMembers(rel, o.doc, &kKindSchemas[static_cast<int>(o.kind)], "", diag);
    objects_.push_back(std::move(o));
  }
  // Indexes, duplicates.
  byGuid_.clear();
  byName_.clear();
  project_ = nullptr;
  for (std::size_t i = 0; i < objects_.size(); ++i) {
    const Object& o = objects_[i];
    auto g = byGuid_.emplace(o.guidText, i);
    if (!g.second) {
      diag.Error(o.file, o.doc.Find("guid")->line, o.doc.Find("guid")->column,
                 "duplicate guid " + o.guidText + " (also in " + objects_[g.first->second].file + ")");
    }
    if (o.kind == Kind::Project) {
      if (project_ != nullptr) diag.Error(o.file, 1, 1, "more than one project object (also " + project_->file + ")");
      project_ = &objects_[i];
      continue;
    }
    if (o.name.empty()) continue;
    auto n = byName_.emplace(std::make_pair(static_cast<int>(o.kind), o.name), i);
    if (!n.second) {
      diag.Error(o.file, o.doc.Find("name")->line, o.doc.Find("name")->column,
                 std::string("duplicate ") + KindName(o.kind) + " name '" + o.name + "' (also in " +
                     objects_[n.first->second].file + ")");
    }
  }
  // Explicit bank assignments.
  for (Object& o : objects_) {
    const json::Value* b = o.doc.Find("bank");
    if (b == nullptr || b->IsNull()) continue;
    const Object* bank = Resolve(o, *b, Kind::Bank, diag);
    if (bank != nullptr) o.bank = bank->name;
  }
  return diag.errors() == 0;
}

std::vector<const Object*> Project::OfKind(Kind k) const {
  std::vector<const Object*> out;
  for (const Object& o : objects_) {
    if (o.kind == k) out.push_back(&o);
  }
  std::sort(out.begin(), out.end(), [](const Object* a, const Object* b) {
    return a->name != b->name ? a->name < b->name : a->guidText < b->guidText;
  });
  return out;
}

const Object* Project::FindByName(Kind kind, const std::string& name) const {
  auto it = byName_.find(std::make_pair(static_cast<int>(kind), name));
  return it == byName_.end() ? nullptr : &objects_[it->second];
}

const Object* Project::Resolve(const Object& from, const json::Value& ref, Kind kind, Diagnostics& diag) const {
  if (!ref.IsString() || ref.text.empty()) {
    ErrorAt(from, ref, std::string("expected a reference to a ") + KindName(kind) + " (guid or name)", diag);
    return nullptr;
  }
  std::string text = ref.text;
  if (LooksLikeGuid(text)) {
    if (text.front() == '{') text = text.substr(1, 36);
    std::transform(text.begin(), text.end(), text.begin(),
                   [](char c) { return static_cast<char>(c >= 'A' && c <= 'F' ? c - 'A' + 'a' : c); });
    auto it = byGuid_.find(text);
    if (it != byGuid_.end()) {
      const Object& o = objects_[it->second];
      if (o.kind != kind) {
        ErrorAt(from, ref, "guid " + text + " is a " + KindName(o.kind) + ", expected a " + KindName(kind), diag);
        return nullptr;
      }
      return &o;
    }
  }
  const Object* o = FindByName(kind, ref.text);
  if (o == nullptr) ErrorAt(from, ref, std::string("unknown ") + KindName(kind) + " " + Describe(ref), diag);
  return o;
}

void Project::ErrorAt(const Object& o, const json::Value& v, const std::string& msg, Diagnostics& d) const {
  d.Error(o.file, v.line, v.column, msg);
}
void Project::WarnAt(const Object& o, const json::Value& v, const std::string& msg, Diagnostics& d) const {
  d.Warning(o.file, v.line, v.column, msg);
}

double Project::Num(const Object& o, const json::Value& parent, const char* key, double def, Diagnostics& d) const {
  const json::Value* v = parent.Find(key);
  if (v == nullptr || v->IsNull()) return def;
  double x = 0;
  if (!v->AsDouble(&x)) {
    ErrorAt(o, *v, std::string("\"") + key + "\" must be a number", d);
    return def;
  }
  return x;
}

std::uint64_t Project::U64(const Object& o, const json::Value& parent, const char* key, std::uint64_t def,
                           Diagnostics& d) const {
  const json::Value* v = parent.Find(key);
  if (v == nullptr || v->IsNull()) return def;
  std::uint64_t x = 0;
  if (!v->AsU64(&x)) {
    ErrorAt(o, *v, std::string("\"") + key + "\" must be a non-negative integer", d);
    return def;
  }
  return x;
}

bool Project::Bool(const Object& o, const json::Value& parent, const char* key, bool def, Diagnostics& d) const {
  const json::Value* v = parent.Find(key);
  if (v == nullptr || v->IsNull()) return def;
  if (!v->IsBool()) {
    ErrorAt(o, *v, std::string("\"") + key + "\" must be true or false", d);
    return def;
  }
  return v->boolean;
}

std::string Project::Str(const Object& o, const json::Value& parent, const char* key, const std::string& def,
                         Diagnostics& d) const {
  const json::Value* v = parent.Find(key);
  if (v == nullptr || v->IsNull()) return def;
  if (!v->IsString()) {
    ErrorAt(o, *v, std::string("\"") + key + "\" must be a string", d);
    return def;
  }
  return v->text;
}

std::uint32_t Project::Enum(const Object& o, const json::Value& parent, const char* key, const char* const* names,
                            std::uint32_t def, Diagnostics& d) const {
  const json::Value* v = parent.Find(key);
  if (v == nullptr || v->IsNull()) return def;
  if (v->IsString()) {
    for (std::uint32_t i = 0; names[i] != nullptr; ++i) {
      if (v->text == names[i]) return i;
    }
  }
  std::string list;
  for (std::uint32_t i = 0; names[i] != nullptr; ++i) list += (i ? ", " : "") + std::string(names[i]);
  ErrorAt(o, *v, std::string("\"") + key + "\" must be one of: " + list, d);
  return def;
}

}  // namespace snd::buildtool::detail

namespace snd::buildtool {
std::vector<std::string> SchemaKinds() { return detail::SchemaKindList(); }
std::vector<std::string> SchemaMembers(const std::string& kind) { return detail::SchemaMemberList(kind); }
}  // namespace snd::buildtool
