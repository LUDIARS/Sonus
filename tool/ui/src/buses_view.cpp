// Mixer structure screen (design.md 4.6, 11.1 ミキサー; the live mixer is the Mixer screen):
// buses (tree, volume, parent, channels, flags, sends with level / pre-post, the effect chain with
// every parameter), snapshots (values, capture from the live mixer, weighted result), ducking
// rules and categories with voice limit groups. Every edit goes through MixerEditor; "Write back"
// writes the live mixer's values into the document (WriteBackMixer, one step).
#include <algorithm>
#include <cmath>
#include <cstdio>

#include "editor_util.hpp"
#include "snd/tool/mixer_editor.hpp"
#include "snd/tool/ui/app.hpp"
#include "snd/tool/ui/kit.hpp"
#include "snd/tool/ui/layout_metrics.hpp"
#include "snd/tool/ui/views.hpp"

namespace snd::tool::ui {
namespace {

constexpr float kRow = 26.0f;
constexpr float kListW = 230.0f;

const char* const kSnapKinds[] = {"busVolume", "effectParam", "sendLevel"};
const char* const kSteal[] = {"oldest", "quietest", "lowestPriority", "never"};

std::vector<PickerItem> Names(const char* const* names, int n) {
  std::vector<PickerItem> v;
  for (int i = 0; i < n; ++i) v.push_back(PickerItem{names[i], std::to_string(i)});
  return v;
}
int ToInt(const std::string& s) { return s.empty() ? 0 : std::atoi(s.c_str()); }

// Buses in tree order (master first, children by name) with their depth.
void Tree(const MixerEditor& me, const std::string& bus, std::uint32_t depth,
          std::vector<std::pair<std::string, std::uint32_t>>* out) {
  if (out->size() > 4096u || depth > 64u) return;
  out->emplace_back(bus, depth);
  for (const std::string& c : me.Children(bus)) Tree(me, c, depth + 1u, out);
}

void BusesTab(Kit& kit, float width, float height) {
  App* app = &kit.app();
  ProjectDocument* d = app->document();
  BusesView& bv = app->view().buses;
  BusesView* bvp = &bv;
  MixerEditor me(*d);
  std::vector<std::pair<std::string, std::uint32_t>> tree;
  const std::string master = me.MasterBus();
  if (!master.empty()) Tree(me, master, 0, &tree);
  for (const DocObject* o : d->OfKind("bus")) {  // buses outside the tree (a broken parent) at the end
    bool in = false;
    for (const auto& t : tree) in = in || t.first == o->guid;
    if (!in) tree.emplace_back(o->guid, 0u);
  }
  bool selected = false;
  for (const auto& t : tree) selected = selected || t.first == bv.bus;
  if (!selected) bv.bus = tree.empty() ? std::string() : tree.front().first;
  // Left: the tree.
  Panel(kit.doc(), "mx.buslist", 0, 0, kListW, height, [&] {
    const float w = kListW - 12.0f;
    kit.Title("mx.bus.title", "Buses (" + std::to_string(tree.size()) + ")", 0, 0, w);
    kit.Action("mx.bus.add", "+ Child", [d, bvp, app] {
      MixerEditor m(*d);
      const std::string parent = bvp->bus.empty() ? m.MasterBus() : bvp->bus;
      const std::string g = m.AddBus(NewName(*d, "bus", "Bus"), parent);
      if (g.empty()) app->Message("refused: new bus");
      else bvp->bus = g;
    }, 0, 24, 70);
    if (!bv.bus.empty()) {
      const std::string g = bv.bus;
      kit.Action("mx.bus.remove", "Remove", [d, g, app] {
        if (!MixerEditor(*d).RemoveBus(g)) app->Message("refused: the bus has children or is referenced");
      }, 74, 24, 70);
      const DocObject* o = d->Find(g);
      if (o != nullptr) {
        kit.NameField("mx.bus.name", "Name", o->name(), [d, g](const std::string& n) { return d->Rename(g, n); }, 0, 50,
                      w, 50);
      }
    }
    const std::uint32_t per = static_cast<std::uint32_t>(std::max(1.0f, (height - 112.0f) / 24.0f));
    const std::uint32_t first = kit.Pager("mx.bus.pager", static_cast<std::uint32_t>(tree.size()), per, &bv.listPage,
                                          0, 76, w);
    KeySet keys;
    for (std::size_t i = 0; i < first && i < tree.size(); ++i) keys.Key(tree[i].first, i);
    for (std::uint32_t i = 0; i < per && first + i < tree.size(); ++i) {
      const auto& [g, depth] = tree[first + i];
      const std::string key = keys.Key(g, first + i);
      const DocObject* o = d->Find(g);
      const std::string name = o != nullptr ? o->name() : g;
      const std::string indent(std::min<std::uint32_t>(depth, 8u) * 2u, ' ');
      const std::string gg = g;
      Button(kit.doc(), "mx.bus." + key, (g == bv.bus ? "> " : "  ") + indent + Clip(name, 30),
             [bvp, gg] { bvp->bus = gg; }, 0, 100.0f + static_cast<float>(i) * 24.0f, w, 22, 0.55f);
    }
  }, 6.0f);
  if (bv.bus.empty()) return;
  const std::string g = bv.bus;
  const DocObject* o = d->Find(g);
  if (o == nullptr) return;
  const json::Value& b = o->doc;
  const float mx = kListW + 8.0f, mw = std::min(380.0f, (width - mx) * 0.45f);
  Panel(kit.doc(), "mx.busprops", mx, 0, mw, height, [&] {
    const float w = mw - 12.0f;
    float y = 0.0f;
    kit.Title("mx.bp.title", o->name() + (BoolAt(b, "master", false) ? " (master)" : ""), 0, y, w);
    y += 24.0f;
    NumberSpec sp;
    sp.value = NumberAt(b, "volumeDb", 0.0);
    sp.step = 0.5;
    sp.minValue = -144;
    sp.maxValue = 24;
    sp.decimals = 2;
    sp.unit = "dB";
    kit.NumberField("mx.bp.vol", "Volume", sp,
                    [d, g](double v, std::uint32_t key) { return MixerEditor(*d).SetVolume(g, v, key); }, 0, y, w);
    y += kRow;
    if (!BoolAt(b, "master", false)) {
      kit.PickField("mx.bp.parent", "Parent", RefName(*d, "bus", StringAt(b, "parent")), "Parent bus",
                    [d, g] {
                      std::vector<PickerItem> v;
                      for (const PickerItem& it : DocItems(*d, "bus")) {
                        if (it.value != g) v.push_back(it);
                      }
                      return v;
                    },
                    [d, g, app](const std::string& v) {
                      if (!MixerEditor(*d).SetParent(g, v)) app->Message("refused: the parent would make a cycle");
                    },
                    0, y, w);
      y += kRow;
    }
    NumberSpec ch;
    ch.value = NumberAt(b, "channels", 0.0);
    ch.step = 1;
    ch.minValue = 0;
    ch.maxValue = 8;
    ch.integer = true;
    ch.decimals = 0;
    kit.NumberField("mx.bp.ch", "Channels", ch, [d, g](double v, std::uint32_t) {
      return MixerEditor(*d).SetChannels(g, static_cast<std::uint32_t>(v));
    }, 0, y, w);
    y += kRow;
    const float tw = (w - 8.0f) / 3.0f;
    struct Fl {
      BusFlag f;
      const char* key;
      const char* label;
    };
    const Fl fl[] = {{BusFlag::Output, "output", "output"}, {BusFlag::LodBypassable, "lodBypassable", "LOD bypass"},
                     {BusFlag::Meter, "meter", "meter"}};
    for (int i = 0; i < 3; ++i) {
      const bool on = BoolAt(b, fl[i].key, false);
      const BusFlag f = fl[i].f;
      kit.Toggle("mx.bp.flag" + std::to_string(i), fl[i].label, on,
                 [d, g, f, on] { MixerEditor(*d).SetFlag(g, f, !on); }, static_cast<float>(i) * (tw + 4.0f), y, tw);
    }
    y += kRow;
    NumberSpec lp;
    lp.value = NumberAt(b, "lodPriority", 0.0);
    lp.step = 1;
    lp.minValue = 0;
    lp.maxValue = 255;
    lp.integer = true;
    lp.decimals = 0;
    kit.NumberField("mx.bp.lod", "LOD prio.", lp, [d, g](double v, std::uint32_t) {
      return MixerEditor(*d).SetLodPriority(g, static_cast<std::uint32_t>(v));
    }, 0, y, w);
    y += kRow + 6.0f;
    // Sends.
    const json::Value* sends = b.Find("sends");
    const std::uint32_t ns = sends != nullptr && sends->IsArray() ? static_cast<std::uint32_t>(sends->items.size()) : 0u;
    kit.Label("mx.send.title", "sends (" + std::to_string(ns) + ")", 0, y, w - 110, 22, 0.65f);
    kit.PickField("mx.send.add", "", "+ Send", "Send to", [d, g] {
      std::vector<PickerItem> v;
      for (const PickerItem& it : DocItems(*d, "bus")) {
        if (it.value != g) v.push_back(it);
      }
      return v;
    }, [d, g, app](const std::string& v) {
      if (!MixerEditor(*d).AddSend(g, v, -12.0)) app->Message("refused: the send would make a cycle");
    }, w - 104, y, 104);
    y += 24.0f;
    for (std::uint32_t i = 0; i < ns && y + 2 * kRow < height - 60.0f; ++i) {
      const json::Value& s = sends->items[i];
      const std::string sid = "mx.send." + std::to_string(i);
      kit.PickField(sid + ".to", "", "-> " + RefName(*d, "bus", StringAt(s, "bus")), "Send target", [d, g] {
        std::vector<PickerItem> v;
        for (const PickerItem& it : DocItems(*d, "bus")) {
          if (it.value != g) v.push_back(it);
        }
        return v;
      }, [d, g, i, app](const std::string& v) {
        if (!MixerEditor(*d).SetSendTarget(g, i, v)) app->Message("refused: the send would make a cycle");
      }, 0, y, w * 0.42f);
      const bool pre = BoolAt(s, "preFader", false);
      kit.Toggle(sid + ".pre", "pre", pre, [d, g, i, pre] { MixerEditor(*d).SetSendPreFader(g, i, !pre); },
                 w * 0.42f + 4.0f, y, 56);
      kit.Action(sid + ".remove", "x", [d, g, i] { MixerEditor(*d).RemoveSend(g, i); }, w - 24.0f, y, 24);
      y += kRow;
      NumberSpec lv;
      lv.value = NumberAt(s, "levelDb", 0.0);
      lv.step = 0.5;
      lv.minValue = -144;
      lv.maxValue = 24;
      lv.unit = "dB";
      lv.decimals = 2;
      kit.NumberField(sid + ".level", "  level", lv,
                      [d, g, i](double v, std::uint32_t key) { return MixerEditor(*d).SetSendLevel(g, i, v, key); }, 0,
                      y, w);
      y += kRow;
    }
    // Graph order (inputs first, master last).
    std::string order = "graph order:";
    const std::vector<std::string> go = me.GraphOrder();
    if (go.empty()) order += " (cycle)";
    for (std::size_t i = 0; i < go.size() && i < 12u; ++i) order += " " + RefName(*d, "bus", go[i]);
    if (go.size() > 12u) order += " ...";
    kit.Label("mx.graphorder", Clip(order, 120), 0, height - 40.0f, w, 20, 0.5f);
  }, 6.0f);
  // Right: the effect chain.
  const float ex = mx + mw + 8.0f, ew = width - ex;
  Panel(kit.doc(), "mx.effects", ex, 0, ew, height, [&] {
    const float w = ew - 12.0f;
    const json::Value* fx = b.Find("effects");
    const std::uint32_t ne = fx != nullptr && fx->IsArray() ? static_cast<std::uint32_t>(fx->items.size()) : 0u;
    kit.Title("mx.fx.title", "effects (" + std::to_string(ne) + ")", 0, 0, w - 120);
    kit.PickField("mx.fx.add", "", "+ Effect", "Effect type", [] {
      std::vector<PickerItem> v;
      for (const EffectTypeInfo& t : StandardEffectTypes()) v.push_back(PickerItem{t.name, t.name});
      return v;
    }, [d, g, ne, bvp](const std::string& v) {
      if (MixerEditor(*d).AddEffect(g, v, ne)) bvp->effectPage = ne;
    }, w - 116, 0, 116);
    const std::vector<EffectTypeInfo> types = StandardEffectTypes();
    // One effect per page (an effect can have many parameters).
    const std::uint32_t first = kit.Pager("mx.fx.pager", ne, 1, &bv.effectPage, 0, 26, 200);
    if (ne == 0u) {
      kit.Label("mx.fx.none", "no effects", 0, 50, w);
      return;
    }
    const std::uint32_t e = first;
    const json::Value& ef = fx->items[e];
    const std::string type = StringAt(ef, "type");
    const std::string eid = "mx.fx." + std::to_string(e);
    float y = 50.0f;
    kit.Label(eid + ".type", std::to_string(e + 1u) + ". " + type, 0, y, w - 170, 22, 0.65f);
    kit.Action(eid + ".up", "Up", [d, g, e, bvp] {
      if (e > 0u && MixerEditor(*d).MoveEffect(g, e, e - 1u)) bvp->effectPage = e - 1u;
    }, w - 166, y, 40, 22, e > 0u);
    kit.Action(eid + ".down", "Down", [d, g, e, bvp] {
      if (MixerEditor(*d).MoveEffect(g, e, e + 1u)) bvp->effectPage = e + 1u;
    }, w - 122, y, 50, 22, e + 1u < ne);
    kit.Action(eid + ".remove", "Remove", [d, g, e] { MixerEditor(*d).RemoveEffect(g, e); }, w - 68, y, 66);
    y += 26.0f;
    const EffectTypeInfo* info = nullptr;
    for (const EffectTypeInfo& t : types) {
      if (t.name == type) info = &t;
    }
    const json::Value* params = ef.Find("params");
    const std::uint32_t np = info != nullptr ? static_cast<std::uint32_t>(info->params.size())
                             : params != nullptr && params->IsArray() ? static_cast<std::uint32_t>(params->items.size())
                                                                      : 0u;
    const float colW = (w - 8.0f) * 0.5f;
    for (std::uint32_t p = 0; p < np && y < height - 30.0f; ++p) {
      NumberSpec sp;
      sp.value = me.EffectParam(g, e, p);
      std::string label = "P" + std::to_string(p);
      if (info != nullptr) {
        const EffectParamDesc& pd = info->params[p];
        label = pd.name + (pd.initOnly ? " (init)" : "");
        sp.minValue = pd.minValue;
        sp.maxValue = pd.maxValue;
        sp.integer = pd.integer;
        sp.unit = pd.unit;
        const double span = static_cast<double>(pd.maxValue) - static_cast<double>(pd.minValue);
        sp.step = pd.integer ? 1.0 : (pd.logScale ? std::max(1.0, sp.value * 0.05) : span / 100.0);
        sp.decimals = pd.integer ? 0 : 3;
      } else {
        sp.step = 0.01;
      }
      kit.NumberField(eid + ".p" + std::to_string(p), Clip(label, 16), sp,
                      [d, g, e, p](double v, std::uint32_t key) { return MixerEditor(*d).SetEffectParam(g, e, p, v, key); },
                      (p % 2u) * (colW + 8.0f), y, colW, 96);
      if (p % 2u == 1u) y += kRow;
    }
  }, 6.0f);
}

void SnapshotsTab(Kit& kit, float width, float height) {
  App* app = &kit.app();
  ProjectDocument* d = app->document();
  BusesView& bv = app->view().buses;
  ObjectListSpec ls;
  ls.id = "mx.snap.list";
  ls.kind = "snapshot";
  ls.title = "Snapshots";
  ls.selected = &bv.snapshot;
  ls.page = &bv.listPage;
  ls.create = [d] { return MixerEditor(*d).AddSnapshot(NewName(*d, "snapshot", "Snapshot"), FirstBank(*d)); };
  Panel(kit.doc(), "mx.snaplist", 0, 0, kListW, height, [&] { ObjectList(kit, ls, 0, 0, kListW - 12, height - 12); },
        6.0f);
  const float mx = kListW + 8.0f, mw = (width - mx) * 0.55f;
  Session* session = &app->session();
  if (!bv.snapshot.empty()) {
    const std::string g = bv.snapshot;
    Panel(kit.doc(), "mx.snapvalues", mx, 0, mw, height, [&] {
      const float w = mw - 12.0f;
      MixerEditor me(*d);
      const std::vector<SnapshotValue> vals = me.SnapshotValues(g);
      kit.Title("mx.sv.title", "values (" + std::to_string(vals.size()) + ")", 0, 0, w - 230);
      kit.Action("mx.sv.add", "+ Value", [d, g] {
        SnapshotValue v;
        v.bus = MixerEditor(*d).MasterBus();
        MixerEditor(*d).AddSnapshotValue(g, v);
      }, w - 226, 0, 70);
      kit.Action("mx.sv.capture", "Capture mixer", [d, g, session, app] {
        if (MixerEditor(*d).CaptureSnapshot(g, session->mixer())) app->Message("snapshot: captured the live mixer");
      }, w - 152, 0, 150);
      const std::uint32_t per = static_cast<std::uint32_t>(std::max(1.0f, (height - 70.0f) / (2.0f * kRow + 6.0f)));
      const std::uint32_t first = kit.Pager("mx.sv.pager", static_cast<std::uint32_t>(vals.size()), per, &bv.valuePage, 0,
                                            26, 200);
      float y = 50.0f;
      for (std::uint32_t i = first; i < vals.size() && i < first + per; ++i) {
        const SnapshotValue v = vals[i];
        const std::string vid = "mx.sv." + std::to_string(i);
        kit.PickField(vid + ".kind", "", kSnapKinds[static_cast<int>(v.kind)], "Value kind",
                      [] { return Names(kSnapKinds, 3); }, [d, g, i, v](const std::string& k) {
                        SnapshotValue n = v;
                        n.kind = static_cast<SnapshotKind>(ToInt(k));
                        MixerEditor(*d).SetSnapshotValue(g, i, n);
                      }, 0, y, 110);
        kit.PickField(vid + ".bus", "", RefName(*d, "bus", v.bus), "Bus", [d] { return DocItems(*d, "bus"); },
                      [d, g, i, v](const std::string& b) {
                        SnapshotValue n = v;
                        n.bus = b;
                        MixerEditor(*d).SetSnapshotValue(g, i, n);
                      }, 114, y, 130);
        kit.Action(vid + ".remove", "x", [d, g, i] { MixerEditor(*d).RemoveSnapshotValue(g, i); }, w - 24, y, 24);
        y += kRow;
        NumberSpec sp;
        sp.value = v.value;
        sp.step = v.kind == SnapshotKind::BusVolume || v.kind == SnapshotKind::SendLevel ? 0.5 : 0.01;
        sp.decimals = 3;
        kit.NumberField(vid + ".value", "value", sp, [d, g, i, v](double x, std::uint32_t) {
          SnapshotValue n = v;
          n.value = x;
          return MixerEditor(*d).SetSnapshotValue(g, i, n);
        }, 0, y, w * 0.5f, 50);
        if (v.kind != SnapshotKind::BusVolume) {
          NumberSpec ix;
          ix.step = 1;
          ix.minValue = 0;
          ix.maxValue = 255;
          ix.integer = true;
          ix.decimals = 0;
          if (v.kind == SnapshotKind::EffectParam) {
            ix.value = v.effect;
            kit.NumberField(vid + ".effect", "fx", ix, [d, g, i, v](double x, std::uint32_t) {
              SnapshotValue n = v;
              n.effect = static_cast<std::uint32_t>(x);
              return MixerEditor(*d).SetSnapshotValue(g, i, n);
            }, w * 0.5f + 4, y, w * 0.25f - 4, 24);
            ix.value = v.param;
            kit.NumberField(vid + ".param", "p", ix, [d, g, i, v](double x, std::uint32_t) {
              SnapshotValue n = v;
              n.param = static_cast<std::uint32_t>(x);
              return MixerEditor(*d).SetSnapshotValue(g, i, n);
            }, w * 0.75f + 4, y, w * 0.25f - 4, 20);
          } else {
            ix.value = v.send;
            kit.NumberField(vid + ".send", "send", ix, [d, g, i, v](double x, std::uint32_t) {
              SnapshotValue n = v;
              n.send = static_cast<std::uint32_t>(x);
              return MixerEditor(*d).SetSnapshotValue(g, i, n);
            }, w * 0.5f + 4, y, w * 0.5f - 4, 40);
          }
        }
        y += kRow + 6.0f;
      }
    }, 6.0f);
  }
  // Right: weights of every snapshot and the result (4.6).
  const float rx = mx + mw + 8.0f, rw = width - rx;
  Panel(kit.doc(), "mx.snapresult", rx, 0, rw, height, [&] {
    const float w = rw - 12.0f;
    kit.Title("mx.sr.title", "result of weights", 0, 0, w);
    float y = 26.0f;
    std::vector<std::pair<std::string, double>> weights;
    std::uint32_t row = 0;
    KeySet keys;
    for (const DocObject* o : d->OfKind("snapshot")) {
      const std::string key = keys.Key(o->guid, row);
      const std::string g = o->guid;
      const double wv = bv.weights.count(g) != 0u ? bv.weights[g] : 0.0;
      weights.emplace_back(g, wv);
      if (row < 6u) {
        NumberSpec sp;
        sp.value = wv;
        sp.step = 0.1;
        sp.minValue = 0;
        sp.maxValue = 1;
        sp.decimals = 2;
        sp.document = false;
        std::map<std::string, double>* wm = &bv.weights;
        kit.NumberField("mx.sr.w." + key, Clip(o->name(), 14), sp, [wm, g](double v, std::uint32_t) {
          (*wm)[g] = v;
          return true;
        }, 0, y, w, 110);
        y += kRow;
      }
      ++row;
    }
    if (row > 6u) {
      kit.Label("mx.sr.more", "+" + std::to_string(row - 6u) + " snapshots (weight 0)", 0, y, w, 20, 0.5f);
      y += 20.0f;
    }
    const std::vector<SnapshotTarget> res = MixerEditor(*d).ApplySnapshots(weights);
    const std::uint32_t per = static_cast<std::uint32_t>(std::max(1.0f, (height - y - 40.0f) / 20.0f));
    const std::uint32_t first = kit.Pager("mx.sr.pager", static_cast<std::uint32_t>(res.size()), per, &bv.resultPage, 0,
                                          y, 200);
    y += 22.0f;
    for (std::uint32_t i = first; i < res.size() && i < first + per; ++i) {
      const SnapshotTarget& t = res[i];
      std::string what = RefName(*d, "bus", t.bus) + " " + kSnapKinds[static_cast<int>(t.kind)];
      if (t.kind == SnapshotKind::EffectParam) what += " fx" + std::to_string(t.effect) + ".p" + std::to_string(t.param);
      if (t.kind == SnapshotKind::SendLevel) what += " send" + std::to_string(t.send);
      kit.Label("mx.sr." + std::to_string(i - first),
                what + ": " + FormatNumberText(t.projectValue, 3) + " -> " + FormatNumberText(t.value, 3), 0, y, w, 20,
                0.5f);
      y += 20.0f;
    }
    if (res.empty()) kit.Label("mx.sr.none", "no values", 0, y, w, 20, 0.5f);
  }, 6.0f);
}

void DuckingTab(Kit& kit, float width, float height) {
  App* app = &kit.app();
  ProjectDocument* d = app->document();
  BusesView& bv = app->view().buses;
  ObjectListSpec ls;
  ls.id = "mx.duck.list";
  ls.kind = "ducking";
  ls.title = "Ducking";
  ls.selected = &bv.ducking;
  ls.page = &bv.listPage;
  ls.create = [d] {
    MixerEditor me(*d);
    const std::string master = me.MasterBus();
    std::string source = master;
    for (const DocObject* o : d->OfKind("bus")) {
      if (o->guid != master) {
        source = o->guid;
        break;
      }
    }
    return me.AddDucking(FirstBank(*d), DuckKind::Sidechain, source, master, -6.0);
  };
  ls.label = [d](const DocObject& o) {
    const bool rule = StringAt(o.doc, "kind", "sidechain") == "rule";
    return RefName(*d, rule ? "category" : "bus", StringAt(o.doc, "source")) + " -> " +
           RefName(*d, "bus", StringAt(o.doc, "target")) + (o.name().empty() ? "" : " (" + o.name() + ")");
  };
  Panel(kit.doc(), "mx.ducklist", 0, 0, kListW + 60, height, [&] { ObjectList(kit, ls, 0, 0, kListW + 48, height - 12); },
        6.0f);
  if (bv.ducking.empty()) return;
  const std::string g = bv.ducking;
  const DocObject* o = d->Find(g);
  if (o == nullptr) return;
  const json::Value& j = o->doc;
  const bool rule = StringAt(j, "kind", "sidechain") == "rule";
  const float mx = kListW + 68.0f, mw = std::min(420.0f, width - mx);
  Panel(kit.doc(), "mx.duckprops", mx, 0, mw, height, [&] {
    const float w = mw - 12.0f;
    float y = 0.0f;
    kit.Toggle("mx.dk.kind", rule ? "rule (category)" : "sidechain (bus)", rule, [d, g, rule] {
      // Switching the kind needs a source of the other kind: the first one.
      const auto all = d->OfKind(rule ? "bus" : "category");
      if (!all.empty()) MixerEditor(*d).SetDuckingKind(g, rule ? DuckKind::Sidechain : DuckKind::Rule, all.front()->guid);
    }, 0, y, w);
    y += kRow;
    const char* sk = rule ? "category" : "bus";
    kit.PickField("mx.dk.source", "Source", RefName(*d, sk, StringAt(j, "source")), "Source", [d, sk] { return DocItems(*d, sk); },
                  [d, g, rule](const std::string& v) {
                    MixerEditor(*d).SetDuckingKind(g, rule ? DuckKind::Rule : DuckKind::Sidechain, v);
                  }, 0, y, w);
    y += kRow;
    kit.PickField("mx.dk.target", "Target", RefName(*d, "bus", StringAt(j, "target")), "Target bus",
                  [d] { return DocItems(*d, "bus"); },
                  [d, g](const std::string& v) { MixerEditor(*d).SetDuckingTarget(g, v); }, 0, y, w);
    y += kRow;
    struct F {
      DuckField f;
      const char* key;
      const char* label;
      double def, step, mn, mx;
      const char* unit;
    };
    const F fs[] = {{DuckField::AmountDb, "amountDb", "Amount", 0, 0.5, -144, 0, "dB"},
                    {DuckField::AttackMs, "attackMs", "Attack", 0, 5, 0, 1e6, "ms"},
                    {DuckField::ReleaseMs, "releaseMs", "Release", 0, 5, 0, 1e6, "ms"},
                    {DuckField::ThresholdDb, "thresholdDb", "Threshold", -40, 1, -144, 24, "dB"}};
    for (int i = 0; i < 4; ++i) {
      NumberSpec sp;
      sp.value = NumberAt(j, fs[i].key, fs[i].def);
      sp.step = fs[i].step;
      sp.minValue = fs[i].mn;
      sp.maxValue = fs[i].mx;
      sp.unit = fs[i].unit;
      sp.decimals = 2;
      sp.enabled = fs[i].f != DuckField::ThresholdDb || !rule;
      const DuckField f = fs[i].f;
      kit.NumberField("mx.dk.f" + std::to_string(i), fs[i].label, sp,
                      [d, g, f](double v, std::uint32_t key) { return MixerEditor(*d).SetDucking(g, f, v, key); }, 0, y, w);
      y += kRow;
    }
  }, 6.0f);
}

void CategoriesTab(Kit& kit, float width, float height) {
  App* app = &kit.app();
  ProjectDocument* d = app->document();
  BusesView& bv = app->view().buses;
  ObjectListSpec ls;
  ls.id = "mx.cat.list";
  ls.kind = "category";
  ls.title = "Categories";
  ls.selected = &bv.category;
  ls.page = &bv.listPage;
  ls.create = [d] { return MixerEditor(*d).AddCategory(NewName(*d, "category", "Category"), std::string()); };
  ls.remove = [d](const std::string& g) { return MixerEditor(*d).RemoveCategory(g); };
  ls.label = [d](const DocObject& o) {
    const std::string parent = StringAt(o.doc, "parent");
    return parent.empty() ? o.name() : RefName(*d, "category", parent) + " / " + o.name();
  };
  Panel(kit.doc(), "mx.catlist", 0, 0, kListW, height, [&] { ObjectList(kit, ls, 0, 0, kListW - 12, height - 12); },
        6.0f);
  const float mx = kListW + 8.0f, mw = (width - mx) * 0.5f;
  if (!bv.category.empty() && d->Find(bv.category) != nullptr) {
    const std::string g = bv.category;
    const json::Value& j = d->Find(g)->doc;
    Panel(kit.doc(), "mx.catprops", mx, 0, mw, height, [&] {
      const float w = mw - 12.0f;
      float y = 0.0f;
      kit.PickField("mx.ct.parent", "Parent", RefName(*d, "category", StringAt(j, "parent"), "(none)"), "Parent category",
                    [d, g] {
                      std::vector<PickerItem> v;
                      for (const PickerItem& it : DocItems(*d, "category", "(none)")) {
                        if (it.value != g) v.push_back(it);
                      }
                      return v;
                    },
                    [d, g, app](const std::string& v) {
                      if (!MixerEditor(*d).SetCategoryParent(g, v)) app->Message("refused: the parent would make a cycle");
                    },
                    0, y, w);
      y += kRow;
      NumberSpec sp;
      sp.value = NumberAt(j, "volumeDb", 0.0);
      sp.step = 0.5;
      sp.minValue = -144;
      sp.maxValue = 24;
      sp.unit = "dB";
      sp.decimals = 2;
      kit.NumberField("mx.ct.vol", "Volume", sp,
                      [d, g](double v, std::uint32_t key) { return MixerEditor(*d).SetCategoryVolume(g, v, key); }, 0, y, w);
      y += kRow;
      const bool ip = BoolAt(j, "ignorePause", false), mf = BoolAt(j, "muteOnFocusLoss", false);
      kit.Toggle("mx.ct.ipause", "ignore pause", ip, [d, g, ip, mf] { MixerEditor(*d).SetCategoryFlag(g, !ip, mf); }, 0,
                 y, w * 0.5f - 4);
      kit.Toggle("mx.ct.focus", "mute on focus loss", mf, [d, g, ip, mf] { MixerEditor(*d).SetCategoryFlag(g, ip, !mf); },
                 w * 0.5f, y, w * 0.5f);
    }, 6.0f);
  }
  // Voice limit groups.
  const float rx = mx + mw + 8.0f, rw = width - rx;
  ObjectListSpec gs;
  gs.id = "mx.grp.list";
  gs.kind = "voiceLimitGroup";
  gs.title = "Voice limit groups";
  gs.selected = &bv.group;
  gs.page = &bv.valuePage;
  gs.create = [d] { return MixerEditor(*d).AddLimitGroup(NewName(*d, "voiceLimitGroup", "Group"), 8, StealMode::Oldest); };
  Panel(kit.doc(), "mx.groups", rx, 0, rw, height, [&] {
    const float w = rw - 12.0f;
    const float bottom = ObjectList(kit, gs, 0, 0, w, height - 100);
    if (bv.group.empty() || d->Find(bv.group) == nullptr) return;
    const std::string g = bv.group;
    const json::Value& j = d->Find(g)->doc;
    NumberSpec sp;
    sp.value = NumberAt(j, "maxVoices", 1.0);
    sp.step = 1;
    sp.minValue = 1;
    sp.maxValue = 4096;
    sp.integer = true;
    sp.decimals = 0;
    kit.NumberField("mx.grp.max", "Max voices", sp, [d, g](double v, std::uint32_t) {
      return MixerEditor(*d).SetMaxVoices(g, static_cast<std::uint32_t>(v));
    }, 0, std::min(bottom + 8.0f, height - 70.0f), w);
    kit.PickField("mx.grp.steal", "Steal", StringAt(j, "steal", "oldest"), "Steal mode", [] { return Names(kSteal, 4); },
                  [d, g](const std::string& v) { MixerEditor(*d).SetSteal(g, static_cast<StealMode>(ToInt(v))); }, 0,
                  std::min(bottom + 8.0f, height - 70.0f) + kRow, w);
  }, 6.0f);
}

}  // namespace

void DeclareBuses(tela::Document& doc, ViewContext& ctx, float width, float height) {
  Kit kit(doc, ctx);
  if (!NeedDocument(kit, "mx.nodoc", width)) return;
  App* app = ctx.app;
  BusesView& bv = app->view().buses;
  BusesView* bvp = &bv;
  kit.Tabs("mx.tab", {"Buses", "Snapshots", "Ducking", "Categories"}, bv.tab, [bvp](std::uint32_t i) {
    bvp->tab = i;
    bvp->listPage = 0;
    bvp->valuePage = 0;
  }, 0, 0, 100);
  kit.Action("mx.writeback", "Write back live mixer", [app] { app->WriteBackMixer(); }, width - 190, 0, 186, 24,
             !app->session().mixer().Overrides().empty());
  const float top = 30.0f;
  Panel(doc, "mx.body", 0, top, width, height - top, [&] {
    const float h = height - top;
    switch (bv.tab) {
      case 1: SnapshotsTab(kit, width, h); break;
      case 2: DuckingTab(kit, width, h); break;
      case 3: CategoriesTab(kit, width, h); break;
      default: BusesTab(kit, width, h); break;
    }
  });
}

}  // namespace snd::tool::ui
