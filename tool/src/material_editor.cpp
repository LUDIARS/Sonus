// Material table editor model (material_editor.hpp).
#include "snd/tool/material_editor.hpp"

#include <algorithm>
#include <cmath>

namespace snd::tool {
namespace {

using json::Value;
const char* const kKinds[] = {"impact", "slide", "roll", "break"};

Value* Member(Value& o, const std::string& k) {
  if (!o.IsObject()) return nullptr;
  for (json::Member& m : o.members) {
    if (m.first == k) return &m.second;
  }
  return nullptr;
}
void EraseMember(Value& o, const std::string& k) {
  for (std::size_t i = 0; i < o.members.size(); ++i) {
    if (o.members[i].first == k) {
      o.members.erase(o.members.begin() + static_cast<std::ptrdiff_t>(i));
      return;
    }
  }
}
// Renames / removes a key of a free-key object (single, footsteps, curves).
void RekeyObject(Value* o, const std::string& from, const std::string& to) {
  if (o == nullptr || !o->IsObject()) return;
  for (json::Member& m : o->members) {
    if (m.first == from) {
      if (to.empty()) {
        EraseMember(*o, from);
      } else {
        m.first = to;
      }
      return;
    }
  }
}

Renumbering Map(const std::vector<std::string>& before, const std::vector<std::string>& after) {
  Renumbering r;
  for (const std::string& n : before) {
    int to = -1;
    for (std::size_t i = 0; i < after.size(); ++i) {
      if (after[i] == n) to = static_cast<int>(i);
    }
    r.oldToNew.push_back(to);
  }
  return r;
}

}  // namespace

const char* ContactName(ContactKind k) { return kKinds[static_cast<int>(k)]; }

StrengthResult EvaluateStrength(const StrengthCurves& c, float impulse, float massKg, float speed) {
  StrengthResult r;
  const float* t = c.impulseToVelocity;
  if (!(impulse >= t[0])) {
    r.layer = -1;
  } else if (impulse < t[1]) {
    r.layer = 0;
  } else if (impulse < t[2]) {
    r.layer = 1;
  } else {
    r.layer = 2;
  }
  r.velocity = t[3] > 0.0f ? std::clamp(impulse / t[3], 0.0f, 1.0f) : 1.0f;
  const float m = massKg > 0.0f ? std::clamp(std::log10(massKg) / 2.0f, 0.0f, 1.0f) : 0.0f;  // 1 kg .. 100 kg
  r.pitchCents = c.massToPitchCents[0] + (c.massToPitchCents[1] - c.massToPitchCents[0]) * m;
  const float s = speed > 0.0f ? std::clamp((std::log10(speed) + 1.0f) / 2.0f, 0.0f, 1.0f) : 0.0f;  // 0.1 .. 10 m/s
  r.volumeDb = c.speedToVolumeDb[0] + (c.speedToVolumeDb[1] - c.speedToVolumeDb[0]) * s;
  return r;
}

MaterialEditor::MaterialEditor(ProjectDocument& doc, std::string guid) : doc_(doc), guid_(NormalizeGuid(guid)) {}
bool MaterialEditor::valid() const {
  const DocObject* o = doc_.Find(guid_);
  return o != nullptr && o->kind() == "materialTable";
}
std::vector<std::string> MaterialEditor::Materials() const {
  std::vector<std::string> out;
  const Value* m = valid() ? doc_.Find(guid_)->doc.Find("materials") : nullptr;
  if (m == nullptr || !m->IsArray()) return out;
  for (const Value& v : m->items) out.push_back(v.IsString() ? v.text : std::string());
  return out;
}
int MaterialEditor::IndexOf(const std::string& n) const {
  const std::vector<std::string> m = Materials();
  for (std::size_t i = 0; i < m.size(); ++i) {
    if (m[i] == n) return static_cast<int>(i);
  }
  return -1;
}
std::string MaterialEditor::EventRef(const std::string& ref) const {
  const DocObject* e = doc_.Resolve("event", ref);
  return e != nullptr ? e->guid : std::string();
}

bool MaterialEditor::AddMaterial(const std::string& name, std::uint32_t at, Renumbering* r) {
  const std::vector<std::string> before = Materials();
  if (!valid() || name.empty() || name == "notes" || IndexOf(name) >= 0 || at > before.size()) return false;
  const bool ok = doc_.Insert(guid_, "materials", at, String(name));
  if (ok && r != nullptr) *r = Map(before, Materials());
  return ok;
}
bool MaterialEditor::RemoveMaterial(const std::string& name, Renumbering* r) {
  const std::vector<std::string> before = Materials();
  const int i = IndexOf(name);
  if (i < 0) return false;
  const bool ok = doc_.Modify(guid_, [&](Value& d) {
    Value* m = Member(d, "materials");
    m->items.erase(m->items.begin() + i);
    for (const char* k : {"single", "footsteps", "curves"}) RekeyObject(Member(d, k), name, std::string());
    if (Value* pairs = Member(d, "pairs"); pairs != nullptr && pairs->IsArray()) {
      for (std::size_t p = pairs->items.size(); p-- > 0;) {
        if (StringAt(pairs->items[p], "a") == name || StringAt(pairs->items[p], "b") == name) {
          pairs->items.erase(pairs->items.begin() + static_cast<std::ptrdiff_t>(p));
        }
      }
    }
    return true;
  });
  if (ok && r != nullptr) *r = Map(before, Materials());
  return ok;
}
bool MaterialEditor::MoveMaterial(std::uint32_t from, std::uint32_t to, Renumbering* r) {
  const std::vector<std::string> before = Materials();
  const bool ok = doc_.MoveElement(guid_, "materials", from, to);
  if (ok && r != nullptr) *r = Map(before, Materials());
  return ok;
}
bool MaterialEditor::RenameMaterial(const std::string& from, const std::string& to) {
  const int i = IndexOf(from);
  if (i < 0 || to.empty() || to == "notes" || IndexOf(to) >= 0) return false;
  return doc_.Modify(guid_, [&](Value& d) {
    Member(d, "materials")->items[static_cast<std::size_t>(i)] = String(to);
    for (const char* k : {"single", "footsteps", "curves"}) RekeyObject(Member(d, k), from, to);
    if (Value* pairs = Member(d, "pairs"); pairs != nullptr && pairs->IsArray()) {
      for (Value& p : pairs->items) {
        for (const char* k : {"a", "b"}) {
          if (Value* x = Member(p, k); x != nullptr && x->IsString() && x->text == from) *x = String(to);
        }
      }
    }
    return true;
  });
}

bool MaterialEditor::SetSingle(const std::string& material, ContactKind k, const std::string& ev) {
  if (IndexOf(material) < 0) return false;
  if (ev.empty()) {
    const Value* s = doc_.Find(guid_)->doc.Find("single");
    const Value* m = s != nullptr ? s->Find(material) : nullptr;
    if (m == nullptr || m->Find(ContactName(k)) == nullptr) return false;
    return doc_.Modify(guid_, [&](Value& d) {
      Value* mm = Member(*Member(d, "single"), material);
      EraseMember(*mm, ContactName(k));
      if (mm->members.empty()) EraseMember(*Member(d, "single"), material);
      return true;
    });
  }
  const DocObject* e = doc_.Find(ev);
  if (e == nullptr || e->kind() != "event") return false;
  // "single.<material>" may hold a material named like a path character; set by Modify.
  const std::string g = e->guid;
  return doc_.Modify(guid_, [&](Value& d) {
    Value* single = Member(d, "single");
    if (single == nullptr) {
      SetMember(d, "single", json::MakeObject(), SpecMemberOrder("materialTable", ""));
      single = Member(d, "single");
    }
    Value* m = Member(*single, material);
    if (m == nullptr) {
      SetMember(*single, material, json::MakeObject(), nullptr);
      m = Member(*single, material);
    }
    SetMember(*m, ContactName(k), String(g), SpecMemberOrder("materialTable", "single.x"));
    return true;
  });
}
bool MaterialEditor::SetPair(const std::string& a, const std::string& b, const std::string& ev) {
  if (IndexOf(a) < 0 || IndexOf(b) < 0) return false;
  std::string g;
  if (!ev.empty()) {
    const DocObject* e = doc_.Find(ev);
    if (e == nullptr || e->kind() != "event") return false;
    g = e->guid;
  }
  return doc_.Modify(guid_, [&](Value& d) {
    Value* pairs = Member(d, "pairs");
    if (pairs == nullptr) {
      if (g.empty()) return false;
      SetMember(d, "pairs", json::MakeArray(), SpecMemberOrder("materialTable", ""));
      pairs = Member(d, "pairs");
    }
    if (!pairs->IsArray()) return false;
    bool found = false;
    for (std::size_t p = pairs->items.size(); p-- > 0;) {
      const std::string pa = StringAt(pairs->items[p], "a"), pb = StringAt(pairs->items[p], "b");
      if ((pa == a && pb == b) || (pa == b && pb == a)) {
        if (g.empty() || found) {
          pairs->items.erase(pairs->items.begin() + static_cast<std::ptrdiff_t>(p));
        } else {
          SetMember(pairs->items[p], "event", String(g), SpecMemberOrder("materialTable", "pairs[0]"));
        }
        found = true;
      }
    }
    if (!found) {
      if (g.empty()) return false;
      Value v = json::MakeObject();
      json::Add(v, "a", String(a));
      json::Add(v, "b", String(b));
      json::Add(v, "event", String(g));
      pairs->items.push_back(v);
    }
    return true;
  });
}
bool MaterialEditor::SetFootstep(const std::string& material, const std::string& ev) {
  if (IndexOf(material) < 0) return false;
  if (ev.empty()) {
    const Value* f = doc_.Find(guid_)->doc.Find("footsteps");
    if (f == nullptr || f->Find(material) == nullptr) return false;
    return doc_.Modify(guid_, [&](Value& d) {
      EraseMember(*Member(d, "footsteps"), material);
      return true;
    });
  }
  const DocObject* e = doc_.Find(ev);
  if (e == nullptr || e->kind() != "event") return false;
  const std::string g = e->guid;
  return doc_.Modify(guid_, [&](Value& d) {
    Value* f = Member(d, "footsteps");
    if (f == nullptr) {
      SetMember(d, "footsteps", json::MakeObject(), SpecMemberOrder("materialTable", ""));
      f = Member(d, "footsteps");
    }
    SetMember(*f, material, String(g), nullptr);
    return true;
  });
}
bool MaterialEditor::SetDefault(ContactKind k, const std::string& ev) {
  if (!valid()) return false;
  const std::string path = std::string("defaults.") + ContactName(k);
  if (ev.empty()) return FindPath(doc_.Find(guid_)->doc, path) != nullptr && doc_.Erase(guid_, path);
  const DocObject* e = doc_.Find(ev);
  return e != nullptr && e->kind() == "event" && doc_.Set(guid_, path, String(e->guid));
}
bool MaterialEditor::SetCurves(const std::string& material, const StrengthCurves& c) {
  if (IndexOf(material) < 0) return false;
  const float* t = c.impulseToVelocity;
  for (float v : {t[0], t[1], t[2], t[3], c.massToPitchCents[0], c.massToPitchCents[1], c.speedToVolumeDb[0], c.speedToVolumeDb[1]}) {
    if (!std::isfinite(v)) return false;
  }
  if (!(t[0] >= 0.0f && t[0] <= t[1] && t[1] <= t[2] && t[2] <= t[3] && t[3] > 0.0f)) return false;
  Value v = json::MakeObject();
  auto arr = [](const float* p, int n) {
    Value a = json::MakeArray();
    for (int i = 0; i < n; ++i) a.items.push_back(Number(p[i]));
    return a;
  };
  json::Add(v, "impulseToVelocity", arr(t, 4));
  json::Add(v, "massToPitchCents", arr(c.massToPitchCents, 2));
  json::Add(v, "speedToVolumeDb", arr(c.speedToVolumeDb, 2));
  return doc_.Modify(guid_, [&](Value& d) {
    Value* cv = Member(d, "curves");
    if (cv == nullptr) {
      SetMember(d, "curves", json::MakeObject(), SpecMemberOrder("materialTable", ""));
      cv = Member(d, "curves");
    }
    SetMember(*cv, material, v, nullptr);
    return true;
  });
}
StrengthCurves MaterialEditor::Curves(const std::string& material) const {
  StrengthCurves c;
  const Value* cv = valid() ? doc_.Find(guid_)->doc.Find("curves") : nullptr;
  const Value* m = cv != nullptr ? cv->Find(material) : nullptr;
  if (m == nullptr) return c;
  for (int i = 0; i < 4; ++i) c.impulseToVelocity[i] = static_cast<float>(NumberAt(*m, "impulseToVelocity[" + std::to_string(i) + "]", c.impulseToVelocity[i]));
  for (int i = 0; i < 2; ++i) {
    c.massToPitchCents[i] = static_cast<float>(NumberAt(*m, "massToPitchCents[" + std::to_string(i) + "]", c.massToPitchCents[i]));
    c.speedToVolumeDb[i] = static_cast<float>(NumberAt(*m, "speedToVolumeDb[" + std::to_string(i) + "]", c.speedToVolumeDb[i]));
  }
  return c;
}

std::string MaterialEditor::Single(const std::string& material, ContactKind k) const {
  const Value* s = valid() ? doc_.Find(guid_)->doc.Find("single") : nullptr;
  const Value* m = s != nullptr ? s->Find(material) : nullptr;
  const Value* e = m != nullptr ? m->Find(ContactName(k)) : nullptr;
  return e != nullptr && e->IsString() ? EventRef(e->text) : std::string();
}
std::string MaterialEditor::Pair(const std::string& a, const std::string& b) const {
  const Value* p = valid() ? doc_.Find(guid_)->doc.Find("pairs") : nullptr;
  if (p == nullptr || !p->IsArray()) return std::string();
  for (const Value& v : p->items) {
    const std::string pa = StringAt(v, "a"), pb = StringAt(v, "b");
    if ((pa == a && pb == b) || (pa == b && pb == a)) return EventRef(StringAt(v, "event"));
  }
  return std::string();
}
std::string MaterialEditor::Footstep(const std::string& material) const {
  const Value* f = valid() ? doc_.Find(guid_)->doc.Find("footsteps") : nullptr;
  const Value* e = f != nullptr ? f->Find(material) : nullptr;
  return e != nullptr && e->IsString() ? EventRef(e->text) : std::string();
}
std::string MaterialEditor::Default(ContactKind k) const {
  return valid() ? EventRef(StringAt(doc_.Find(guid_)->doc, std::string("defaults.") + ContactName(k))) : std::string();
}

MaterialResolution MaterialEditor::Resolve(std::uint32_t a, std::uint32_t b, ContactKind k) const {
  MaterialResolution r;
  r.a = std::min(a, b);
  r.b = std::max(a, b);
  r.kind = k;
  const std::vector<std::string> m = Materials();
  if (r.b >= m.size()) {
    r.source = "none";
    return r;
  }
  const std::string pair = Pair(m[r.a], m[r.b]);
  if (!pair.empty()) {
    r.events.push_back(pair);
    r.source = "pair";
    return r;
  }
  const std::string def = Default(k);
  bool usedDefault = false, usedSingle = false;
  for (std::uint32_t x : {r.a, r.b}) {
    std::string e = Single(m[x], k);
    if (e.empty()) {
      e = def;
      usedDefault = usedDefault || !e.empty();
    } else {
      usedSingle = true;
    }
    if (!e.empty() && std::find(r.events.begin(), r.events.end(), e) == r.events.end()) r.events.push_back(e);
    if (r.a == r.b) break;
  }
  r.source = usedSingle && usedDefault ? "single+default" : usedSingle ? "single" : usedDefault ? "default" : "none";
  return r;
}

std::vector<MaterialResolution> MaterialEditor::Fallbacks() const {
  std::vector<MaterialResolution> out;
  const std::uint32_t n = static_cast<std::uint32_t>(Materials().size());
  for (std::uint32_t a = 0; a < n; ++a) {
    for (std::uint32_t b = a; b < n; ++b) {
      for (int k = 0; k < 4; ++k) {
        MaterialResolution r = Resolve(a, b, static_cast<ContactKind>(k));
        if (r.source != "pair" && r.source != "single") out.push_back(std::move(r));
      }
    }
  }
  return out;
}

}  // namespace snd::tool
