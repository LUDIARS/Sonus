// The Tela declaration of the tool's screens: what is declared, stable ids
// across frames and value changes, buttons wired to the models, paging.
#include "snd/test/test.hpp"
#include "ui_helpers.hpp"

using namespace snd::tool;
using namespace uitest;

namespace {

struct Offline {
  tooltest::ToolProject tool;
  Session s;
  ui::App app;
  explicit Offline(int width = 1280, int height = 800) : app(s, Desc(width, height)) {
    SND_REQUIRE(tool.ok);
    s.UseProject(tool.project);
    app.Frame(tooltest::Now());
  }
  static ui::AppDesc Desc(int w, int h) {
    ui::AppDesc d;
    d.width = w;
    d.height = h;
    return d;
  }
  std::string Bus(const char* name) const { return ui::BusId(snd::HashId(name)); }
};

}  // namespace

SND_TEST(ToolUiDeclaration, MixerStripsAndControls) {
  Offline t;
  const tela::Runtime& rt = t.app.runtime();
  for (const char* b : {"Master", "Music", "Sfx"}) {
    const std::string base = t.Bus(b);
    for (const char* part : {"", ".name", ".mute", ".solo", ".fader.hit", ".fader", ".meter", ".db", ".sync"}) {
      SND_CHECK_MSG(Find(rt, base + part) != nullptr, "missing %s%s", base.c_str(), part);
    }
  }
  const std::string cat = ui::CategoryId(snd::HashId("SE"));
  SND_CHECK(Find(rt, cat) != nullptr && Find(rt, cat + ".fader.hit") != nullptr && Find(rt, cat + ".mute") != nullptr);
  SND_CHECK(Find(rt, cat + ".meter") == nullptr);  // categories have no meter
  // Strips are placed left to right in tree order, kStripWidth + kStripGap apart.
  const float x0 = Find(rt, t.Bus("Master"))->bounds.x;
  SND_CHECK_NEAR(Find(rt, t.Bus("Music"))->bounds.x - x0, ui::kStripWidth + ui::kStripGap, 1e-3);
  SND_CHECK_NEAR(Find(rt, t.Bus("Sfx"))->bounds.x - x0, 2 * (ui::kStripWidth + ui::kStripGap), 1e-3);
  SND_CHECK_NEAR(Find(rt, cat)->bounds.x - x0, 3 * (ui::kStripWidth + ui::kStripGap), 1e-3);
  // Sfx: the delay's first six parameters are knobs, MaxDelay (init only) is the "+1" overflow.
  const std::string sfx = t.Bus("Sfx");
  for (int p = 0; p < 6; ++p) {
    SND_CHECK(Find(rt, sfx + ".fx0." + std::to_string(p)) != nullptr);
    SND_CHECK(Find(rt, sfx + ".fx0." + std::to_string(p) + ".hit") != nullptr);
  }
  SND_CHECK(Find(rt, sfx + ".fx0.6") == nullptr);
  SND_CHECK(Find(rt, sfx + ".fx.more") != nullptr);
  SND_CHECK(Find(rt, t.Bus("Master") + ".fx.none") != nullptr);
  // Controls the router follows: 4 faders, 6 knobs, 1 parameter knob.
  std::uint32_t faders = 0, knobs = 0;
  for (const ui::ControlBinding& b : t.app.router().controls()) {
    faders += b.kind == ui::ControlKind::Fader ? 1u : 0u;
    knobs += b.kind == ui::ControlKind::Knob ? 1u : 0u;
    const tela::PlacedElement* hit = Find(rt, b.hitId);
    SND_REQUIRE(hit != nullptr);
    SND_CHECK(hit->element.kind == tela::ElementKind::button);
    SND_CHECK(hit->element.input == tela::InputPolicy::exclusive);
  }
  SND_CHECK_EQ(faders, 4u);
  SND_CHECK_EQ(knobs, 7u);
  // The drawing sits above its hit button and lets the input through.
  const tela::PlacedElement* fader = Find(rt, sfx + ".fader");
  const tela::PlacedElement* hit = Find(rt, sfx + ".fader.hit");
  SND_CHECK(fader->bounds == hit->bounds);
  SND_CHECK(fader->element.input == tela::InputPolicy::passthrough);
  SND_CHECK(rt.hit(Center(fader).x, Center(fader).y) == tela::InputPolicy::exclusive);
  SND_CHECK(Find(rt, "ovr.revertall") != nullptr && Find(rt, "par." + Hex32(snd::HashId("Knob")) + ".on") != nullptr);
  SND_CHECK(rt.elements().size() < ui::kMaxElements);
}

SND_TEST(ToolUiDeclaration, IdsAreStableAcrossFramesAndValues) {
  Offline t;
  const std::set<std::string> first = Ids(t.app.runtime());
  t.app.Frame(tooltest::Now());
  SND_CHECK(Ids(t.app.runtime()) == first);
  // Values change, the structure does not.
  const std::int32_t sfx = tooltest::Bus(t.tool.project, "Sfx");
  t.s.mixer().Set(Target{Control::BusFader, sfx, 0, 0}, -20.0f);
  t.s.mixer().Set(Target{Control::BusMute, sfx, 0, 0}, 1.0f);
  t.s.mixer().Set(Target{Control::EffectParam, sfx, 0, 2}, -40.0f);
  t.app.Frame(tooltest::Now());
  std::set<std::string> after = Ids(t.app.runtime());
  // The overrides list grows by its rows; everything else is the same.
  std::set<std::string> diff;
  for (const std::string& id : after) {
    if (!first.count(id)) diff.insert(id);
  }
  for (const std::string& id : first) SND_CHECK_MSG(after.count(id) == 1u, "id %s disappeared", id.c_str());
  SND_CHECK_EQ(diff.size(), 4u);  // two overrides x (row, revert button)
  for (const std::string& id : diff) SND_CHECK(id.rfind("ovr.", 0) == 0);
  // The profiler screen and back.
  t.app.OnScreen(ui::Screen::Profiler);
  t.app.Frame(tooltest::Now());
  SND_CHECK(Find(t.app.runtime(), "prof.graph") != nullptr && Find(t.app.runtime(), "rec.toggle") != nullptr);
  SND_CHECK(Find(t.app.runtime(), t.Bus("Sfx")) == nullptr);
  const std::set<std::string> prof = Ids(t.app.runtime());
  t.app.Frame(tooltest::Now());
  SND_CHECK(Ids(t.app.runtime()) == prof);
  t.app.OnScreen(ui::Screen::Mixer);
  t.app.Frame(tooltest::Now());
  SND_CHECK(Ids(t.app.runtime()) == after);
}

SND_TEST(ToolUiDeclaration, ButtonsDriveTheModels) {
  Offline t;
  Pointer ptr(t.app);
  const std::string sfx = t.Bus("Sfx");
  const std::int32_t s = tooltest::Bus(t.tool.project, "Sfx");
  tela::Point c = Center(Find(t.app.runtime(), sfx + ".mute"));
  ptr.Click(c.x, c.y);
  SND_CHECK(t.s.mixer().buses()[static_cast<std::size_t>(s)].muted);
  t.app.Frame(tooltest::Now());
  c = Center(Find(t.app.runtime(), sfx + ".solo"));
  ptr.Click(c.x, c.y);
  SND_CHECK(t.s.mixer().buses()[static_cast<std::size_t>(s)].soloed);
  t.app.Frame(tooltest::Now());
  c = Center(Find(t.app.runtime(), "ovr.undo"));
  ptr.Click(c.x, c.y);
  SND_CHECK(!t.s.mixer().buses()[static_cast<std::size_t>(s)].soloed);
  t.app.Frame(tooltest::Now());
  // The overrides row of Sfx (muted) has a Revert button.
  const std::string key = "ovr.bus." + Hex32(snd::HashId("Sfx")) + ".vol.revert";
  SND_REQUIRE(Find(t.app.runtime(), key) != nullptr);
  c = Center(Find(t.app.runtime(), key));
  ptr.Click(c.x, c.y);
  SND_CHECK(!t.s.mixer().buses()[static_cast<std::size_t>(s)].muted);
  SND_CHECK(t.s.mixer().Overrides().empty());
  t.app.Frame(tooltest::Now());
  // Parameter override toggle.
  const std::string on = "par." + Hex32(snd::HashId("Knob")) + ".on";
  c = Center(Find(t.app.runtime(), on));
  ptr.Click(c.x, c.y);
  SND_CHECK(t.s.mixer().parameters()[0].enabled);
  t.app.Frame(tooltest::Now());
  // Tabs.
  c = Center(Find(t.app.runtime(), "tab.profiler"));
  ptr.Click(c.x, c.y);
  SND_CHECK(t.app.view().screen == ui::Screen::Profiler);
}

SND_TEST(ToolUiDeclaration, NarrowWindowPagesTheStrips) {
  Offline t(560, 700);  // strips area: 560 - 12 - 310 - 6 = 232 px -> 2 strips per page
  SND_CHECK(Find(t.app.runtime(), t.Bus("Master")) != nullptr);
  SND_CHECK(Find(t.app.runtime(), t.Bus("Music")) != nullptr);
  SND_CHECK(Find(t.app.runtime(), t.Bus("Sfx")) == nullptr);
  Pointer ptr(t.app);
  tela::Point c = Center(Find(t.app.runtime(), "mixer.next"));
  ptr.Click(c.x, c.y);
  t.app.Frame(tooltest::Now());
  SND_CHECK(Find(t.app.runtime(), t.Bus("Sfx")) != nullptr);
  SND_CHECK(Find(t.app.runtime(), ui::CategoryId(snd::HashId("SE"))) != nullptr);
  ptr.Click(c.x, c.y);  // past the last page: stays
  t.app.Frame(tooltest::Now());
  SND_CHECK_EQ(t.app.view().mixerPage, 1u);
  c = Center(Find(t.app.runtime(), "mixer.prev"));
  ptr.Click(c.x, c.y);
  t.app.Frame(tooltest::Now());
  SND_CHECK(Find(t.app.runtime(), t.Bus("Master")) != nullptr);
}
