// Waveform synthesis patch editor model (patch_editor.hpp).
#include "snd/tool/patch_editor.hpp"

#include <cmath>
#include <set>

namespace snd::tool {
namespace {

using json::Value;
namespace pt = buildtool::patch;

std::string Idx(const char* a, std::size_t i, const std::string& rest = std::string()) {
  return std::string(a) + "[" + std::to_string(i) + "]" + rest;
}

}  // namespace

PatchEditor::PatchEditor(ProjectDocument& doc, std::string guid) : doc_(doc), guid_(NormalizeGuid(guid)) {}
bool PatchEditor::valid() const {
  const DocObject* o = doc_.Find(guid_);
  return o != nullptr && o->kind() == "patch";
}
std::vector<PatchNode> PatchEditor::Nodes() const {
  std::vector<PatchNode> out;
  const Value* n = valid() ? doc_.Find(guid_)->doc.Find("nodes") : nullptr;
  if (n == nullptr || !n->IsArray()) return out;
  for (const Value& v : n->items) {
    out.push_back(PatchNode{StringAt(v, "id"), StringAt(v, "kind"), static_cast<float>(NumberAt(v, "position[0]", 0)),
                            static_cast<float>(NumberAt(v, "position[1]", 0))});
  }
  return out;
}
std::vector<PatchConnection> PatchEditor::Connections() const {
  std::vector<PatchConnection> out;
  const Value* c = valid() ? doc_.Find(guid_)->doc.Find("connections") : nullptr;
  if (c == nullptr || !c->IsArray()) return out;
  for (const Value& v : c->items) {
    PatchConnection x{StringAt(v, "from"), StringAt(v, "to"), StringAt(v, "input")};
    if (x.input.empty()) {  // the first input of the target's kind
      for (const PatchNode& n : Nodes()) {
        const pt::NodeKindInfo* k = n.id == x.to ? pt::FindNodeKind(n.kind) : nullptr;
        if (k != nullptr && k->inputCount > 0) x.input = k->inputs[0].name;
      }
    }
    out.push_back(x);
  }
  return out;
}
std::string PatchEditor::OutputNode() const {
  for (const PatchNode& n : Nodes()) {
    const pt::NodeKindInfo* k = pt::FindNodeKind(n.kind);
    if (k != nullptr && k->output == pt::Signal::None) return n.id;
  }
  return std::string();
}
int PatchEditor::NodeIndex(const std::string& id) const {
  const std::vector<PatchNode> n = Nodes();
  for (std::size_t i = 0; i < n.size(); ++i) {
    if (n[i].id == id) return static_cast<int>(i);
  }
  return -1;
}

std::string PatchEditor::AddNode(const std::string& kind, float x, float y, const std::string& idIn) {
  const pt::NodeKindInfo* k = pt::FindNodeKind(kind);
  const std::vector<PatchNode> nodes = Nodes();
  if (!valid() || k == nullptr || nodes.size() >= pt::kMaxNodes || !std::isfinite(x) || !std::isfinite(y)) return std::string();
  if (k->output == pt::Signal::None && !OutputNode().empty()) return std::string();  // one output node
  std::string id = idIn;
  if (id.empty()) {
    for (std::uint32_t n = 1;; ++n) {
      id = kind + std::to_string(n);
      if (NodeIndex(id) < 0) break;
    }
  }
  if (NodeIndex(id) >= 0) return std::string();
  Value v = json::MakeObject();
  json::Add(v, "id", String(id));
  json::Add(v, "kind", String(kind));
  Value pos = json::MakeArray();
  pos.items.push_back(Number(x));
  pos.items.push_back(Number(y));
  json::Add(v, "position", pos);
  doc_.BeginGroup();
  bool ok = doc_.Insert(guid_, "nodes", nodes.size(), v);
  if (ok && k->output == pt::Signal::None) ok = doc_.Set(guid_, "output", String(id)) || true;
  doc_.EndGroup();
  return ok ? id : std::string();
}

bool PatchEditor::RemoveNode(const std::string& id) {
  const int i = NodeIndex(id);
  if (i < 0) return false;
  return doc_.Modify(guid_, [&](Value& d) {
    Value* nodes = FindPath(d, "nodes");
    nodes->items.erase(nodes->items.begin() + i);
    if (Value* c = FindPath(d, "connections"); c != nullptr && c->IsArray()) {
      for (std::size_t k = c->items.size(); k-- > 0;) {
        if (StringAt(c->items[k], "from") == id || StringAt(c->items[k], "to") == id) {
          c->items.erase(c->items.begin() + static_cast<std::ptrdiff_t>(k));
        }
      }
    }
    if (StringAt(d, "output") == id) {
      for (std::size_t m = 0; m < d.members.size(); ++m) {
        if (d.members[m].first == "output") {
          d.members.erase(d.members.begin() + static_cast<std::ptrdiff_t>(m));
          break;
        }
      }
    }
    return true;
  });
}

bool PatchEditor::MoveNode(const std::string& id, float x, float y, std::uint32_t mergeKey) {
  const int i = NodeIndex(id);
  if (i < 0 || !std::isfinite(x) || !std::isfinite(y)) return false;
  const PatchNode cur = Nodes()[static_cast<std::size_t>(i)];
  if (cur.x == x && cur.y == y) return true;  // D-177b (0, 0 when absent)
  Value pos = json::MakeArray();
  pos.items.push_back(Number(x));
  pos.items.push_back(Number(y));
  return doc_.Set(guid_, Idx("nodes", static_cast<std::size_t>(i), ".position"), pos, mergeKey);
}

bool PatchEditor::RenameNode(const std::string& id, const std::string& newId) {
  const int i = NodeIndex(id);
  if (i >= 0 && id == newId) return true;  // D-177b
  if (i < 0 || newId.empty() || NodeIndex(newId) >= 0) return false;
  return doc_.Modify(guid_, [&](Value& d) {
    *FindPath(d, Idx("nodes", static_cast<std::size_t>(i), ".id")) = String(newId);
    if (Value* c = FindPath(d, "connections"); c != nullptr && c->IsArray()) {
      for (Value& x : c->items) {
        for (const char* k : {"from", "to"}) {
          if (Value* v = FindPath(x, k); v != nullptr && v->IsString() && v->text == id) *v = String(newId);
        }
      }
    }
    if (Value* o = FindPath(d, "output"); o != nullptr && o->IsString() && o->text == id) *o = String(newId);
    return true;
  });
}

bool PatchEditor::SetParam(const std::string& id, const std::string& param, double value, std::uint32_t mergeKey) {
  const int i = NodeIndex(id);
  if (i < 0 || !std::isfinite(value)) return false;
  const pt::NodeKindInfo* k = pt::FindNodeKind(Nodes()[static_cast<std::size_t>(i)].kind);
  const pt::ParamInfo* p = k != nullptr ? pt::FindParam(*k, param) : nullptr;
  if (p == nullptr || p->choices != nullptr || value < p->minValue || value > p->maxValue) return false;
  if (Param(id, param) == value) return true;  // D-177b (the table default when absent)
  const std::string path = Idx("nodes", static_cast<std::size_t>(i), ".params");
  return doc_.Modify(
      guid_,
      [&](Value& d) {
        Value* node = FindPath(d, Idx("nodes", static_cast<std::size_t>(i)));
        Value* ps = FindPath(*node, "params");
        if (ps == nullptr) {
          SetMember(*node, "params", json::MakeObject(), SpecMemberOrder("patch", "nodes[0]"));
          ps = FindPath(*node, "params");
        }
        if (!ps->IsObject()) return false;
        SetMember(*ps, param, Number(value), nullptr);
        return true;
      },
      mergeKey);
}

bool PatchEditor::SetChoice(const std::string& id, const std::string& param, const std::string& choice) {
  const int i = NodeIndex(id);
  if (i < 0) return false;
  const pt::NodeKindInfo* k = pt::FindNodeKind(Nodes()[static_cast<std::size_t>(i)].kind);
  const pt::ParamInfo* p = k != nullptr ? pt::FindParam(*k, param) : nullptr;
  if (p == nullptr || p->choices == nullptr) return false;
  bool known = false;
  std::uint32_t index = 0;
  for (std::uint32_t c = 0; p->choices[c] != nullptr; ++c) {
    if (choice == p->choices[c]) {
      known = true;
      index = c;
    }
  }
  if (!known) return false;
  if (Param(id, param) == index) return true;  // D-177b
  return doc_.Modify(guid_, [&](Value& d) {
    Value* node = FindPath(d, Idx("nodes", static_cast<std::size_t>(i)));
    Value* ps = FindPath(*node, "params");
    if (ps == nullptr) {
      SetMember(*node, "params", json::MakeObject(), SpecMemberOrder("patch", "nodes[0]"));
      ps = FindPath(*node, "params");
    }
    if (!ps->IsObject()) return false;
    SetMember(*ps, param, String(choice), nullptr);
    return true;
  });
}

bool PatchEditor::SetCurve(const std::string& id, const std::string& parameterGuid,
                           const std::vector<std::pair<double, double>>& points) {
  const int i = NodeIndex(id);
  const DocObject* prm = doc_.Find(parameterGuid);
  if (i < 0 || prm == nullptr || prm->kind() != "parameter") return false;
  const pt::NodeKindInfo* k = pt::FindNodeKind(Nodes()[static_cast<std::size_t>(i)].kind);
  if (k == nullptr || !k->usesParameter || points.size() < 2 || points.size() > pt::kMaxCurvePoints) return false;
  for (std::size_t p = 0; p < points.size(); ++p) {
    if (!std::isfinite(points[p].first) || !std::isfinite(points[p].second)) return false;
    if (p > 0 && points[p].first <= points[p - 1].first) return false;
  }
  Value ps = json::MakeObject();
  json::Add(ps, "parameter", String(prm->guid));
  Value arr = json::MakeArray();
  for (const auto& [x, y] : points) {
    Value pt2 = json::MakeArray();
    pt2.items.push_back(Number(x));
    pt2.items.push_back(Number(y));
    arr.items.push_back(pt2);
  }
  json::Add(ps, "points", arr);
  const std::string path = Idx("nodes", static_cast<std::size_t>(i), ".params");
  if (const Value* cur = FindPath(doc_.Find(guid_)->doc, path); cur != nullptr && cur->IsObject()) {
    const DocObject* cp = doc_.Resolve("parameter", StringAt(*cur, "parameter"));
    const Value* cpts = cur->Find("points");
    if (cp != nullptr && cp->guid == prm->guid && cpts != nullptr && JsonSameMeaning(*cpts, arr)) return true;  // D-177b
    // Other members of "params" (notes ...) stay.
    return doc_.Modify(guid_, [&](Value& d) {
      Value* p = FindPath(d, path);
      SetMember(*p, "parameter", String(prm->guid), nullptr);
      SetMember(*p, "points", arr, nullptr);
      return true;
    });
  }
  return doc_.Set(guid_, path, ps);
}

double PatchEditor::Param(const std::string& id, const std::string& param) const {
  const int i = NodeIndex(id);
  if (i < 0) return 0.0;
  const pt::NodeKindInfo* k = pt::FindNodeKind(Nodes()[static_cast<std::size_t>(i)].kind);
  const pt::ParamInfo* p = k != nullptr ? pt::FindParam(*k, param) : nullptr;
  const double def = p != nullptr ? p->defaultValue : 0.0;
  const Value* v = FindPath(doc_.Find(guid_)->doc, Idx("nodes", static_cast<std::size_t>(i), ".params"));
  const Value* x = v != nullptr ? v->Find(param) : nullptr;
  if (x == nullptr) return def;
  if (x->IsString() && p != nullptr && p->choices != nullptr) {
    for (std::uint32_t c = 0; p->choices[c] != nullptr; ++c) {
      if (x->text == p->choices[c]) return c;
    }
  }
  double d = def;
  return x->IsNumber() && x->AsDouble(&d) ? d : def;
}

bool PatchEditor::WouldCycle(const std::string& from, const std::string& to) const {
  // A path to -> ... -> from closes a cycle with from -> to.
  std::vector<std::string> work{to};
  std::set<std::string> seen;
  const std::vector<PatchConnection> cs = Connections();
  while (!work.empty()) {
    const std::string x = work.back();
    work.pop_back();
    if (x == from) return true;
    if (!seen.insert(x).second) continue;
    for (const PatchConnection& c : cs) {
      if (c.from == x) work.push_back(c.to);
    }
  }
  return false;
}

bool PatchEditor::Connect(const std::string& from, const std::string& to, const std::string& inputIn) {
  const int fi = NodeIndex(from), ti = NodeIndex(to);
  if (fi < 0 || ti < 0 || from == to) return false;
  const std::vector<PatchNode> nodes = Nodes();
  const pt::NodeKindInfo* fk = pt::FindNodeKind(nodes[static_cast<std::size_t>(fi)].kind);
  const pt::NodeKindInfo* tk = pt::FindNodeKind(nodes[static_cast<std::size_t>(ti)].kind);
  if (fk == nullptr || tk == nullptr || fk->output == pt::Signal::None || tk->inputCount == 0) return false;
  const std::string input = inputIn.empty() ? tk->inputs[0].name : inputIn;
  const pt::InputInfo* in = pt::FindInput(*tk, input);
  if (in == nullptr || in->signal != fk->output) return false;
  for (const PatchConnection& c : Connections()) {
    if (c.to == to && c.input == input) return false;  // one connection per input
  }
  if (WouldCycle(from, to)) return false;
  Value v = json::MakeObject();
  json::Add(v, "from", String(from));
  json::Add(v, "to", String(to));
  json::Add(v, "input", String(input));
  return doc_.Insert(guid_, "connections", Connections().size(), v);
}

bool PatchEditor::Disconnect(const std::string& to, const std::string& input) {
  const std::vector<PatchConnection> cs = Connections();
  for (std::size_t i = 0; i < cs.size(); ++i) {
    if (cs[i].to == to && cs[i].input == input) return doc_.Erase(guid_, Idx("connections", i));
  }
  return false;
}

std::vector<pt::Problem> PatchEditor::Check() const {
  std::vector<pt::Problem> problems;
  pt::Program prog;
  if (!valid()) return problems;
  const DocObject* o = doc_.Find(guid_);
  struct Ctx {
    const ProjectDocument* doc;
  } ctx{&doc_};
  pt::Compile(
      o->doc, HashId(o->name()), &prog, &problems,
      [](void* u, const Value& ref) -> Id {
        const DocObject* p = static_cast<Ctx*>(u)->doc->Resolve("parameter", ref.text);
        return p != nullptr ? HashId(p->name()) : 0u;
      },
      &ctx);
  return problems;
}

bool PatchEditor::Compiled(pt::Program* out) const {
  if (!valid()) return false;
  const DocObject* o = doc_.Find(guid_);
  struct Ctx {
    const ProjectDocument* doc;
  } ctx{&doc_};
  std::vector<pt::Problem> problems;
  return pt::Compile(
      o->doc, HashId(o->name()), out, &problems,
      [](void* u, const Value& ref) -> Id {
        const DocObject* p = static_cast<Ctx*>(u)->doc->Resolve("parameter", ref.text);
        return p != nullptr ? HashId(p->name()) : 0u;
      },
      &ctx);
}

}  // namespace snd::tool
