// The fourth rework: names are shown as they are (even "inf" / "nan"), only numbers are made finite
// where they become text; a broken tempo point is a problem and the Music screen shows it without
// overflowing its display arithmetic.
#include <climits>
#include <cmath>
#include <cstdio>
#include <string>

#include "indep_s7_ui_fixture.hpp"
#include "snd/test/test.hpp"
#include "snd/tool/names.hpp"
#include "snd/tool/ui/kit.hpp"

using namespace iu;

namespace {

bool HasLabel(const Rig& r, const std::string& text) {
  for (const tela::PlacedElement& e : r.rt().elements()) {
    if (e.element.label.find(text) != std::string::npos) return true;
  }
  return false;
}

}  // namespace

SND_TEST(ToolUiFix4, NamesAreShownAsTheyAre) {
  for (const char* name : {"inf", "nan", "-inf", "NaN"}) {
    std::map<std::string, std::string> f = doctest::BaseFiles();
    f["buses/Reverb.json"] = "{\"type\": \"bus\", \"guid\": \"" + G(4) + "\", \"name\": \"" + name + "\", \"parent\": \"" + G(2) + "\"}\n";
    f["categories/SE.json"] = "{\"type\": \"category\", \"guid\": \"" + G(5) + "\", \"name\": \"" + name + "x\"}\n";
    Rig r(f);
    r.app->Navigate(G(4));
    r.Frame();
    SND_CHECK_MSG(HasLabel(r, std::string(" ") + name) || HasLabel(r, name), "bus %s not shown", name);
    // The graph order names every bus; the name is in it as it is.
    const tela::PlacedElement* g = r.El("mx.graphorder");
    if (g != nullptr) SND_CHECK_MSG(g->element.label.find(name) != std::string::npos, "graph order: '%s'", g->element.label.c_str());
    r.Show(ui::Screen::Mixer);
    SND_CHECK_MSG(HasLabel(r, name), "mixer strip %s not shown", name);
  }
  // Numbers: "-" when not finite, wherever they become text.
  char b[96];
  ui::SnprintfFinite(b, sizeof b, "%s = %.3f (%s) %+.2f dB %6.1f %.*f %llu %d", "inf", std::nan(""), "nan",
                     -static_cast<double>(INFINITY), static_cast<double>(INFINITY), 2, 1.5, 7ull, -3);
  SND_CHECK_EQ(std::string(b), std::string("inf = - (nan) - dB - 1.50 7 -3"));
  SND_CHECK_EQ(FormatDb(std::nanf("")), std::string("- dB"));
  SND_CHECK_EQ(FormatDb(-INFINITY), std::string("-inf dB"));  // the promised text (stage 2)
  SND_CHECK_EQ(ui::NumberText(std::nan(""), 2, "s"), std::string("-"));
}

SND_TEST(ToolUiFix4, BrokenTempoPointsAreProblemsAndShownWithoutOverflow) {
  for (const char* v : {"-5", "1e300", "4294967296", "2.5", "0", "1e6"}) {
    const std::string n = v;
    std::map<std::string, std::string> f = LargeFiles(2, 1, 4, 3, 12);
    f["segments/Seg.json"] = "{\"type\": \"segment\", \"guid\": \"" + G(22) + "\", \"name\": \"Seg\", \"asset\": \"" + G(7) +
                             "\", \"bars\": 1, \"tempo\": [{\"tick\": 0, \"bpm\": " + n + ", \"beatsPerBar\": " + n +
                             ", \"beatUnit\": " + n + "}]}\n";
    Rig r(f);
    r.app->RefreshProblems();
    bool reported = false;
    for (const DocIssue& i : r.app->problems()) reported = reported || i.file.find("Seg") != std::string::npos;
    SND_CHECK_MSG(reported, "tempo %s: no problem for the segment", v);
    r.app->Navigate(G(24));
    r.Frame();
    const tela::PlacedElement* line = r.El("mu.pv.text");
    SND_REQUIRE(line != nullptr);
    const std::string& l = line->element.label;
    SND_CHECK_MSG(l.find("214748364") == std::string::npos, "tempo %s: '%s'", v, l.c_str());
    r.app->Navigate(G(22));
    r.Frame();
  }
}
