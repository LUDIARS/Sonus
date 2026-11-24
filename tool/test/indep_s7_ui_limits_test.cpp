// Independent tests of group U, limits (indep_s7_ui_properties.md E1, E4, E5, G1, G2, H): every
// screen over no document, an empty project, the base project and a large one, at DPI 0.25 .. 4
// and small / large windows, in a child process (a Tela exception ends -fno-exceptions code):
// no crash, at most 4096 elements, no duplicate id, drawings within Tela's limits; ids stable
// over frames and screen round trips.
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>

#include <tela/pixel_surface.hpp>

#include "indep_isolate.hpp"
#include "indep_s7_ui_fixture.hpp"
#include "snd/test/test.hpp"
#include "snd/tool/ui/image_file.hpp"
#include "snd/tool/ui/raster_compositor.hpp"

using namespace iu;

namespace {

std::map<std::string, std::string> EmptyFiles() {
  std::map<std::string, std::string> f;
  f["project.json"] = "{\"type\": \"project\", \"guid\": \"" + G(100) + "\", \"name\": \"Empty\", \"formatVersion\": 1}\n";
  return f;
}

// The objects each screen can show of the large project (selected with Navigate).
const char* const kLargeSelections[] = {"", "d0c00000-0000-4000-8000-000000000015" /* Big */,
                                        "d0c00000-0000-4000-8000-000000000018" /* Music */,
                                        "d0c00000-0000-4000-8000-000000000019" /* Phys */,
                                        "d0c00000-0000-4000-8000-00000000001a" /* Patch */,
                                        "d0c00000-0000-4000-8000-000000000014" /* Huge */,
                                        "d0c00000-0000-4000-8000-000000000016" /* Seg */};

// Runs every screen at one size and DPI; returns 0 or a code naming the first problem (printed).
int RunAll(const std::map<std::string, std::string>* files, int w, int h, float dpi, bool selections) {
  Rig r(files != nullptr ? *files : std::map<std::string, std::string>{}, files != nullptr, 1280, 800, 1.0f);
  r.app->SetViewport(w, h, dpi);
  int code = 0;
  const int nsel = selections ? static_cast<int>(sizeof kLargeSelections / sizeof kLargeSelections[0]) : 1;
  for (int sel = 0; sel < nsel && code == 0; ++sel) {
    if (kLargeSelections[sel][0] != '\0') {
      r.app->Navigate(kLargeSelections[sel]);
      r.Frame();
    }
    for (ui::Screen sc : kAllScreens) {
      std::printf("[screen %s sel %d %dx%d dpi %.2f]\n", ui::ScreenName(sc), sel, w, h, static_cast<double>(dpi));
      std::fflush(stdout);
      r.Show(sc);
      r.Frame();
      const std::string dup = FirstDuplicate(r.rt());
      const std::string lim = LimitProblem(r.rt());
      if (!dup.empty()) {
        std::printf("  duplicate id %s\n", dup.c_str());
        code = 2;
      }
      if (!lim.empty()) {
        std::printf("  limit: %s\n", lim.c_str());
        code = 3;
      }
      if (r.rt().elements().empty()) {
        std::printf("  nothing declared\n");
        code = 4;
      }
      if (code != 0) break;
      // A compose must succeed or refuse (no crash); at DPI 4 large windows are skipped (memory).
      if (static_cast<double>(w) * h <= 2.0e6) {
        tela::PixelSurface px;
        ui::Compose(r.rt(), ui::ComposeOptions{}, &px);
      }
    }
  }
  return code;
}

}  // namespace

// G1, E5: every screen, documents of every size, DPI 0.25 .. 4, small to large windows.
SND_TEST(IndepS7UiLimits, EveryScreenEveryProjectEveryDpiStaysInTelasLimits) {
  const std::map<std::string, std::string> empty = EmptyFiles();
  const std::map<std::string, std::string> base = doctest::BaseFiles();
  const std::map<std::string, std::string> large = LargeFiles();
  struct Doc {
    const char* name;
    const std::map<std::string, std::string>* files;
    bool selections;
  };
  const Doc docs[] = {{"none", nullptr, false}, {"empty", &empty, false}, {"base", &base, false}, {"large", &large, true}};
  struct Combo {
    float dpi;
    int w, h;
  };
  // Small documents: every DPI at three sizes. The large one: both DPI ends at both size ends and
  // the ordinary window at DPI 1 and 2 (the large document is slow to declare in sanitizer builds).
  const Combo all[] = {{0.25f, 320, 200}, {0.25f, 1280, 800}, {0.25f, 3840, 2160}, {1.0f, 320, 200},
                       {1.0f, 1280, 800}, {1.0f, 3840, 2160},  {2.0f, 320, 200},   {2.0f, 1280, 800},
                       {2.0f, 3840, 2160}, {4.0f, 320, 200},   {4.0f, 1280, 800},  {4.0f, 3840, 2160}};
  const Combo few[] = {{0.25f, 320, 200}, {0.25f, 3840, 2160}, {1.0f, 1280, 800},
                       {2.0f, 1280, 800}, {4.0f, 320, 200},    {4.0f, 3840, 2160}};
  for (const Doc& d : docs) {
    const Combo* combos = d.selections ? few : all;
    const std::size_t n = d.selections ? sizeof few / sizeof few[0] : sizeof all / sizeof all[0];
    for (std::size_t i = 0; i < n; ++i) {
      const Combo c = combos[i];
      const int code = indep::Isolated([&] { return RunAll(d.files, c.w, c.h, c.dpi, d.selections); });
      if (code == indep::kNotIsolated) {
        SND_CHECK_EQ(RunAll(d.files, c.w, c.h, c.dpi, d.selections), 0);
        continue;
      }
      SND_CHECK_MSG(code == 0, "doc %s, %dx%d, dpi %.2f: code %d", d.name, c.w, c.h, static_cast<double>(c.dpi), code);
    }
  }
}

// G2: DPI outside 0.25 .. 4 (0, negative, NaN, Inf, 8) and degenerate windows do not break.
SND_TEST(IndepS7UiLimits, OutOfRangeViewportsAreClamped) {
  const int code = indep::Isolated([] {
    Rig r;
    int bad = 0;
    struct V {
      int w, h;
      float dpi;
    };
    for (V v : {V{1280, 800, 0.0f}, V{1280, 800, -1.0f}, V{1280, 800, std::nanf("")}, V{1280, 800, 8.0f},
                V{1280, 800, INFINITY}, V{0, 0, 1.0f}, V{1, 1, 1.0f}, V{-5, 100, 1.0f}, V{100000, 10, 1.0f}}) {
      r.app->SetViewport(v.w, v.h, v.dpi);
      for (ui::Screen sc : kAllScreens) {
        r.Show(sc);
        if (!LimitProblem(r.rt()).empty() || !FirstDuplicate(r.rt()).empty()) ++bad;
      }
      const tela::Viewport& vp = r.rt().viewport();
      if (!(vp.dpi_scale >= 0.25f && vp.dpi_scale <= 4.0f)) {
        std::printf("dpi %f kept as %f\n", static_cast<double>(v.dpi), static_cast<double>(vp.dpi_scale));
        ++bad;
      }
    }
    return bad == 0 ? 0 : 1;
  });
  SND_CHECK_MSG(code == 0 || code == indep::kNotIsolated, "code %d", code);
}

// E1, E4: ids are the same over two frames and after a round trip through every other screen.
SND_TEST(IndepS7UiLimits, IdsAreStableOverFramesAndRoundTrips) {
  Rig r(LargeFiles(60, 20, 30, 16, 40));
  for (const char* sel : kLargeSelections) {
    if (sel[0] != '\0') r.app->Navigate(sel);
    r.Frame();
    for (ui::Screen sc : kAllScreens) {
      r.Show(sc);
      const std::set<std::string> a = r.Ids();
      r.Frame();
      SND_CHECK_MSG(r.Ids() == a, "%s changes ids between frames", ui::ScreenName(sc));
      for (ui::Screen other : kAllScreens) r.Show(other);
      r.Show(sc);
      const std::set<std::string> b = r.Ids();
      // Only the screen's own elements are compared (the header / footer show messages).
      std::size_t missing = 0;
      for (const std::string& id : a) missing += b.count(id) == 0 ? 1u : 0u;
      SND_CHECK_MSG(missing == 0, "%s: %u ids gone after a round trip", ui::ScreenName(sc),
                    static_cast<unsigned>(missing));
    }
  }
}

// Writes the large project to SND_INDEP_LARGE_DIR (the headless CTest's fixture). Without the
// variable it does nothing.
SND_TEST(IndepS7UiLimits, WriteLargeProjectForHeadless) {
  const char* dir = std::getenv("SND_INDEP_LARGE_DIR");
  if (dir == nullptr) {
    SND_CHECK(true);
    return;
  }
  namespace fs = std::filesystem;
  std::error_code ec;
  fs::remove_all(dir, ec);
  for (const auto& [file, text] : LargeFiles()) {
    const fs::path p = fs::path(dir) / file;
    fs::create_directories(p.parent_path(), ec);
    std::FILE* f = std::fopen(p.string().c_str(), "wb");
    SND_REQUIRE(f != nullptr);
    std::fwrite(text.data(), 1, text.size(), f);
    std::fclose(f);
  }
  const std::vector<std::uint8_t> wav = doctest::ToneWav();
  fs::create_directories(fs::path(dir) / "audio", ec);
  std::FILE* f = std::fopen((fs::path(dir) / "audio/tone.wav").string().c_str(), "wb");
  SND_REQUIRE(f != nullptr);
  std::fwrite(wav.data(), 1, wav.size(), f);
  std::fclose(f);
  // And an empty project next to it.
  const fs::path empty = fs::path(dir).string() + "-empty";
  fs::remove_all(empty, ec);
  fs::create_directories(empty, ec);
  f = std::fopen((empty / "project.json").string().c_str(), "wb");
  SND_REQUIRE(f != nullptr);
  const std::string t = EmptyFiles()["project.json"];
  std::fwrite(t.data(), 1, t.size(), f);
  std::fclose(f);
}
