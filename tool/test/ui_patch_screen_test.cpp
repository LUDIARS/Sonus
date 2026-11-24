// The patch screen: nodes (add, drag to move as one undo step, parameters, remove with Delete),
// connections by dragging an output onto an input and by click / click, disconnecting, the build's
// check and the dependency order, the node limit.
#include <cmath>

#include "snd/buildtool/patch.hpp"
#include "snd/test/test.hpp"
#include "snd/tool/patch_editor.hpp"
#include "ui_editor_helpers.hpp"

using namespace snd::tool;
using namespace edtest;

namespace {

std::string NewPatch(Editor& t) {
  SND_CHECK(t.Click("pa.list.new"));
  return t.app.view().patch.patch;
}

}  // namespace

SND_TEST(ToolUiPatch, NodesConnectionsAndCheck) {
  Editor t(ui::Screen::Patch);
  const std::string g = NewPatch(t);
  SND_REQUIRE(!g.empty());
  PatchEditor pe(t.doc, g);
  SND_REQUIRE_EQ(pe.Nodes().size(), 1u);  // a new patch has its output node
  const std::string out = pe.OutputNode();
  SND_REQUIRE(!out.empty());
  SND_CHECK(t.Has("pa.node." + out) && t.Has("pa.node." + out + ".in.in"));
  // An unconnected output: the build's check says so.
  SND_CHECK(t.Has("pa.problem.0"));
  SND_REQUIRE(t.Pick("pa.add", "oscillator"));
  const std::string osc = t.app.view().patch.node;
  SND_REQUIRE(!osc.empty());
  SND_CHECK(t.Has("pa.sel.p.frequencyHz") && t.Has("pa.sel.p.waveform"));
  // Move the oscillator left of the output (one undo step).
  const std::size_t undo0 = t.doc.UndoCount();
  const PatchNode before = pe.Nodes()[1];
  t.Drag("pa.node." + osc, -150.0f, 40.0f, 4);
  SND_CHECK_EQ(pe.Nodes()[1].x, std::round(before.x - 150.0f));
  SND_CHECK_EQ(pe.Nodes()[1].y, std::round(before.y + 40.0f));
  SND_CHECK_EQ(t.doc.UndoCount(), undo0 + 1u);
  // Drag from the oscillator's output port onto the output node's input.
  const tela::PlacedElement* port = t.El("pa.node." + osc + ".out");
  const tela::PlacedElement* in = t.El("pa.node." + out + ".in.in");
  SND_REQUIRE(port != nullptr && in != nullptr);
  const tela::Point a = Center(port), b = Center(in);
  t.DragFrom(a, b.x - a.x, b.y - a.y, 5);
  SND_REQUIRE_EQ(pe.Connections().size(), 1u);
  SND_CHECK(pe.Connections()[0].from == osc && pe.Connections()[0].to == out);
  SND_CHECK(!t.Has("pa.problem.0"));
  SND_CHECK(t.Label("pa.order").find(osc) != std::string::npos);
  // Click the connected input: disconnected; then click / click connects again.
  SND_REQUIRE(t.Click("pa.node." + out + ".in.in"));
  SND_CHECK(pe.Connections().empty());
  SND_REQUIRE(t.Click("pa.node." + osc + ".out"));
  SND_CHECK(t.app.view().patch.pendingFrom == osc);
  SND_REQUIRE(t.Click("pa.node." + out + ".in.in"));
  SND_CHECK_EQ(pe.Connections().size(), 1u);
  SND_CHECK(t.app.view().patch.pendingFrom.empty());
  // A parameter, a choice, then Delete removes the selected node and its connection.
  SND_REQUIRE(t.Click("pa.node." + osc));
  SND_REQUIRE(t.Click("pa.sel.p.frequencyHz.inc"));
  SND_CHECK(pe.Param(osc, "frequencyHz") > 440.0);
  SND_REQUIRE(t.Pick("pa.sel.p.waveform", "square"));
  SND_CHECK_EQ(StringAt(t.doc.Find(g)->doc, "nodes[1].params.waveform"), std::string("square"));
  t.Key(ui::kKeyDelete);
  SND_CHECK_EQ(pe.Nodes().size(), 1u);
  SND_CHECK(pe.Connections().empty());
}

SND_TEST(ToolUiPatch, NodeLimit) {
  Editor t(ui::Screen::Patch);
  const std::string g = NewPatch(t);
  PatchEditor pe(t.doc, g);
  for (std::uint32_t i = 1; i < snd::buildtool::patch::kMaxNodes; ++i) pe.AddNode("noise", 20.0f * static_cast<float>(i), 300.0f);
  t.Frame();
  SND_CHECK_EQ(pe.Nodes().size(), static_cast<std::size_t>(snd::buildtool::patch::kMaxNodes));
  SND_REQUIRE(t.Pick("pa.add", "noise"));  // refused: the limit
  SND_CHECK_EQ(pe.Nodes().size(), static_cast<std::size_t>(snd::buildtool::patch::kMaxNodes));
  SND_CHECK(t.app.runtime().elements().size() < ui::kMaxElements);
  for (const auto& p : t.app.runtime().elements()) {
    if (p.element.kind == tela::ElementKind::canvas) SND_CHECK(p.element.drawing.shapes().size() <= 256u);
  }
}
