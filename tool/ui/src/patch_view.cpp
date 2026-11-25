// Waveform synthesis patch screen (design.md 5, 5.1, 11.1 波形生成パッチエディタ): the node graph
// (drag a node to move it; drag from an output port onto an input port to connect, or click the
// output port and then an input port; click a connected input to disconnect; drag the background to
// scroll), adding and removing nodes, the selected node's parameters, the connections, the build's
// check (cycles, limits) and the dependency order. Edits through PatchEditor.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <set>

#include "editor_util.hpp"
#include "snd/buildtool/patch.hpp"
#include "snd/tool/patch_editor.hpp"
#include "snd/tool/ui/app.hpp"
#include "snd/tool/ui/editor_draw.hpp"
#include "snd/tool/ui/kit.hpp"
#include "snd/tool/ui/layout_metrics.hpp"
#include "snd/tool/ui/views.hpp"

namespace snd::tool::ui {
namespace {

namespace pt = ::snd::buildtool::patch;
constexpr float kListW = 200.0f;
constexpr float kSideW = 330.0f;
constexpr float kRow = 26.0f;
constexpr float kNodeW = 120.0f;

tela::Rect NodeBox(const PatchNode& n, const pt::NodeKindInfo* k, float sx, float sy) {
  const std::uint32_t inputs = k != nullptr ? k->inputCount : 0u;
  return tela::Rect{n.x - sx, n.y - sy, kNodeW, 32.0f + 16.0f * static_cast<float>(std::max(1u, inputs))};
}

// Element id parts of the nodes: the node id when it is unique, short and plain, else "#<position>"
// (a broken patch file can repeat ids; Tela refuses duplicate element ids).
std::vector<std::string> NodeKeys(const std::vector<PatchNode>& nodes) {
  std::vector<std::string> keys;
  std::set<std::string> used;
  for (std::size_t i = 0; i < nodes.size(); ++i) {
    const std::string& id = nodes[i].id;
    bool plain = !id.empty() && id.size() <= 64u && id[0] != '#';
    for (char c : id) plain = plain && c > ' ' && c != '.';
    std::string k = plain ? id : "#" + std::to_string(i);
    if (!used.insert(k).second) {
      k = "#" + std::to_string(i);
      used.insert(k);
    }
    keys.push_back(k);
  }
  return keys;
}

// The input port under a point of the graph (node id and input name), false if none.
bool InputAt(const std::vector<PatchNode>& nodes, float sx, float sy, float px, float py, std::string* node,
             std::string* input) {
  for (const PatchNode& n : nodes) {
    const pt::NodeKindInfo* k = pt::FindNodeKind(n.kind);
    if (k == nullptr) continue;
    const tela::Rect box = NodeBox(n, k, sx, sy);
    for (std::uint32_t i = 0; i < k->inputCount; ++i) {
      const tela::Point c = PatchInputPort(box, i);
      if (std::fabs(px - c.x) <= 9.0f && std::fabs(py - c.y) <= 9.0f) {
        *node = n.id;
        *input = k->inputs[i].name;
        return true;
      }
    }
  }
  return false;
}

void Graph(Kit& kit, const std::string& g, float x, float y, float w, float h) {
  App* app = &kit.app();
  ProjectDocument* d = app->document();
  PatchView* pv = &app->view().patch;
  PatchEditor pe(*d, g);
  const std::vector<PatchNode> nodes = pe.Nodes();
  const std::vector<PatchConnection> cons = pe.Connections();
  const float sx = pv->scrollX, sy = pv->scrollY;
  // Background: deselect, scroll.
  auto bg = std::make_shared<DragHandler>();
  auto anchor = std::make_shared<tela::Point>();
  bg->press = [pv, anchor](float, float, std::uint32_t) {
    pv->node.clear();
    *anchor = tela::Point{pv->scrollX, pv->scrollY};
  };
  bg->drag = [pv, anchor](const DragMotion& m) {
    pv->scrollX = anchor->x - m.dx;
    pv->scrollY = anchor->y - m.dy;
  };
  kit.Hit("pa.view.hit", bg, x, y, w, h);
  PatchLook look;
  look.dpi = kit.ctx().dpi;
  look.width = w;
  look.height = h;
  for (const PatchNode& n : nodes) {
    const pt::NodeKindInfo* k = pt::FindNodeKind(n.kind);
    const tela::Rect box = NodeBox(n, k, sx, sy);
    PatchNodeLook nl;
    nl.box = box;
    nl.inputs = k != nullptr ? k->inputCount : 0u;
    nl.output = k == nullptr || k->output != pt::Signal::None;
    nl.selected = n.id == pv->node;
    nl.pending = n.id == pv->pendingFrom;
    look.nodes.push_back(nl);
  }
  for (const PatchConnection& c : cons) {
    const PatchNode* from = nullptr;
    const PatchNode* to = nullptr;
    for (const PatchNode& n : nodes) {
      if (n.id == c.from) from = &n;
      if (n.id == c.to) to = &n;
    }
    if (from == nullptr || to == nullptr) continue;
    const pt::NodeKindInfo* fk = pt::FindNodeKind(from->kind);
    const pt::NodeKindInfo* tk = pt::FindNodeKind(to->kind);
    std::uint32_t index = 0;
    if (tk != nullptr && pt::FindInput(*tk, c.input, &index) == nullptr) index = 0;
    PatchWire wire;
    wire.from = PatchOutputPort(NodeBox(*from, fk, sx, sy));
    wire.to = PatchInputPort(NodeBox(*to, tk, sx, sy), index);
    wire.control = fk != nullptr && fk->output == pt::Signal::Control;
    look.wires.push_back(wire);
  }
  const std::vector<std::string> keys = NodeKeys(nodes);
  // A hit is the part of its rectangle inside the view (nodes partly scrolled out stay pressable
  // where they are seen; nothing lies outside the window, D-187). false if nothing is left.
  auto clip = [x, y, w, h](float& rx, float& ry, float& rw, float& rh) {
    const float x0 = std::max(rx, x), y0 = std::max(ry, y);
    const float x1 = std::min(rx + rw, x + w), y1 = std::min(ry + rh, y + h);
    if (x1 - x0 < 1.0f || y1 - y0 < 1.0f) return false;
    rx = x0;
    ry = y0;
    rw = x1 - x0;
    rh = y1 - y0;
    return true;
  };
  // Node hits: body (select, drag to move), output port (drag onto an input to connect, or click to
  // start a connection), input ports (complete a connection, or disconnect).
  for (std::size_t i = 0; i < nodes.size(); ++i) {
    const PatchNode n = nodes[i];
    const pt::NodeKindInfo* k = pt::FindNodeKind(n.kind);
    const tela::Rect box = look.nodes[i].box;
    if (box.x + box.width < 0.0f || box.y + box.height < 0.0f || box.x > w || box.y > h) continue;
    const std::string id = "pa.node." + keys[i];
    const std::string nid = n.id;
    auto body = DocumentDrag(*app, [pv, nid](float, float, std::uint32_t) { pv->node = nid; },
                             [d, g, nid, n](const DragMotion& m) {
                               PatchEditor(*d, g).MoveNode(nid, std::round(n.x + m.dx), std::round(n.y + m.dy));
                             });
    float bx = x + box.x + 8.0f, by = y + box.y, bw = box.width - 16.0f, bh = box.height;
    if (!clip(bx, by, bw, bh)) continue;
    kit.Hit(id, body, bx, by, bw, bh);
    if (k != nullptr && k->output != pt::Signal::None) {
      const tela::Point op = PatchOutputPort(box);
      float px = x + op.x - 7.0f, py = y + op.y - 7.0f, pw = 14.0f, ph = 14.0f;
      const bool shown = clip(px, py, pw, ph);
      const float ox = px - x, oy = py - y;  // the hit's origin in graph coordinates
      auto out = std::make_shared<DragHandler>();
      const std::vector<PatchNode> all = nodes;
      out->release = [d, g, pv, nid, all, ox, oy, sx, sy, app](bool moved, float lx, float ly) {
        if (!moved) {  // a click: connect from here, then click an input
          pv->pendingFrom = pv->pendingFrom == nid ? std::string() : nid;
          return;
        }
        std::string to, input;
        // The release point in graph coordinates (the port hit is 14 px around the port).
        if (!InputAt(all, sx, sy, ox + lx, oy + ly, &to, &input)) return;
        if (!PatchEditor(*d, g).Connect(nid, to, input)) app->Message("refused: connection (signal, an input taken, a cycle)");
        app->Edited();
      };
      if (shown) kit.Hit(id + ".out", out, px, py, pw, ph);
    }
    for (std::uint32_t in = 0; k != nullptr && in < k->inputCount; ++in) {
      const tela::Point ip = PatchInputPort(box, in);
      const std::string input = k->inputs[in].name;
      bool connected = false;
      for (const PatchConnection& c : cons) connected = connected || (c.to == nid && c.input == input);
      float px = x + ip.x - 7.0f, py = y + ip.y - 7.0f, pw = 14.0f, ph = 14.0f;
      if (!clip(px, py, pw, ph)) continue;
      Button(kit.doc(), id + ".in." + input, std::string(), [d, g, pv, nid, input, connected, app] {
        if (!pv->pendingFrom.empty()) {
          if (!PatchEditor(*d, g).Connect(pv->pendingFrom, nid, input)) app->Message("refused: connection");
          pv->pendingFrom.clear();
        } else if (connected) {
          PatchEditor(*d, g).Disconnect(nid, input);
        }
      }, px, py, pw, ph);
    }
  }
  Canvas(kit.doc(), "pa.view", DrawPatch(look, kit.palette()), x, y, w, h);
  // Node labels above the drawing.
  for (std::size_t i = 0; i < nodes.size(); ++i) {
    const tela::Rect box = look.nodes[i].box;
    if (box.x + box.width < 0.0f || box.y < 0.0f || box.x > w || box.y > h - 16.0f) continue;
    kit.Label("pa.node." + keys[i] + ".label", Clip(nodes[i].id, 14) + " " + Clip(nodes[i].kind, 12), x + box.x + 4.0f,
              y + box.y, box.width - 8.0f, 18, 0.45f);
  }
}

void Side(Kit& kit, const std::string& g, float x, float y, float w, float h) {
  App* app = &kit.app();
  ProjectDocument* d = app->document();
  PatchView* pv = &app->view().patch;
  PatchEditor pe(*d, g);
  const std::vector<PatchNode> nodes = pe.Nodes();
  bool found = false;
  for (const PatchNode& n : nodes) found = found || n.id == pv->node;
  if (!found) pv->node.clear();
  char b[160];
  std::snprintf(b, sizeof b, "nodes %u / %u   output %s", static_cast<unsigned>(nodes.size()), pt::kMaxNodes,
                pe.OutputNode().empty() ? "(none)" : pe.OutputNode().c_str());
  kit.Label("pa.count", b, x, y, w, 20, 0.55f);
  y += 22.0f;
  const float cx = pv->scrollX + 200.0f, cy = pv->scrollY + 120.0f;
  kit.PickField("pa.add", "", "+ Node", "Node kind", [] {
    std::vector<PickerItem> v;
    std::uint32_t n = 0;
    const pt::NodeKindInfo* kinds = pt::NodeKinds(&n);
    for (std::uint32_t i = 0; i < n; ++i) v.push_back(PickerItem{kinds[i].name, kinds[i].name});
    return v;
  }, [d, g, pv, cx, cy, app](const std::string& kind) {
    const std::string id = PatchEditor(*d, g).AddNode(kind, std::round(cx), std::round(cy));
    if (id.empty()) app->Message("refused: node (the limit, or a second output)");
    else pv->node = id;
  }, x, y, 100);
  if (!pv->pendingFrom.empty()) {
    kit.Action("pa.pending", "connecting from " + Clip(pv->pendingFrom, 24) + " (cancel)", [pv] { pv->pendingFrom.clear(); }, x + 104, y,
               w - 104);
  }
  y += kRow + 4.0f;
  if (!pv->node.empty()) {
    const std::string nid = pv->node;
    std::string kind;
    for (const PatchNode& n : nodes) {
      if (n.id == nid) kind = n.kind;
    }
    const pt::NodeKindInfo* k = pt::FindNodeKind(kind);
    kit.NameField("pa.sel.id", "Node", nid, [d, g, pv, nid](const std::string& v) {
      if (!PatchEditor(*d, g).RenameNode(nid, v)) return false;
      pv->node = v;
      return true;
    }, x, y, w - 74, 44);
    auto remove = [d, g, nid, pv] {
      if (PatchEditor(*d, g).RemoveNode(nid)) pv->node.clear();
    };
    kit.Action("pa.sel.remove", "Remove", remove, x + w - 70, y, 70);
    app->SetDeleteAction(remove);
    y += kRow;
    kit.Label("pa.sel.kind", "kind " + kind, x, y, w, 20, 0.55f);
    y += 22.0f;
    for (std::uint32_t p = 0; k != nullptr && p < k->paramCount && y < h * 0.62f; ++p) {
      const pt::ParamInfo& pi = k->params[p];
      const std::string pname = pi.name;
      if (pi.choices != nullptr) {
        const int cur = SatInt(pe.Param(nid, pname), -1);
        int count = 0;
        while (pi.choices[count] != nullptr) ++count;
        const std::string label = cur >= 0 && cur < count ? pi.choices[cur] : std::to_string(cur);
        const char* const* choices = pi.choices;
        kit.PickField("pa.sel.p." + pname, Clip(pname, 12), label, pname, [choices, count] {
          std::vector<PickerItem> v;
          for (int c = 0; c < count; ++c) v.push_back(PickerItem{choices[c], choices[c]});
          return v;
        }, [d, g, nid, pname](const std::string& v) { PatchEditor(*d, g).SetChoice(nid, pname, v); }, x, y, w, 96);
      } else {
        NumberSpec sp;
        sp.value = pe.Param(nid, pname);
        sp.minValue = pi.minValue;
        sp.maxValue = pi.maxValue;
        sp.step = std::max(1e-3, (static_cast<double>(pi.maxValue) - pi.minValue) / 100.0);
        sp.decimals = 3;
        kit.NumberField("pa.sel.p." + pname, Clip(pname, 12), sp,
                        [d, g, nid, pname](double v, std::uint32_t key) { return PatchEditor(*d, g).SetParam(nid, pname, v, key); },
                        x, y, w, 96);
      }
      y += kRow;
    }
    if (k != nullptr && k->usesParameter) {
      kit.PickField("pa.sel.curve", "parameter", "-", "Parameter of the curve", [d] { return DocItems(*d, "parameter"); },
                    [d, g, nid](const std::string& v) {
                      PatchEditor(*d, g).SetCurve(nid, v, {{0.0, 0.0}, {1.0, 1.0}});
                    }, x, y, w, 96);
      y += kRow;
    }
  }
  y = std::max(y, h * 0.62f) + 4.0f;
  // Connections, the check and the order.
  const std::vector<PatchConnection> cons = pe.Connections();
  kit.Label("pa.cons.title", "connections (" + std::to_string(cons.size()) + ")", x, y, w, 20, 0.55f);
  y += 20.0f;
  for (std::size_t i = 0; i < cons.size() && i < 5u; ++i) {
    const PatchConnection c = cons[i];
    kit.Label("pa.con." + std::to_string(i), c.from + " -> " + c.to + "." + c.input, x, y, w - 28, 20, 0.5f);
    kit.Action("pa.con." + std::to_string(i) + ".remove", "x", [d, g, c] { PatchEditor(*d, g).Disconnect(c.to, c.input); },
               x + w - 24, y, 24, 20);
    y += 20.0f;
  }
  if (cons.size() > 5u) {
    kit.Label("pa.cons.more", "+" + std::to_string(cons.size() - 5u) + " more", x, y, w, 18, 0.5f);
    y += 18.0f;
  }
  const std::vector<pt::Problem> problems = pe.Check();
  std::uint32_t row = 0;
  for (const pt::Problem& p : problems) {
    if (y > h - 40.0f || row >= 4u) break;
    kit.Label("pa.problem." + std::to_string(row++), std::string(p.error ? "error " : "warning ") + p.path + ": " + p.message, x, y,
              w, 18, 0.45f);
    y += 18.0f;
  }
  if (problems.empty()) {
    pt::Program prog;
    std::string order = "order:";
    if (pe.Compiled(&prog)) {
      std::uint32_t n = 0;
      const pt::NodeKindInfo* kinds = pt::NodeKinds(&n);
      for (const pt::CompiledNode& cn : prog.nodes) {
        if (cn.source < nodes.size()) order += " " + nodes[cn.source].id;
        else if (cn.kind < n) order += std::string(" ") + kinds[cn.kind].name;
      }
    }
    kit.Label("pa.order", Clip(order, 160), x, y, w, 36, 0.45f);
  }
}

}  // namespace

void DeclarePatch(tela::Document& doc, ViewContext& ctx, float width, float height) {
  Kit kit(doc, ctx);
  if (!NeedDocument(kit, "pa.nodoc", width)) return;
  App* app = ctx.app;
  ProjectDocument* d = app->document();
  PatchView& pv = app->view().patch;
  ObjectListSpec ls;
  ls.id = "pa.list";
  ls.kind = "patch";
  ls.title = "Patches";
  ls.selected = &pv.patch;
  ls.page = &pv.listPage;
  ls.create = [d] {
    json::Value init = json::MakeObject();
    const std::string bank = FirstBank(*d);
    if (!bank.empty()) json::Add(init, "bank", String(bank));
    const std::string g = d->Create("patch", NewName(*d, "patch", "Patch"), &init);
    if (!g.empty()) {
      std::uint32_t n = 0;
      const pt::NodeKindInfo* kinds = pt::NodeKinds(&n);
      for (std::uint32_t i = 0; i < n; ++i) {
        if (kinds[i].output == pt::Signal::None) PatchEditor(*d, g).AddNode(kinds[i].name, 420.0f, 60.0f);
      }
    }
    return g;
  };
  const std::string before = pv.patch;
  Panel(doc, "pa.listpanel", 0, 0, kListW, height, [&] { ObjectList(kit, ls, 0, 0, kListW - 12, height - 12); }, 6.0f);
  if (pv.patch != before) {
    pv.node.clear();
    pv.pendingFrom.clear();
  }
  if (pv.patch.empty()) return;
  const std::string g = pv.patch;
  const float gx = kListW + 8.0f, gw = std::max(200.0f, width - kListW - kSideW - 16.0f);
  Panel(doc, "pa.graph", gx, 0, gw, height, [&] { Graph(kit, g, 0, 0, gw - 12, height - 12); }, 6.0f);
  Panel(doc, "pa.side", width - kSideW, 0, kSideW, height, [&] { Side(kit, g, 0, 0, kSideW - 12, height - 12); }, 6.0f);
}

}  // namespace snd::tool::ui
