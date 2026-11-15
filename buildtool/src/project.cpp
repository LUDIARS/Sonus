#include "project.hpp"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <system_error>

namespace snd::buildtool::detail {
namespace fs = std::filesystem;

namespace {

const char* const kKindNames[] = {"project", "bank",  "bus",     "category", "parameter",
                                  "snapshot", "voiceLimitGroup", "ducking", "asset", "event",
                                  "segment",  "playlist", "materialTable"};
static_assert(sizeof(kKindNames) / sizeof(kKindNames[0]) == static_cast<std::size_t>(Kind::Count));

// Top-level members each kind understands (others: warning, likely a typo).
const char* const kCommon[] = {"type", "guid", "name", "aliases", "bank", "notes", nullptr};
const char* const kKeys[][32] = {
    /* project */ {"formatVersion", "languages", "platforms", nullptr},
    /* bank */ {"dependsOn", "language", nullptr},
    /* bus */ {"parent", "master", "output", "lodBypassable", "meter", "volumeDb", "channels", "lodPriority",
               "sends", "effects", nullptr},
    /* category */ {"parent", "ignorePause", "muteOnFocusLoss", "volumeDb", nullptr},
    /* parameter */ {"kind", "scope", "min", "max", "default", "rampRate", nullptr},
    /* snapshot */ {"values", nullptr},
    /* voiceLimitGroup */ {"maxVoices", "steal", nullptr},
    /* ducking */ {"kind", "source", "target", "amountDb", "attackMs", "releaseMs", "thresholdDb", nullptr},
    /* asset */ {"file", "languages", "loadForm", "loop", "convert", "stallPolicy", "virtualReturn", "ioPriority",
                 "preloadMs", "seekIntervalMs", nullptr},
    /* event */ {"bus", "category", "limitGroup", "maxInstances", "retriggerMs", "priority", "sequence", "is3D",
                 "ignorePause", "stinger", "virtualReturn", "fadeOutMs", "comboWindowMs", "maxDistance", "cone",
                 "distanceCurves", "curves", "tracks", "music", nullptr},
    /* segment */ {"asset", "bars", "tempo", "entryFrame", "exitFrame", "markers", "stems", "entryPreload", nullptr},
    /* playlist */ {"mode", "loopCount", "entries", nullptr},
    /* materialTable */ {"materials", "single", "pairs", "footsteps", "defaults", "curves", nullptr},
};

bool ReadFile(const fs::path& p, std::string* out) {
  std::ifstream in(p, std::ios::binary);
  if (!in) return false;
  out->assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
  return true;
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

}  // namespace

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
  std::sort(files.begin(), files.end());
  objects_.clear();
  objects_.reserve(files.size());
  for (const std::string& rel : files) {
    std::string text;
    if (!ReadFile(fs::path(root_) / rel, &text)) {
      diag.Error(rel, 0, 0, "cannot read file");
      continue;
    }
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
    // Unknown members: warn (typos would otherwise silently fall back to defaults).
    for (const auto& m : o.doc.members) {
      bool known = false;
      for (const char* const* k = kCommon; *k != nullptr && !known; ++k) known = m.first == *k;
      for (const char* const* k = kKeys[static_cast<int>(o.kind)]; *k != nullptr && !known; ++k) known = m.first == *k;
      if (!known) diag.Warning(rel, m.second.line, m.second.column, "unknown member \"" + m.first + "\" ignored");
    }
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
