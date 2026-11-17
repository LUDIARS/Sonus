// The whole tool against a live game, drawn with Tela::Raster: the master
// meter in the mixer image is as high as the level the game rendered, and both
// screens compose. With SND_TOOL_SNAPSHOT_DIR set, the images are written
// there (mixer.ppm, profiler.ppm, and *_text.ppm with text placeholders).
#include <cmath>
#include <cstdlib>

#include "snd/test/test.hpp"
#include "snd/tool/ui/image_file.hpp"
#include "snd/tool/ui/raster_compositor.hpp"
#include "snd/tool/ui/widgets.hpp"
#include "ui_helpers.hpp"

using namespace snd::tool;
using namespace uitest;

namespace {

void Save(const tela::Runtime& rt, const char* name) {
  const char* dir = std::getenv("SND_TOOL_SNAPSHOT_DIR");
  if (dir == nullptr || *dir == '\0') return;
  for (int text = 0; text < 2; ++text) {
    ui::ComposeOptions o;
    o.textPlaceholders = text != 0;
    tela::PixelSurface s;
    SND_CHECK(ui::Compose(rt, o, &s));
    const std::string path = std::string(dir) + "/" + name + (text != 0 ? "_text" : "") + ".ppm";
    SND_CHECK_MSG(ui::WritePpm(s, path), "cannot write %s", path.c_str());
  }
}

}  // namespace

SND_TEST(ToolUiSnapshot, LiveMixerAndProfilerImages) {
  tooltest::LiveRig rig;
  SND_REQUIRE(rig.ok);
  tooltest::ToolProject tool;
  SND_REQUIRE(tool.ok);
  rig.Play("Tone");
  rig.Play("Pad");
  Session s;
  s.UseProject(tool.project);
  ui::AppDesc d;
  d.connection = rig.Conn(tool.project.buildHash());
  d.connection.meterHz = 200;
  ui::App app(s, d);
  SND_REQUIRE_EQ(s.Connect(d.connection), SND_OK);
  auto run = [&](auto pred) { return rig.f.StepUntil([&] { app.Frame(tooltest::Now()); return pred(); }); };
  SND_REQUIRE(run([&] { return s.connection().state() == LinkState::Online; }));
  // Some state to show: Music at -10 dB, the delay's dry level, a parameter override, Sfx soloed.
  s.mixer().Set(Target{Control::BusFader, tooltest::Bus(tool.project, "Music"), 0, 0}, -10.0f);
  s.mixer().Set(Target{Control::EffectParam, tooltest::Bus(tool.project, "Sfx"), 0, 2}, -3.0f);
  s.mixer().Set(Target{Control::ParamOverride, 0, 0, 0}, 0.8f);
  s.mixer().Set(Target{Control::ParamOverrideOn, 0, 0, 0}, 1.0f);
  SND_REQUIRE(run([&] {
    return s.sync().Count(CellState::Pending) == 0u && s.sync().Count(CellState::Unsent) == 0u;
  }));
  for (int i = 0; i < 64; ++i) {
    rig.f.Step();
    app.Frame(tooltest::Now());
  }
  const float level = rig.Level();
  SND_REQUIRE(run([&] {
    const BusMeter* m = s.profiler().FindMeter(0u);
    return m != nullptr && std::fabs(m->peak[0] - level) < 1e-5f && s.profiler().timings().size() > 50u;
  }));
  app.Declare(tooltest::Now());
  // Master meter (left channel): its bar top in the image.
  const tela::PlacedElement* meter = Find(app.runtime(), ui::BusId(snd::HashId("Master")) + ".meter");
  SND_REQUIRE(meter != nullptr);
  tela::PixelSurface img;
  SND_REQUIRE(ui::Compose(app.runtime(), ui::ComposeOptions{}, &img));
  const float db = LinearToDb(level);
  const float expectTop = meter->bounds.y + meter->bounds.height * (1.0f - ui::MeterFraction(db));
  // Column at the left edge of channel 0 (outside the RMS inset), below the hold line.
  const int x = static_cast<int>(meter->bounds.x + 2.5f);
  const tela::Color zone = ui::MeterZoneColor(db, app.palette());
  int top = -1;
  unsigned char rgb[3];
  for (int y = static_cast<int>(expectTop) + 2; y >= static_cast<int>(meter->bounds.y); --y) {
    ui::PixelRgb(img, x, y, rgb);
    if (std::abs(rgb[0] - zone.r) <= 2 && std::abs(rgb[1] - zone.g) <= 2 && std::abs(rgb[2] - zone.b) <= 2) top = y;
  }
  SND_CHECK_MSG(top >= 0 && std::fabs(static_cast<float>(top) - expectTop) <= 1.5f,
                "meter top %d, expected %.1f (level %.4f)", top, static_cast<double>(expectTop), static_cast<double>(level));
  Save(app.runtime(), "mixer");
  // Profiler screen with a recording loaded.
  SND_REQUIRE(s.profiler().StartRecording(s.connection().client()));
  for (int i = 0; i < 32; ++i) {
    rig.f.Step();
    app.Frame(tooltest::Now());
  }
  SND_REQUIRE(s.profiler().StopRecording(s.connection().client()));
  SND_REQUIRE(run([&] { return s.profiler().recordingState() == RecordingState::Ready; }));
  tooltest::ProjectDir dir("snap");
  const std::string path = (dir.root() / "take.snrec").string();
  SND_REQUIRE(s.SaveRecording(path));
  SND_REQUIRE(s.LoadRecording(path));
  app.OnScreen(ui::Screen::Profiler);
  for (int i = 0; i < 8; ++i) {
    rig.f.Step();
    app.Frame(tooltest::Now());
  }
  SND_CHECK(Find(app.runtime(), "prof.voice.0.0") != nullptr);
  SND_CHECK(Find(app.runtime(), "prof.voice.1.0") != nullptr);
  SND_CHECK(ui::Compose(app.runtime(), ui::ComposeOptions{}, &img));
  Save(app.runtime(), "profiler");
}
