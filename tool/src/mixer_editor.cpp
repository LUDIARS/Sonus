// Mixer structure model (mixer_editor.hpp).
#include "snd/tool/mixer_editor.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>
#include <set>

#include "snd/dsp/effects.hpp"

namespace snd::tool {
namespace {

using json::Value;

const char* const kStandardEffects[] = {"snd.fx.reverb",     "snd.fx.echo",       "snd.fx.delay",    "snd.fx.chorus",
                                        "snd.fx.flanger",    "snd.fx.distortion", "snd.fx.compressor", "snd.fx.limiter",
                                        "snd.fx.parameq",    "snd.fx.lowpass",    "snd.fx.highpass", "snd.fx.pitchshift",
                                        "snd.fx.normalize",  "snd.fx.surround"};
const char* const kSnapKinds[] = {"busVolume", "effectParam", "sendLevel"};
const char* const kSteal[] = {"oldest", "quietest", "lowestPriority", "never"};

std::string GuidText(const Guid& g) {
  char b[40];
  std::snprintf(b, sizeof b, "%08x-%04x-%04x-%04x-%012llx", static_cast<unsigned>(g.hi >> 32),
                static_cast<unsigned>((g.hi >> 16) & 0xFFFF), static_cast<unsigned>(g.hi & 0xFFFF),
                static_cast<unsigned>(g.lo >> 48), static_cast<unsigned long long>(g.lo & 0xFFFFFFFFFFFFull));
  return b;
}

std::string P(const char* a, std::uint32_t i, const char* rest = "") {
  return std::string(a) + "[" + std::to_string(i) + "]" + rest;
}

std::size_t ArraySize(const Value& o, const char* key) {
  const Value* a = o.Find(key);
  return a != nullptr && a->IsArray() ? a->items.size() : 0u;
}

bool Finite(double v) { return std::isfinite(v); }

}  // namespace

std::vector<EffectTypeInfo> StandardEffectTypes() {
  std::vector<EffectTypeInfo> out;
  for (const char* n : kStandardEffects) {
    const dsp::EffectInfo* info = dsp::FindEffect(HashId(n));
    if (info == nullptr) continue;
    EffectTypeInfo t;
    t.name = n;
    for (std::uint32_t i = 0; i < info->paramCount; ++i) {
      const dsp::EffectParamInfo& pi = info->params[i];
      EffectParamDesc d;
      d.name = pi.name;
      d.unit = pi.unit;
      d.minValue = pi.minValue;
      d.maxValue = pi.maxValue;
      d.dspDefault = pi.defaultValue;
      d.projectValue = pi.defaultValue;
      d.integer = (pi.flags & dsp::kParamInteger) != 0u;
      d.initOnly = (pi.flags & dsp::kParamInitOnly) != 0u;
      d.logScale = d.unit == "Hz" && d.minValue > 0.0f;
      t.params.push_back(d);
    }
    out.push_back(std::move(t));
  }
  std::sort(out.begin(), out.end(), [](const EffectTypeInfo& a, const EffectTypeInfo& b) { return a.name < b.name; });
  return out;
}

double MixerEditor::NormalizeEffectParam(const std::string& type, std::uint32_t p, double v) {
  const dsp::EffectInfo* info = dsp::FindEffect(HashId(type));
  if (info == nullptr || p >= info->paramCount) return v;
  const dsp::EffectParamInfo& pi = info->params[p];
  // In float, as MixerModel::Normalize (the bank stores float32).
  float f = std::clamp(static_cast<float>(v), pi.minValue, pi.maxValue);
  if ((pi.flags & dsp::kParamInteger) != 0u) f = std::round(f);
  return f;
}

// --- buses ----------------------------------------------------------------------------------
const DocObject* MixerEditor::Bus(const std::string& g) const {
  const DocObject* o = doc_.Find(g);
  return o != nullptr && o->kind() == "bus" ? o : nullptr;
}

std::string MixerEditor::ParentOf(const DocObject& bus) const {
  const Value* p = bus.doc.Find("parent");
  if (p == nullptr || !p->IsString()) return std::string();
  const DocObject* o = doc_.Resolve("bus", p->text);
  return o != nullptr ? o->guid : std::string();
}

std::string MixerEditor::MasterBus() const {
  for (const DocObject* b : doc_.OfKind("bus")) {
    if (b->doc.Find("parent") == nullptr) return b->guid;
  }
  return std::string();
}

std::vector<std::string> MixerEditor::Children(const std::string& bus) const {
  std::vector<std::string> out;
  const std::string g = NormalizeGuid(bus);
  for (const DocObject* b : doc_.OfKind("bus")) {
    if (ParentOf(*b) == g) out.push_back(b->guid);
  }
  return out;
}

// Edges: bus -> parent, bus -> send targets (with an optional changed parent / extra send).
bool MixerEditor::BusGraphAcyclic(const std::string& bus, const std::string& newParent,
                                  const std::string& extraSendTarget) const {
  std::map<std::string, std::vector<std::string>> out;
  for (const DocObject* b : doc_.OfKind("bus")) {
    std::vector<std::string>& e = out[b->guid];
    const std::string parent = b->guid == bus && !newParent.empty() ? newParent : ParentOf(*b);
    if (!parent.empty()) e.push_back(parent);
    if (const Value* s = b->doc.Find("sends"); s != nullptr && s->IsArray()) {
      for (const Value& sv : s->items) {
        const DocObject* t = doc_.Resolve("bus", StringAt(sv, "bus"));
        if (t != nullptr) e.push_back(t->guid);
      }
    }
    if (b->guid == bus && !extraSendTarget.empty()) e.push_back(extraSendTarget);
  }
  std::map<std::string, int> state;  // 0 new, 1 on stack, 2 done
  for (const auto& [start, unused] : out) {
    (void)unused;
    if (state[start] != 0) continue;
    std::vector<std::pair<std::string, std::size_t>> stack{{start, 0}};
    state[start] = 1;
    while (!stack.empty()) {
      auto& [node, next] = stack.back();
      const std::vector<std::string>& es = out[node];
      if (next < es.size()) {
        const std::string to = es[next++];
        if (state[to] == 1) return false;
        if (state[to] == 0) {
          state[to] = 1;
          stack.emplace_back(to, 0);
        }
      } else {
        state[node] = 2;
        stack.pop_back();
      }
    }
  }
  return true;
}

std::vector<std::string> MixerEditor::GraphOrder() const {
  // Kahn: a bus comes after every bus that feeds it (children, send sources); ties by name.
  std::vector<const DocObject*> buses = doc_.OfKind("bus");
  std::map<std::string, std::vector<std::string>> outs;
  std::map<std::string, std::uint32_t> indeg;
  std::map<std::string, std::string> names;
  for (const DocObject* b : buses) {
    indeg[b->guid];
    names[b->guid] = b->name();
  }
  for (const DocObject* b : buses) {
    std::vector<std::string> targets;
    if (const std::string p = ParentOf(*b); !p.empty()) targets.push_back(p);
    if (const Value* s = b->doc.Find("sends"); s != nullptr && s->IsArray()) {
      for (const Value& sv : s->items) {
        const DocObject* t = doc_.Resolve("bus", StringAt(sv, "bus"));
        if (t != nullptr) targets.push_back(t->guid);
      }
    }
    for (const std::string& t : targets) {
      outs[b->guid].push_back(t);
      ++indeg[t];
    }
  }
  std::vector<std::string> ready, order;
  for (const auto& [g, n] : indeg) {
    if (n == 0u) ready.push_back(g);
  }
  auto byName = [&](const std::string& a, const std::string& b) {
    return names[a] != names[b] ? names[a] > names[b] : a > b;  // smallest last (popped first)
  };
  while (!ready.empty()) {
    std::sort(ready.begin(), ready.end(), byName);
    const std::string x = ready.back();
    ready.pop_back();
    order.push_back(x);
    for (const std::string& t : outs[x]) {
      if (--indeg[t] == 0u) ready.push_back(t);
    }
  }
  if (order.size() != buses.size()) order.clear();
  return order;
}

std::string MixerEditor::AddBus(const std::string& name, const std::string& parentGuid) {
  const std::string master = MasterBus();
  Value init = json::MakeObject();
  if (parentGuid.empty()) {
    if (!master.empty()) return std::string();  // a second root
    json::Add(init, "master", Bool(true));
  } else {
    const DocObject* p = Bus(parentGuid);
    if (p == nullptr) return std::string();
    json::Add(init, "parent", String(p->guid));
  }
  return doc_.Create("bus", name, &init);
}

bool MixerEditor::RemoveBus(const std::string& bus) {
  const DocObject* b = Bus(bus);
  if (b == nullptr || !Children(bus).empty()) return false;
  return doc_.Remove(b->guid);  // refused while referenced (events, sends, snapshots, ducking)
}

bool MixerEditor::SetParent(const std::string& bus, const std::string& parentGuid) {
  const DocObject* b = Bus(bus);
  const DocObject* p = Bus(parentGuid);
  if (b == nullptr || p == nullptr || b->doc.Find("parent") == nullptr) return false;  // the master stays the root
  if (ParentOf(*b) == p->guid) return true;  // D-177b: already its parent (by name or GUID)
  if (!BusGraphAcyclic(b->guid, p->guid, std::string())) return false;
  return doc_.Set(b->guid, "parent", String(p->guid));
}

bool MixerEditor::SetVolume(const std::string& bus, double db, std::uint32_t mergeKey) {
  if (Bus(bus) == nullptr || !Finite(db)) return false;
  return doc_.SetDefaulted(bus, "volumeDb", Number(std::clamp(db, static_cast<double>(kFaderOffDb), static_cast<double>(kFaderMaxDb))),
                           Number(0), mergeKey);
}
bool MixerEditor::SetChannels(const std::string& bus, std::uint32_t channels) {
  return Bus(bus) != nullptr && channels <= 8u && doc_.SetDefaulted(bus, "channels", Number(channels), Number(0));
}
bool MixerEditor::SetFlag(const std::string& bus, BusFlag f, bool on) {
  const DocObject* b = Bus(bus);
  if (b == nullptr) return false;
  const char* key = f == BusFlag::Output ? "output" : f == BusFlag::LodBypassable ? "lodBypassable" : "meter";
  if (BoolAt(b->doc, key, false) == on) return true;  // D-177b (an explicit false stays)
  if (!on) return doc_.Erase(bus, key);
  return doc_.Set(bus, key, Bool(true));
}
bool MixerEditor::SetLodPriority(const std::string& bus, std::uint32_t priority) {
  return Bus(bus) != nullptr && doc_.SetDefaulted(bus, "lodPriority", Number(priority), Number(0));
}

// --- sends ----------------------------------------------------------------------------------
bool MixerEditor::AddSend(const std::string& bus, const std::string& target, double levelDb, bool preFader) {
  const DocObject* b = Bus(bus);
  const DocObject* t = Bus(target);
  if (b == nullptr || t == nullptr || b->guid == t->guid || !Finite(levelDb)) return false;
  if (!BusGraphAcyclic(b->guid, std::string(), t->guid)) return false;
  Value s = json::MakeObject();
  json::Add(s, "bus", String(t->guid));
  json::Add(s, "levelDb", Number(levelDb));
  if (preFader) json::Add(s, "preFader", Bool(true));
  return doc_.Insert(b->guid, "sends", ArraySize(b->doc, "sends"), s);
}
bool MixerEditor::RemoveSend(const std::string& bus, std::uint32_t i) {
  const DocObject* b = Bus(bus);
  return b != nullptr && i < ArraySize(b->doc, "sends") && doc_.Erase(bus, P("sends", i));
}
bool MixerEditor::SetSendLevel(const std::string& bus, std::uint32_t i, double db, std::uint32_t mergeKey) {
  const DocObject* b = Bus(bus);
  return b != nullptr && i < ArraySize(b->doc, "sends") && Finite(db) &&
         doc_.SetDefaulted(bus, P("sends", i, ".levelDb"), Number(std::max(db, static_cast<double>(kFaderOffDb))), Number(0), mergeKey);
}
bool MixerEditor::SetSendPreFader(const std::string& bus, std::uint32_t i, bool preFader) {
  const DocObject* b = Bus(bus);
  if (b == nullptr || i >= ArraySize(b->doc, "sends")) return false;
  if (BoolAt(b->doc, P("sends", i, ".preFader"), false) == preFader) return true;  // D-177b
  if (!preFader) return doc_.Erase(bus, P("sends", i, ".preFader"));
  return doc_.Set(bus, P("sends", i, ".preFader"), Bool(true));
}
bool MixerEditor::SetSendTarget(const std::string& bus, std::uint32_t i, const std::string& target) {
  const DocObject* b = Bus(bus);
  const DocObject* t = Bus(target);
  if (b == nullptr || t == nullptr || b->guid == t->guid || i >= ArraySize(b->doc, "sends")) return false;
  if (const DocObject* cur = doc_.Resolve("bus", StringAt(b->doc, P("sends", i, ".bus"))); cur != nullptr && cur->guid == t->guid) {
    return true;  // D-177b
  }
  // Check the graph with the send moved.
  ProjectDocument trial = doc_;
  if (!trial.Set(b->guid, P("sends", i, ".bus"), String(t->guid))) return false;
  if (!MixerEditor(trial).BusGraphAcyclic(b->guid, std::string(), std::string())) return false;
  return doc_.Set(b->guid, P("sends", i, ".bus"), String(t->guid));
}

// --- effects --------------------------------------------------------------------------------
bool MixerEditor::AddEffect(const std::string& bus, const std::string& type, std::uint32_t at) {
  const DocObject* b = Bus(bus);
  if (b == nullptr || type.empty() || at > ArraySize(b->doc, "effects")) return false;
  Value e = json::MakeObject();
  json::Add(e, "type", String(type));
  Value params = json::MakeArray();
  if (const dsp::EffectInfo* info = dsp::FindEffect(HashId(type))) {
    for (std::uint32_t i = 0; i < info->paramCount; ++i) params.items.push_back(Number(info->params[i].defaultValue));
  }
  json::Add(e, "params", params);
  return doc_.Insert(b->guid, "effects", at, e);
}
bool MixerEditor::RemoveEffect(const std::string& bus, std::uint32_t e) {
  const DocObject* b = Bus(bus);
  return b != nullptr && e < ArraySize(b->doc, "effects") && doc_.Erase(bus, P("effects", e));
}
bool MixerEditor::MoveEffect(const std::string& bus, std::uint32_t from, std::uint32_t to) {
  return Bus(bus) != nullptr && doc_.MoveElement(bus, "effects", from, to);
}
double MixerEditor::EffectParam(const std::string& bus, std::uint32_t e, std::uint32_t p) const {
  const DocObject* b = Bus(bus);
  if (b == nullptr) return 0.0;
  const std::string type = StringAt(b->doc, P("effects", e, ".type"));
  double def = 0.0;
  if (const dsp::EffectInfo* info = dsp::FindEffect(HashId(type)); info != nullptr && p < info->paramCount) {
    def = info->params[p].defaultValue;
  }
  return NumberAt(b->doc, P("effects", e, ".params") + "[" + std::to_string(p) + "]", def);
}
bool MixerEditor::SetEffectParam(const std::string& bus, std::uint32_t e, std::uint32_t p, double v, std::uint32_t mergeKey) {
  const DocObject* b = Bus(bus);
  if (b == nullptr || e >= ArraySize(b->doc, "effects") || !Finite(v)) return false;
  const std::string type = StringAt(b->doc, P("effects", e, ".type"));
  const dsp::EffectInfo* info = dsp::FindEffect(HashId(type));
  if (info != nullptr && p >= info->paramCount) return false;
  const double n = NormalizeEffectParam(type, p, v);
  if (EffectParam(b->guid, e, p) == n) return true;  // D-177b (the dsp default when absent)
  const std::string arrPath = P("effects", e, ".params");
  const Value* arr = FindPath(b->doc, arrPath);
  const std::size_t have = arr != nullptr && arr->IsArray() ? arr->items.size() : 0u;
  if (p < have) return doc_.SetNumber(bus, arrPath + "[" + std::to_string(p) + "]", n, mergeKey);
  // Fill the parameters before p with their current (default) values.
  return doc_.Modify(
      b->guid,
      [&](Value& d) {
        Value* fx = FindPath(d, P("effects", e));
        if (fx == nullptr || !fx->IsObject()) return false;
        Value* ps = FindPath(*fx, "params");
        if (ps == nullptr) {
          SetMember(*fx, "params", json::MakeArray(), SpecMemberOrder("bus", "effects[0]"));
          ps = FindPath(*fx, "params");
        }
        if (!ps->IsArray()) return false;
        for (std::size_t k = ps->items.size(); k < p; ++k) {
          ps->items.push_back(Number(info != nullptr ? info->params[k].defaultValue : 0.0));
        }
        ps->items.push_back(Number(n));
        return true;
      },
      mergeKey);
}
bool MixerEditor::SetEffectFlags(const std::string& bus, std::uint32_t e, std::uint32_t flags) {
  const DocObject* b = Bus(bus);
  return b != nullptr && e < ArraySize(b->doc, "effects") && doc_.SetDefaulted(bus, P("effects", e, ".flags"), Number(flags), Number(0));
}
bool MixerEditor::SetEffectBlob(const std::string& bus, std::uint32_t e, const std::string& path) {
  const DocObject* b = Bus(bus);
  if (b == nullptr || e >= ArraySize(b->doc, "effects")) return false;
  if (path.empty()) {
    const Value* cur = FindPath(b->doc, P("effects", e, ".blob"));
    return cur == nullptr || (cur->IsString() && cur->text.empty()) || doc_.Erase(bus, P("effects", e, ".blob"));
  }
  return doc_.Set(bus, P("effects", e, ".blob"), String(path));
}

// --- snapshots ------------------------------------------------------------------------------
std::string MixerEditor::AddSnapshot(const std::string& name, const std::string& bankGuid) {
  const DocObject* bank = doc_.Find(bankGuid);
  if (bank == nullptr || bank->kind() != "bank") return std::string();
  Value init = json::MakeObject();
  json::Add(init, "bank", String(bank->guid));
  json::Add(init, "values", json::MakeArray());
  return doc_.Create("snapshot", name, &init);
}

static bool MakeSnapValue(const ProjectDocument& doc, const SnapshotValue& v, Value* out) {
  const DocObject* b = doc.Find(v.bus);
  if (b == nullptr || b->kind() != "bus" || !std::isfinite(v.value)) return false;
  if (v.kind == SnapshotKind::EffectParam && v.effect >= ArraySize(b->doc, "effects")) return false;
  if (v.kind == SnapshotKind::SendLevel && v.send >= ArraySize(b->doc, "sends")) return false;
  *out = json::MakeObject();
  json::Add(*out, "kind", String(kSnapKinds[static_cast<int>(v.kind)]));
  json::Add(*out, "bus", String(b->guid));
  double value = v.value;
  if (v.kind == SnapshotKind::EffectParam) {
    value = MixerEditor::NormalizeEffectParam(StringAt(b->doc, P("effects", v.effect, ".type")), v.param, value);
  }
  json::Add(*out, "value", Number(value));
  if (v.kind == SnapshotKind::EffectParam) {
    json::Add(*out, "effect", Number(v.effect));
    json::Add(*out, "param", Number(v.param));
  }
  if (v.kind == SnapshotKind::SendLevel) json::Add(*out, "send", Number(v.send));
  return true;
}

bool MixerEditor::AddSnapshotValue(const std::string& snap, const SnapshotValue& v) {
  const DocObject* s = doc_.Find(snap);
  Value val;
  if (s == nullptr || s->kind() != "snapshot" || !MakeSnapValue(doc_, v, &val)) return false;
  return doc_.Insert(s->guid, "values", ArraySize(s->doc, "values"), val);
}
bool MixerEditor::SetSnapshotValue(const std::string& snap, std::uint32_t i, const SnapshotValue& v) {
  const DocObject* s = doc_.Find(snap);
  Value val;
  if (s == nullptr || s->kind() != "snapshot" || i >= ArraySize(s->doc, "values") || !MakeSnapValue(doc_, v, &val)) {
    return false;
  }
  {
    const SnapshotValue cur = SnapshotValues(snap)[i];
    const bool fx = v.kind == SnapshotKind::EffectParam, sd = v.kind == SnapshotKind::SendLevel;
    if (cur.kind == v.kind && cur.bus == NormalizeGuid(v.bus) && NumberAt(val, "value", 0) == cur.value &&
        (!fx || (cur.effect == v.effect && cur.param == v.param)) && (!sd || cur.send == v.send)) {
      return true;  // D-177b
    }
  }
  return doc_.Set(s->guid, P("values", i), val);
}
bool MixerEditor::RemoveSnapshotValue(const std::string& snap, std::uint32_t i) {
  const DocObject* s = doc_.Find(snap);
  return s != nullptr && i < ArraySize(s->doc, "values") && doc_.Erase(snap, P("values", i));
}
std::vector<SnapshotValue> MixerEditor::SnapshotValues(const std::string& snap) const {
  std::vector<SnapshotValue> out;
  const DocObject* s = doc_.Find(snap);
  if (s == nullptr) return out;
  const Value* vs = s->doc.Find("values");
  if (vs == nullptr || !vs->IsArray()) return out;
  for (const Value& v : vs->items) {
    SnapshotValue x;
    const std::string k = StringAt(v, "kind", "busVolume");
    for (int i = 0; i < 3; ++i) {
      if (k == kSnapKinds[i]) x.kind = static_cast<SnapshotKind>(i);
    }
    const DocObject* b = doc_.Resolve("bus", StringAt(v, "bus"));
    x.bus = b != nullptr ? b->guid : StringAt(v, "bus");
    x.value = NumberAt(v, "value", 0);
    x.effect = static_cast<std::uint32_t>(NumberAt(v, "effect", 0));
    x.param = static_cast<std::uint32_t>(NumberAt(v, "param", 0));
    x.send = static_cast<std::uint32_t>(NumberAt(v, "send", 0));
    out.push_back(x);
  }
  return out;
}

bool MixerEditor::CaptureSnapshot(const std::string& snap, const MixerModel& mixer) {
  const DocObject* s = doc_.Find(snap);
  const Project* p = mixer.project();
  if (s == nullptr || s->kind() != "snapshot" || p == nullptr) return false;
  Value values = json::MakeArray();
  for (std::size_t i = 0; i < p->buses().size() && i < mixer.buses().size(); ++i) {
    const BusDesc& bd = p->buses()[i];
    const std::string g = GuidText(bd.guid);
    if (doc_.Find(g) == nullptr) continue;
    const MixerModel::BusStrip& st = mixer.buses()[i];
    if (st.faderDb != bd.volumeDb) {
      SnapshotValue v;
      v.bus = g;
      v.value = static_cast<double>(st.faderDb) - static_cast<double>(bd.volumeDb);
      Value jv;
      if (MakeSnapValue(doc_, v, &jv)) values.items.push_back(jv);
    }
    for (std::size_t e = 0; e < bd.effects.size() && e < st.params.size(); ++e) {
      for (std::size_t q = 0; q < bd.effects[e].params.size() && q < st.params[e].size(); ++q) {
        if (bd.effects[e].params[q].initOnly || st.params[e][q] == bd.effects[e].params[q].projectValue) continue;
        SnapshotValue v;
        v.kind = SnapshotKind::EffectParam;
        v.bus = g;
        v.effect = static_cast<std::uint32_t>(e);
        v.param = static_cast<std::uint32_t>(q);
        v.value = st.params[e][q];
        Value jv;
        if (MakeSnapValue(doc_, v, &jv)) values.items.push_back(jv);
      }
    }
  }
  return doc_.Set(s->guid, "values", values);
}

std::vector<SnapshotTarget> MixerEditor::ApplySnapshots(const std::vector<std::pair<std::string, double>>& weights) const {
  std::vector<SnapshotTarget> out;
  auto find = [&](const SnapshotTarget& t) -> SnapshotTarget* {
    for (SnapshotTarget& x : out) {
      if (x.kind == t.kind && x.bus == t.bus && x.effect == t.effect && x.param == t.param && x.send == t.send) return &x;
    }
    return nullptr;
  };
  for (const auto& [snap, w0] : weights) {
    const double w = std::isfinite(w0) ? std::clamp(w0, 0.0, 1.0) : 0.0;
    for (const SnapshotValue& v : SnapshotValues(snap)) {
      const DocObject* b = doc_.Find(v.bus);
      if (b == nullptr) continue;
      SnapshotTarget t;
      t.kind = v.kind;
      t.bus = b->guid;
      if (v.kind == SnapshotKind::EffectParam) {
        t.effect = v.effect;
        t.param = v.param;
        t.projectValue = EffectParam(b->guid, v.effect, v.param);
      } else if (v.kind == SnapshotKind::SendLevel) {
        t.send = v.send;
        t.projectValue = NumberAt(b->doc, P("sends", v.send, ".levelDb"), 0);
      } else {
        t.projectValue = NumberAt(b->doc, "volumeDb", 0);
      }
      SnapshotTarget* x = find(t);
      if (x == nullptr) {
        t.value = t.projectValue;
        out.push_back(t);
        x = &out.back();
      }
      if (v.kind == SnapshotKind::BusVolume) {
        x->value += w * v.value;  // runtime: base dB + sum(weight * value) (sys_dsp.cpp)
      } else {
        x->value += w * (v.value - x->projectValue);
      }
    }
  }
  return out;
}

// --- ducking --------------------------------------------------------------------------------
std::string MixerEditor::AddDucking(const std::string& bankGuid, DuckKind kind, const std::string& source,
                                    const std::string& target, double amountDb) {
  const DocObject* bank = doc_.Find(bankGuid);
  const DocObject* src = doc_.Find(source);
  const DocObject* dst = Bus(target);
  if (bank == nullptr || bank->kind() != "bank" || src == nullptr || dst == nullptr || !Finite(amountDb)) return std::string();
  if (src->kind() != (kind == DuckKind::Rule ? "category" : "bus")) return std::string();
  Value init = json::MakeObject();
  json::Add(init, "bank", String(bank->guid));
  json::Add(init, "kind", String(kind == DuckKind::Rule ? "rule" : "sidechain"));
  json::Add(init, "source", String(src->guid));
  json::Add(init, "target", String(dst->guid));
  json::Add(init, "amountDb", Number(amountDb));
  return doc_.Create("ducking", std::string(), &init);
}
bool MixerEditor::SetDuckingKind(const std::string& duck, DuckKind kind, const std::string& source) {
  const DocObject* d = doc_.Find(duck);
  const DocObject* src = doc_.Find(source);
  if (d == nullptr || d->kind() != "ducking" || src == nullptr || src->kind() != (kind == DuckKind::Rule ? "category" : "bus")) {
    return false;
  }
  const std::string sg = src->guid;
  const DocObject* curSrc = doc_.Resolve(kind == DuckKind::Rule ? "category" : "bus", StringAt(d->doc, "source"));
  if ((StringAt(d->doc, "kind", "sidechain") == "rule") == (kind == DuckKind::Rule) && curSrc != nullptr && curSrc->guid == sg) {
    return true;  // D-177b
  }
  return doc_.Modify(d->guid, [&](Value& v) {
    const char* const* order = SpecMemberOrder("ducking", "");
    SetMember(v, "kind", String(kind == DuckKind::Rule ? "rule" : "sidechain"), order);
    SetMember(v, "source", String(sg), order);
    return true;
  });
}
bool MixerEditor::SetDuckingTarget(const std::string& duck, const std::string& target) {
  const DocObject* d = doc_.Find(duck);
  const DocObject* t = Bus(target);
  if (d == nullptr || d->kind() != "ducking" || t == nullptr) return false;
  if (const DocObject* cur = doc_.Resolve("bus", StringAt(d->doc, "target")); cur != nullptr && cur->guid == t->guid) return true;
  return doc_.Set(d->guid, "target", String(t->guid));
}
bool MixerEditor::SetDucking(const std::string& duck, DuckField f, double v, std::uint32_t mergeKey) {
  const DocObject* d = doc_.Find(duck);
  if (d == nullptr || d->kind() != "ducking" || !Finite(v)) return false;
  if ((f == DuckField::AttackMs || f == DuckField::ReleaseMs) && v < 0) return false;
  const char* key = f == DuckField::AmountDb ? "amountDb" : f == DuckField::AttackMs ? "attackMs"
                    : f == DuckField::ReleaseMs ? "releaseMs" : "thresholdDb";
  const double def = f == DuckField::AttackMs ? 10.0 : f == DuckField::ReleaseMs ? 200.0 : 0.0;  // the build's defaults
  return doc_.SetDefaulted(d->guid, key, Number(v), Number(def), mergeKey);
}

// --- categories -----------------------------------------------------------------------------
std::string MixerEditor::AddCategory(const std::string& name, const std::string& parentGuid) {
  Value init = json::MakeObject();
  if (!parentGuid.empty()) {
    const DocObject* p = doc_.Find(parentGuid);
    if (p == nullptr || p->kind() != "category") return std::string();
    json::Add(init, "parent", String(p->guid));
  }
  return doc_.Create("category", name, &init);
}
bool MixerEditor::RemoveCategory(const std::string& cat) {
  const DocObject* c = doc_.Find(cat);
  if (c == nullptr || c->kind() != "category") return false;
  for (const DocObject* o : doc_.OfKind("category")) {
    const DocObject* p = doc_.Resolve("category", StringAt(o->doc, "parent"));
    if (p != nullptr && p->guid == c->guid) return false;
  }
  return doc_.Remove(c->guid);
}
bool MixerEditor::SetCategoryParent(const std::string& cat, const std::string& parentGuid) {
  const DocObject* c = doc_.Find(cat);
  if (c == nullptr || c->kind() != "category") return false;
  const std::string cg = c->guid;
  if (parentGuid.empty()) return c->doc.Find("parent") == nullptr || doc_.Erase(cg, "parent");
  const DocObject* p = doc_.Find(parentGuid);
  if (p == nullptr || p->kind() != "category") return false;
  if (const DocObject* cur = doc_.Resolve("category", StringAt(c->doc, "parent")); cur != nullptr && cur->guid == p->guid) return true;
  // Walk up from the new parent: reaching `cat` is a cycle.
  std::string g = p->guid;
  for (std::size_t guard = 0; guard <= doc_.objects().size() && !g.empty(); ++guard) {
    if (g == cg) return false;
    const DocObject* o = doc_.Find(g);
    const DocObject* up = o != nullptr ? doc_.Resolve("category", StringAt(o->doc, "parent")) : nullptr;
    g = up != nullptr ? up->guid : std::string();
  }
  return doc_.Set(cg, "parent", String(p->guid));
}
bool MixerEditor::SetCategoryVolume(const std::string& cat, double db, std::uint32_t mergeKey) {
  const DocObject* c = doc_.Find(cat);
  return c != nullptr && c->kind() == "category" && Finite(db) &&
         doc_.SetDefaulted(c->guid, "volumeDb", Number(std::clamp(db, static_cast<double>(kFaderOffDb), static_cast<double>(kFaderMaxDb))),
                           Number(0), mergeKey);
}
bool MixerEditor::SetCategoryFlag(const std::string& cat, bool ignorePause, bool muteOnFocusLoss) {
  const DocObject* c = doc_.Find(cat);
  if (c == nullptr || c->kind() != "category") return false;
  if (BoolAt(c->doc, "ignorePause", false) == ignorePause && BoolAt(c->doc, "muteOnFocusLoss", false) == muteOnFocusLoss) {
    return true;  // D-177b
  }
  return doc_.Modify(c->guid, [&](Value& v) {
    const char* const* order = SpecMemberOrder("category", "");
    for (std::size_t i = v.members.size(); i-- > 0;) {
      const std::string& k = v.members[i].first;
      if (k == "ignorePause" || k == "muteOnFocusLoss") v.members.erase(v.members.begin() + static_cast<std::ptrdiff_t>(i));
    }
    if (ignorePause) SetMember(v, "ignorePause", Bool(true), order);
    if (muteOnFocusLoss) SetMember(v, "muteOnFocusLoss", Bool(true), order);
    return true;
  });
}

// --- voice limit groups ---------------------------------------------------------------------
std::string MixerEditor::AddLimitGroup(const std::string& name, std::uint32_t maxVoices, StealMode steal) {
  if (maxVoices == 0u) return std::string();
  Value init = json::MakeObject();
  json::Add(init, "maxVoices", Number(maxVoices));
  json::Add(init, "steal", String(kSteal[static_cast<int>(steal)]));
  return doc_.Create("voiceLimitGroup", name, &init);
}
bool MixerEditor::SetMaxVoices(const std::string& group, std::uint32_t maxVoices) {
  const DocObject* g = doc_.Find(group);
  return g != nullptr && g->kind() == "voiceLimitGroup" && maxVoices >= 1u && doc_.SetNumber(g->guid, "maxVoices", maxVoices);
}
bool MixerEditor::SetSteal(const std::string& group, StealMode steal) {
  const DocObject* g = doc_.Find(group);
  return g != nullptr && g->kind() == "voiceLimitGroup" &&
         doc_.SetDefaulted(g->guid, "steal", String(kSteal[static_cast<int>(steal)]), String("oldest"));
}

// --- write back -------------------------------------------------------------------------------
std::uint32_t WriteBackMixer(ProjectDocument& doc, const MixerModel& mixer) {
  const Project* p = mixer.project();
  if (p == nullptr) return 0;
  std::uint32_t written = 0;
  doc.BeginGroup();
  for (std::size_t i = 0; i < p->buses().size() && i < mixer.buses().size(); ++i) {
    const BusDesc& bd = p->buses()[i];
    const std::string g = GuidText(bd.guid);
    if (doc.Find(g) == nullptr) continue;
    const MixerModel::BusStrip& st = mixer.buses()[i];
    if (st.faderDb != bd.volumeDb && doc.SetNumber(g, "volumeDb", st.faderDb)) ++written;
    for (std::size_t e = 0; e < bd.effects.size() && e < st.params.size(); ++e) {
      for (std::size_t q = 0; q < bd.effects[e].params.size() && q < st.params[e].size(); ++q) {
        if (st.params[e][q] == bd.effects[e].params[q].projectValue) continue;
        if (MixerEditor(doc).SetEffectParam(g, static_cast<std::uint32_t>(e), static_cast<std::uint32_t>(q), st.params[e][q])) {
          ++written;
        }
      }
    }
  }
  for (std::size_t i = 0; i < p->categories().size() && i < mixer.categories().size(); ++i) {
    const CategoryDesc& cd = p->categories()[i];
    const std::string g = GuidText(cd.guid);
    if (doc.Find(g) == nullptr) continue;
    const float f = mixer.categories()[i].faderDb;
    if (f != cd.volumeDb && doc.SetNumber(g, "volumeDb", f)) ++written;
  }
  doc.EndGroup();
  return written;
}

}  // namespace snd::tool
