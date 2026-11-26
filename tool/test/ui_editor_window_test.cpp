// The smallest window (D-187, third round): from kMinEditorWidth x kMinEditorHeight (logical, at
// any DPI) up, every button of every screen - the header's tabs, the footer's Undo / Redo / Save and
// problems, the body's buttons and number fields, pagers - lies inside the window; below it the
// tool declares only the "window too small" notice.
//
// SND_UI_WINDOW=<w>x<h> prints what lies outside at that size (used to choose the minimum).
#include <cstdio>
#include <cstdlib>
#include <string>

#include "indep_s7_ui_fixture.hpp"
#include "snd/test/test.hpp"
#include "snd/tool/ui/layout_metrics.hpp"

using namespace iu;

namespace {

std::map<std::string, std::string> Files() {
  std::map<std::string, std::string> f = LargeFiles(3, 2, 6, 4, 12);
  f["segments/Seg.json"] = "{\"type\": \"segment\", \"guid\": \"" + G(22) + "\", \"name\": \"Seg\", \"asset\": \"" + G(7) +
                           "\", \"bars\": 1, \"tempo\": [{\"tick\": 0, \"bpm\": 120, \"beatsPerBar\": 4, \"beatUnit\": 4}],"
                           " \"markers\": [{\"name\": \"M1\", \"frame\": 100, \"kind\": \"user\"}],"
                           " \"stems\": [{\"channelFirst\": 0, \"channelCount\": 1}]}\n";
  f["snapshots/Quiet.json"] = "{\"type\": \"snapshot\", \"guid\": \"" + G(9) + "\", \"name\": \"Quiet\", \"bank\": \"Main\", "
                              "\"values\": [{\"kind\": \"effectParam\", \"bus\": \"Sfx\", \"effect\": 0, \"param\": 0, \"value\": 500}]}\n";
  f["voiceLimitGroups/G.json"] = "{\"type\": \"voiceLimitGroup\", \"guid\": \"" + G(43) + "\", \"name\": \"G\", \"maxVoices\": 4}\n";
  return f;
}

struct View {
  const char* name;
  void (*select)(Rig&);
};
const View kViews[] = {
    {"mixer", [](Rig& r) { r.Show(ui::Screen::Mixer); }},
    {"prof0", [](Rig& r) { r.app->view().profiler.tab = 0; r.Show(ui::Screen::Profiler); }},
    {"prof1", [](Rig& r) { r.app->view().profiler.tab = 1; r.Show(ui::Screen::Profiler); }},
    {"prof2", [](Rig& r) { r.app->view().profiler.tab = 2; r.Show(ui::Screen::Profiler); }},
    {"prof3", [](Rig& r) { r.app->view().profiler.tab = 3; r.Show(ui::Screen::Profiler); }},
    {"prof4", [](Rig& r) { r.app->view().profiler.tab = 4; r.Show(ui::Screen::Profiler); }},
    {"prof5", [](Rig& r) { r.app->view().profiler.tab = 5; r.Show(ui::Screen::Profiler); }},
    {"replay", [](Rig& r) { r.Show(ui::Screen::Replay); }},
    {"problems", [](Rig& r) { r.Show(ui::Screen::Problems); }},
    {"link", [](Rig& r) { r.Show(ui::Screen::Link); }},
    {"bus", [](Rig& r) { r.app->Navigate(G(3)); }},
    {"snapshot", [](Rig& r) { r.app->Navigate(G(9)); }},
    {"group", [](Rig& r) { r.app->Navigate(G(43)); }},
    {"ducking", [](Rig& r) { r.app->Navigate(G(3)); r.app->view().buses.tab = 2; }},
    {"categories", [](Rig& r) { r.app->Navigate(G(5)); }},
    {"event", [](Rig& r) { r.app->Navigate(G(8)); r.Frame(); r.ClickId("ev.t0.c0"); }},
    {"params", [](Rig& r) {
       r.app->Navigate(G(6));
       r.Frame();
       if (r.app->view().params.event.empty()) r.app->view().params.event = G(8);
       r.Frame();
       r.ClickId("pc.pt.0");
     }},
    {"spatial", [](Rig& r) { r.app->Navigate(G(8)); r.app->OnScreen(ui::Screen::Spatial); }},
    {"segment", [](Rig& r) { r.app->Navigate(G(22)); }},
    {"playlist", [](Rig& r) { r.app->Navigate(G(23)); }},
    {"rules", [](Rig& r) { r.app->Navigate(G(24)); }},
    {"stingers", [](Rig& r) { r.app->Navigate(G(24)); r.app->view().music.tab = 3; }},
    {"material", [](Rig& r) { r.app->Navigate(G(25)); }},
    {"patch", [](Rig& r) { r.app->Navigate(G(26)); r.Frame(); r.ClickId("pa.node.osc0"); }},
};

// Buttons outside a window of `w` x `h` logical at `dpi` on every view; "" if none. `small`: the
// number of views that showed the notice; `others`: elements besides the notice while small.
std::string Outside(float w, float h, float dpi, unsigned* small, unsigned* others) {
  std::string bad;
  *small = 0;
  *others = 0;
  for (const View& v : kViews) {
    Rig r(Files());
    r.app->SetViewport(static_cast<int>(w * dpi), static_cast<int>(h * dpi), dpi);
    v.select(r);
    r.Frame();
    if (r.El("body.small") != nullptr) {
      ++*small;
      for (const tela::PlacedElement& p : r.rt().elements()) {
        if (p.element.id != "body.small" && p.element.id != "root") ++*others;
      }
      continue;
    }
    for (const tela::PlacedElement& p : r.rt().elements()) {
      if (p.element.kind != tela::ElementKind::button) continue;
      const tela::Rect& b = p.bounds;
      if (b.width <= 0.0f || b.height <= 0.0f) continue;
      if (b.x < -0.5f || b.y < -0.5f || b.x + b.width > w + 0.5f || b.y + b.height > h + 0.5f) {
        char t[200];
        std::snprintf(t, sizeof t, "\n  %gx%g/%g %s %s (x %.0f y %.0f w %.0f h %.0f)", static_cast<double>(w),
                      static_cast<double>(h), static_cast<double>(dpi), v.name, p.element.id.c_str(),
                      static_cast<double>(b.x), static_cast<double>(b.y), static_cast<double>(b.width),
                      static_cast<double>(b.height));
        if (bad.size() < 8000) bad += t;
      }
    }
  }
  return bad;
}

}  // namespace

SND_TEST(ToolUiWindow, EveryButtonIsInsideFromTheSmallestWindowUp) {
  if (const char* e = std::getenv("SND_UI_WINDOW")) {  // probing
    float w = 0.0f, h = 0.0f;
    if (std::sscanf(e, "%fx%f", &w, &h) == 2) {
      for (float dpi : {0.25f, 1.0f, 2.0f, 4.0f}) {
        unsigned small = 0, others = 0;
        std::printf("%gx%g/%g small %u:%s\n", static_cast<double>(w), static_cast<double>(h), static_cast<double>(dpi), small,
                    Outside(w, h, dpi, &small, &others).c_str());
      }
    }
    return;
  }
  const float mw = ui::kMinEditorWidth, mh = ui::kMinEditorHeight;
  struct Size {
    float w, h;
  };
  for (Size s : {Size{mw, mh}, Size{mw + 1.0f, mh}, Size{mw, mh + 37.0f}, Size{mw + 133.0f, mh + 61.0f}, Size{1280.0f, 800.0f},
                 Size{1920.0f, 1080.0f}}) {
    for (float dpi : {0.25f, 1.0f, 1.5f, 2.0f, 4.0f}) {
      // Within the viewports the tool takes (App::SetViewport: 16384 px a side, 16777216 px in all).
      if (s.w * dpi > 16384.0f || s.h * dpi > 16384.0f || s.w * dpi * s.h * dpi > 16777216.0f) continue;
      unsigned small = 0, others = 0;
      const std::string bad = Outside(s.w, s.h, dpi, &small, &others);
      SND_CHECK_MSG(bad.empty(), "%s", bad.c_str());
      SND_CHECK_EQ(small, 0u);
    }
  }
}

SND_TEST(ToolUiWindow, BelowTheSmallestWindowOnlyTheNotice) {
  const float mw = ui::kMinEditorWidth, mh = ui::kMinEditorHeight;
  struct Size {
    float w, h;
  };
  for (Size s : {Size{mw - 1.0f, mh}, Size{mw, mh - 1.0f}, Size{320.0f, 240.0f}, Size{640.0f, 400.0f}, Size{1.0f, 1.0f}}) {
    for (float dpi : {0.25f, 1.0f, 2.0f, 4.0f}) {
      unsigned small = 0, others = 0;
      Outside(s.w, s.h, dpi, &small, &others);
      SND_CHECK_MSG(small == sizeof kViews / sizeof kViews[0], "%gx%g/%g: %u views small", static_cast<double>(s.w),
                    static_cast<double>(s.h), static_cast<double>(dpi), small);
      SND_CHECK_EQ(others, 0u);
    }
  }
  // Keys still work while small: F-keys change the screen, Ctrl+Z undoes.
  Rig r(Files());
  r.app->SetViewport(400, 300, 1.0f);
  r.Show(ui::Screen::Mixer);
  r.Key(ui::kKeyF1 + 2u);
  SND_CHECK(r.app->view().screen != ui::Screen::Mixer);
  SND_CHECK(r.El("body.small") != nullptr);
}

// The mixer pages its strips at the smallest window (with more strips than fit).
SND_TEST(ToolUiWindow, MixerPagesAtTheSmallestWindow) {
  Rig r(LargeFiles(3, 20, 6, 4, 12));
  r.app->SetViewport(static_cast<int>(ui::kMinEditorWidth), static_cast<int>(ui::kMinEditorHeight), 1.0f);
  r.Show(ui::Screen::Mixer);
  SND_REQUIRE(r.El("mixer.next") != nullptr);
  const std::uint32_t page0 = r.app->view().mixerPage;
  SND_REQUIRE(r.ClickId("mixer.next"));
  SND_CHECK_EQ(r.app->view().mixerPage, page0 + 1u);
  for (int i = 0; i < 20; ++i) r.ClickId("mixer.next");  // stops at the last page
  const std::uint32_t last = r.app->view().mixerPage;
  SND_REQUIRE(r.ClickId("mixer.next"));
  SND_CHECK_EQ(r.app->view().mixerPage, last);
  SND_REQUIRE(r.ClickId("mixer.prev"));
  SND_CHECK_EQ(r.app->view().mixerPage, last - 1u);
}
