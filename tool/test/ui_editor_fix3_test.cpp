// The third rework's screen-side checks: a transition preview's request time has a range, numbers
// beyond the bank's floats are refused on the Params screen and no text shows inf / nan.
#include <cstdio>
#include <string>

#include "indep_s7_ui_fixture.hpp"
#include "snd/test/test.hpp"
#include "snd/tool/curve_editor.hpp"

using namespace iu;

namespace {

bool Enter(Rig& r, const std::string& id, const std::string& text) {  // true: taken (entry closed)
  if (!r.ClickId(id) || !r.app->text().active()) return false;
  for (int i = 0; i < 64; ++i) r.app->Key(ui::KeyEvent{ui::kKeyBackspace, 0, 0});
  r.Type(text);
  r.Key(ui::kKeyEnter);
  const bool taken = !r.app->text().active();
  if (!taken) r.Key(ui::kKeyEscape);
  return taken;
}

bool AnyNonFinite(const Rig& r) {
  for (const tela::PlacedElement& e : r.rt().elements()) {
    std::string l = e.element.label;
    for (const char* ok : {"-inf dB", "0=inf"}) {  // the promised fader text and the legend
      for (std::size_t at; (at = l.find(ok)) != std::string::npos;) l.erase(at, std::string(ok).size());
    }
    std::string w;
    for (std::size_t k = 0; k <= l.size(); ++k) {  // words of letters: "inf", "nan", "infinity"
      const char c = k < l.size() ? l[k] : ' ';
      if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')) {
        w += static_cast<char>(c | 0x20);
        continue;
      }
      if (w == "inf" || w == "nan" || w == "infinity") {
        std::printf("non-finite text %s = '%s'\n", e.element.id.c_str(), e.element.label.c_str());
        return true;
      }
      w.clear();
    }
  }
  return false;
}

}  // namespace

SND_TEST(ToolUiFix3, TransitionRequestTimeHasARange) {
  Rig r(LargeFiles(3, 2, 6, 4, 12));
  r.app->Navigate(G(24));
  r.Frame();
  SND_REQUIRE(r.El("mu.pv.req") != nullptr);
  const double before = r.app->view().music.requestSeconds;
  for (const char* bad : {"1e308", "1e300", "-1", "1000001"}) {
    SND_CHECK_MSG(!Enter(r, "mu.pv.req", bad), "request %s taken", bad);
    SND_CHECK_EQ(r.app->view().music.requestSeconds, before);
  }
  SND_CHECK(Enter(r, "mu.pv.req", "1000000"));
  r.Frame();
  SND_CHECK(!AnyNonFinite(r));
}

SND_TEST(ToolUiFix3, ParamsRefuseNumbersBeyondTheBanksFloats) {
  Rig r;
  r.app->Navigate(G(6));
  r.Frame();
  ParameterEditor pe(r.doc, G(6));
  const double max0 = pe.maxValue(), def0 = pe.defaultValue();
  for (const char* bad : {"1e308", "-1e308", "1e39"}) {
    SND_CHECK_MSG(!Enter(r, "pc.par.max", bad), "max %s taken", bad);
    SND_CHECK_MSG(!Enter(r, "pc.par.def", bad), "default %s taken", bad);
  }
  SND_CHECK_EQ(pe.maxValue(), max0);
  SND_CHECK_EQ(pe.defaultValue(), def0);
  for (ui::Screen sc : kAllScreens) {
    r.Show(sc);
    SND_CHECK_MSG(!AnyNonFinite(r), "%s shows inf / nan", ui::ScreenName(sc));
  }
}
