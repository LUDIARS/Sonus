// Project reading for the tool (spec/interface/project-format.md).
#include "snd/tool/project.hpp"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <map>
#include <system_error>

#include "snd/buildtool/build.hpp"
#include "snd/buildtool/diagnostics.hpp"
#include "snd/buildtool/json.hpp"
#include "snd/dsp/effects.hpp"
#include "snd/tool/document.hpp"

namespace snd::tool {
namespace {

namespace fs = std::filesystem;
namespace json = snd::buildtool::json;

int HexDigit(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

bool ReadFile(const std::string& path, std::string* out) {
  std::FILE* f = std::fopen(path.c_str(), "rb");
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

struct RawObject {
  std::string file;
  std::string type;
  std::string name;
  Guid guid;
  json::Value doc;
};

std::string At(const RawObject& o, const json::Value& v) {
  return o.file + ":" + std::to_string(v.line) + ":" + std::to_string(v.column);
}

float Number(const json::Value* v, float def) {
  double d = 0.0;
  if (v == nullptr || !v->IsNumber() || !v->AsDouble(&d)) return def;
  return static_cast<float>(d);
}

std::string String(const json::Value* v) { return (v != nullptr && v->IsString()) ? v->text : std::string(); }

// Index of the object a reference names (GUID or name), -1 if none.
template <class T>
std::int32_t Resolve(const std::vector<T>& items, const std::string& ref) {
  Guid g;
  const bool isGuid = ParseGuid(ref, &g);
  for (std::size_t i = 0; i < items.size(); ++i) {
    if (isGuid ? items[i].guid == g : items[i].name == ref) return static_cast<std::int32_t>(i);
  }
  return -1;
}

// Reorders `items` (each with .parent indexes into the old order and .name) into tree
// order: roots by name, then depth first with children by name. Fixes parent / depth.
template <class T>
std::vector<T> TreeOrder(const std::vector<T>& items, const std::vector<std::int32_t>& parentOf) {
  std::vector<std::vector<std::int32_t>> children(items.size());
  std::vector<std::int32_t> roots;
  for (std::size_t i = 0; i < items.size(); ++i) {
    const std::int32_t p = parentOf[i];
    if (p < 0) {
      roots.push_back(static_cast<std::int32_t>(i));
    } else {
      children[static_cast<std::size_t>(p)].push_back(static_cast<std::int32_t>(i));
    }
  }
  auto byName = [&](std::int32_t a, std::int32_t b) {
    return items[static_cast<std::size_t>(a)].name < items[static_cast<std::size_t>(b)].name;
  };
  std::sort(roots.begin(), roots.end(), byName);
  for (auto& c : children) std::sort(c.begin(), c.end(), byName);
  std::vector<T> out;
  std::vector<std::int32_t> newIndex(items.size(), -1);
  std::vector<std::uint8_t> visited(items.size(), 0);
  struct Frame {
    std::int32_t item;
    std::int32_t newParent;
    std::uint32_t depth;
  };
  std::vector<Frame> stack;
  for (auto it = roots.rbegin(); it != roots.rend(); ++it) stack.push_back(Frame{*it, -1, 0});
  while (!stack.empty()) {
    const Frame f = stack.back();
    stack.pop_back();
    const std::size_t i = static_cast<std::size_t>(f.item);
    if (visited[i] != 0u) continue;  // cycles are reported by the caller
    visited[i] = 1;
    T t = items[i];
    t.parent = f.newParent;
    t.depth = f.depth;
    newIndex[i] = static_cast<std::int32_t>(out.size());
    out.push_back(std::move(t));
    const auto& c = children[i];
    for (auto it = c.rbegin(); it != c.rend(); ++it) stack.push_back(Frame{*it, newIndex[i], f.depth + 1});
  }
  return out;  // items in a parent cycle are left out
}

// Keys: see BusDesc::key.
template <class T>
void AssignKeys(std::vector<T>& items) {
  char b[48];
  for (std::size_t i = 0; i < items.size(); ++i) {
    std::uint32_t sameId = 0, sameGuid = 0;
    for (const T& o : items) {
      sameId += o.id == items[i].id ? 1u : 0u;
      sameGuid += o.guid == items[i].guid ? 1u : 0u;
    }
    if (sameId == 1u) {
      std::snprintf(b, sizeof b, "%08x", items[i].id);
    } else if (sameGuid == 1u) {
      std::snprintf(b, sizeof b, "g%016llx%016llx", static_cast<unsigned long long>(items[i].guid.hi),
                    static_cast<unsigned long long>(items[i].guid.lo));
    } else {
      std::snprintf(b, sizeof b, "n%zu", i);
    }
    items[i].key = b;
  }
}

}  // namespace

bool ParseGuid(const std::string& text, Guid* out) {
  std::string s = text;
  if (s.size() == 38 && s.front() == '{' && s.back() == '}') s = s.substr(1, 36);
  if (s.size() != 36) return false;
  std::uint64_t parts[2] = {0, 0};
  std::uint32_t digits = 0;
  for (std::size_t i = 0; i < s.size(); ++i) {
    const bool dash = i == 8 || i == 13 || i == 18 || i == 23;
    if (dash) {
      if (s[i] != '-') return false;
      continue;
    }
    const int d = HexDigit(s[i]);
    if (d < 0) return false;
    std::uint64_t& p = parts[digits / 16];
    p = (p << 4) | static_cast<std::uint64_t>(d);
    ++digits;
  }
  if (digits != 32) return false;
  out->hi = parts[0];
  out->lo = parts[1];
  return true;
}

std::uint32_t Project::errorCount() const {
  std::uint32_t n = 0;
  for (const ProjectIssue& i : issues_) n += i.error ? 1u : 0u;
  return n;
}

bool Project::Load(const std::string& dir, const std::string& skipDir) {
  dir_ = dir;
  name_.clear();
  buses_.clear();
  categories_.clear();
  parameters_.clear();
  issues_.clear();
  banks_.clear();
  buildHash_ = 0;

  std::error_code ec;
  const fs::path root(dir);
  if (!fs::is_directory(root, ec)) {
    issues_.push_back(ProjectIssue{true, "", "cannot read project directory '" + dir + "'"});
    return false;
  }
  fs::path skip;
  if (!skipDir.empty()) skip = fs::weakly_canonical(fs::path(skipDir), ec);
  std::vector<std::string> files;
  fs::recursive_directory_iterator it(root, ec), end;
  if (ec) {
    issues_.push_back(ProjectIssue{true, "", "cannot list '" + dir + "': " + ec.message()});
    return false;
  }
  while (it != end) {
    const fs::path p = it->path();
    const std::string leaf = p.filename().string();
    std::error_code e2;
    const bool isDir = it->is_directory(e2);
    if (isDir) {
      bool skipThis = !leaf.empty() && leaf[0] == '.';
      if (!skipThis && !skip.empty()) skipThis = fs::weakly_canonical(p, e2) == skip;
      if (skipThis) it.disable_recursion_pending();
    } else if (p.extension() == ".json" && (leaf.empty() || leaf[0] != '.')) {
      files.push_back(fs::relative(p, root, e2).generic_string());
    }
    it.increment(ec);
    if (ec) {
      issues_.push_back(ProjectIssue{true, "", "cannot list '" + dir + "': " + ec.message()});
      return false;
    }
  }
  std::sort(files.begin(), files.end());
  std::vector<std::pair<std::string, std::string>> texts;
  for (const std::string& rel : files) {
    std::string text;
    if (!ReadFile((root / rel).string(), &text)) {
      issues_.push_back(ProjectIssue{true, rel, "cannot read file"});
      continue;
    }
    texts.emplace_back(rel, std::move(text));
  }
  return LoadTexts(texts);
}

bool Project::Load(const ProjectDocument& doc) {
  dir_ = doc.directory();
  name_.clear();
  buses_.clear();
  categories_.clear();
  parameters_.clear();
  issues_.clear();
  banks_.clear();
  buildHash_ = 0;
  std::vector<std::pair<std::string, std::string>> texts;
  for (const DocObject& o : doc.objects()) texts.emplace_back(o.file, doc.TextOf(o));
  std::sort(texts.begin(), texts.end());
  for (const DocLoadIssue& i : doc.loadIssues()) issues_.push_back(ProjectIssue{true, i.file, i.message});
  return LoadTexts(texts);
}

bool Project::LoadTexts(const std::vector<std::pair<std::string, std::string>>& texts) {
  std::vector<RawObject> raw;
  for (const auto& [rel, textIn] : texts) {
    std::string text = textIn;
    if (text.size() >= 3 && static_cast<unsigned char>(text[0]) == 0xEF && static_cast<unsigned char>(text[1]) == 0xBB &&
        static_cast<unsigned char>(text[2]) == 0xBF) {
      text.erase(0, 3);
    }
    RawObject o;
    o.file = rel;
    json::ParseError pe;
    if (!json::Parse(text, &o.doc, &pe)) {
      issues_.push_back(ProjectIssue{true, rel + ":" + std::to_string(pe.line) + ":" + std::to_string(pe.column),
                                     pe.message});
      continue;
    }
    if (!o.doc.IsObject()) {
      issues_.push_back(ProjectIssue{true, rel, "not a JSON object"});
      continue;
    }
    o.type = String(o.doc.Find("type"));
    o.name = String(o.doc.Find("name"));
    const json::Value* g = o.doc.Find("guid");
    if (o.type.empty()) {
      issues_.push_back(ProjectIssue{true, rel, "missing \"type\""});
      continue;
    }
    if (g == nullptr || !g->IsString() || !ParseGuid(g->text, &o.guid)) {
      issues_.push_back(ProjectIssue{true, rel, "missing or malformed \"guid\""});
      continue;
    }
    raw.push_back(std::move(o));
  }

  // --- buses ------------------------------------------------------------------------------
  std::vector<BusDesc> buses;
  std::vector<const RawObject*> busRaw;
  for (const RawObject& o : raw) {
    if (o.type == "project") name_ = o.name;
    if (o.type != "bus") continue;
    BusDesc b;
    b.name = o.name;
    b.id = HashId(o.name);
    b.guid = o.guid;
    b.file = o.file;
    const json::Value* m = o.doc.Find("master");
    b.master = m != nullptr && m->IsBool() && m->boolean;
    b.volumeDb = Number(o.doc.Find("volumeDb"), 0.0f);
    b.channels = ClampToU32(Number(o.doc.Find("channels"), 0.0f));  // a broken file's -5: 0 (D-187)
    if (const json::Value* fx = o.doc.Find("effects"); fx != nullptr && fx->IsArray()) {
      for (const json::Value& e : fx->items) {
        EffectDesc d;
        d.type = String(e.Find("type"));
        d.typeId = HashId(d.type);
        const dsp::EffectInfo* info = dsp::FindEffect(d.typeId);
        d.known = info != nullptr;
        std::vector<float> values;
        if (const json::Value* ps = e.Find("params"); ps != nullptr && ps->IsArray()) {
          for (const json::Value& p : ps->items) values.push_back(Number(&p, 0.0f));
        }
        const std::size_t count = info != nullptr ? std::max<std::size_t>(info->paramCount, values.size()) : values.size();
        for (std::size_t i = 0; i < count; ++i) {
          EffectParamDesc pd;
          if (info != nullptr && i < info->paramCount) {
            const dsp::EffectParamInfo& pi = info->params[i];
            pd.name = pi.name;
            pd.unit = pi.unit;
            pd.minValue = pi.minValue;
            pd.maxValue = pi.maxValue;
            pd.dspDefault = pi.defaultValue;
            pd.integer = (pi.flags & dsp::kParamInteger) != 0u;
            pd.initOnly = (pi.flags & dsp::kParamInitOnly) != 0u;
          } else {
            pd.name = "P" + std::to_string(i);
            const float v = i < values.size() ? values[i] : 0.0f;
            pd.minValue = std::min(0.0f, v);
            pd.maxValue = std::max(1.0f, v);
            pd.dspDefault = 0.0f;
          }
          pd.projectValue = i < values.size() ? values[i] : pd.dspDefault;
          pd.logScale = pd.unit == "Hz" && pd.minValue > 0.0f;
          d.params.push_back(pd);
        }
        b.effects.push_back(std::move(d));
      }
    }
    buses.push_back(std::move(b));
    busRaw.push_back(&o);
  }
  std::vector<std::int32_t> busParent(buses.size(), -1);
  for (std::size_t i = 0; i < buses.size(); ++i) {
    const json::Value* p = busRaw[i]->doc.Find("parent");
    if (p == nullptr) continue;
    const std::int32_t pi = p->IsString() ? Resolve(buses, p->text) : -1;
    if (pi < 0) {
      issues_.push_back(ProjectIssue{true, At(*busRaw[i], *p), "unknown bus '" + String(p) + "'"});
    }
    busParent[i] = pi;
  }
  std::uint32_t roots = 0;
  for (std::size_t i = 0; i < buses.size(); ++i) {
    if (busParent[i] < 0 && busRaw[i]->doc.Find("parent") == nullptr) {
      ++roots;
      buses[i].master = true;
    }
  }
  if (roots > 1) issues_.push_back(ProjectIssue{true, "", "more than one root bus (master)"});
  buses_ = TreeOrder(buses, busParent);
  if (buses_.size() != buses.size()) issues_.push_back(ProjectIssue{true, "", "bus parent cycle"});

  // --- categories -------------------------------------------------------------------------
  std::vector<CategoryDesc> cats;
  std::vector<const RawObject*> catRaw;
  for (const RawObject& o : raw) {
    if (o.type != "category") continue;
    CategoryDesc c;
    c.name = o.name;
    c.id = HashId(o.name);
    c.guid = o.guid;
    c.volumeDb = Number(o.doc.Find("volumeDb"), 0.0f);
    cats.push_back(std::move(c));
    catRaw.push_back(&o);
  }
  std::vector<std::int32_t> catParent(cats.size(), -1);
  for (std::size_t i = 0; i < cats.size(); ++i) {
    const json::Value* p = catRaw[i]->doc.Find("parent");
    if (p == nullptr) continue;
    catParent[i] = p->IsString() ? Resolve(cats, p->text) : -1;
    if (catParent[i] < 0) {
      issues_.push_back(ProjectIssue{true, At(*catRaw[i], *p), "unknown category '" + String(p) + "'"});
    }
  }
  categories_ = TreeOrder(cats, catParent);
  if (categories_.size() != cats.size()) issues_.push_back(ProjectIssue{true, "", "category parent cycle"});

  // --- parameters -------------------------------------------------------------------------
  for (const RawObject& o : raw) {
    if (o.type != "parameter") continue;
    ParameterDesc p;
    p.name = o.name;
    p.id = HashId(o.name);
    p.guid = o.guid;
    p.isSwitch = String(o.doc.Find("kind")) == "switch";
    if (const json::Value* s = o.doc.Find("scope"); s != nullptr && s->IsArray()) {
      p.global = false;
      for (const json::Value& v : s->items) p.global = p.global || (v.IsString() && v.text == "global");
    }
    p.minValue = Number(o.doc.Find("min"), 0.0f);
    p.maxValue = Number(o.doc.Find("max"), p.isSwitch ? 0.0f : 1.0f);
    p.defaultValue = Number(o.doc.Find("default"), p.minValue);
    parameters_.push_back(std::move(p));
  }
  std::stable_sort(parameters_.begin(), parameters_.end(),
                   [](const ParameterDesc& a, const ParameterDesc& b) { return a.name < b.name; });
  AssignKeys(buses_);
  AssignKeys(categories_);
  AssignKeys(parameters_);
  return errorCount() == 0;
}

bool Project::Build(const std::string& outDir) {
  buildtool::BuildOptions o;
  o.projectDir = dir_;
  o.outDir = outDir;
  o.devBuild = true;
  o.idsHeader.clear();
  buildtool::Diagnostics diag;
  buildtool::BuildResult r;
  const bool ok = buildtool::Build(o, diag, &r);
  for (const buildtool::Diagnostic& d : diag.items()) {
    std::string where = d.file;
    if (d.line > 0) where += ":" + std::to_string(d.line) + ":" + std::to_string(d.column);
    issues_.push_back(ProjectIssue{d.severity == buildtool::Severity::Error, where, d.message});
  }
  if (!ok) return false;
  buildHash_ = r.projectBuildHash;
  banks_.clear();
  for (const buildtool::BuiltBank& b : r.banks) {
    banks_.push_back(BankFile{b.name, (fs::path(outDir) / b.path).string(), b.bankId});
  }
  return true;
}

std::int32_t Project::FindBus(Id id) const {
  for (std::size_t i = 0; i < buses_.size(); ++i) {
    if (buses_[i].id == id) return static_cast<std::int32_t>(i);
  }
  return -1;
}
std::int32_t Project::FindCategory(Id id) const {
  for (std::size_t i = 0; i < categories_.size(); ++i) {
    if (categories_[i].id == id) return static_cast<std::int32_t>(i);
  }
  return -1;
}
std::int32_t Project::FindParameter(Id id) const {
  for (std::size_t i = 0; i < parameters_.size(); ++i) {
    if (parameters_[i].id == id) return static_cast<std::int32_t>(i);
  }
  return -1;
}
bool Project::IsBusUnder(std::int32_t bus, std::int32_t ancestor) const {
  for (std::int32_t b = bus; b >= 0 && static_cast<std::size_t>(b) < buses_.size(); b = buses_[static_cast<std::size_t>(b)].parent) {
    if (b == ancestor) return true;
  }
  return false;
}

}  // namespace snd::tool
