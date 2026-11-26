// The 3D preview screen: the top-down view (drag the emitter, the facing handles, pan and zoom),
// the values against SpatialPreview::Compute, the audition through the AuditionRouter on a real
// preview engine (the emitter follows the drag), disabled with the reason without a destination,
// and the listener / emitter in the image.
#include <cmath>

#include "snd/test/test.hpp"
#include "snd/tool/doc_build.hpp"
#include "snd/tool/preview/preview_engine.hpp"
#include "snd/tool/ui/image_file.hpp"
#include "snd/tool/ui/raster_compositor.hpp"
#include "ui_editor_helpers.hpp"

using namespace snd::tool;
using namespace edtest;

namespace {
const std::string kShot = doctest::G(8);
}

SND_TEST(ToolUiSpatial, DragTheEmitterAndTheFacings) {
  Editor t(ui::Screen::Spatial);
  SND_REQUIRE(t.app.view().spatial.event == kShot);
  SpatialPreview* sp = t.app.spatial();
  SND_REQUIRE(sp != nullptr);
  for (const char* id : {"sp.view", "sp.view.hit", "sp.listener", "sp.emitter", "sp.listener.yaw", "sp.emitter.yaw", "sp.v.0",
                         "sp.curve.plot", "sp.play", "sp.why"}) {
    SND_CHECK_MSG(t.Has(id), "missing %s", id);
  }
  SND_CHECK(t.Label("sp.play") == "(Play)");  // no audition router: disabled with the reason
  SND_CHECK(t.Label("sp.why").find("no audition router") != std::string::npos);
  // The emitter starts at half the maxDistance (50 m) to the front right: (15, 20), 25 m away.
  SND_CHECK(t.Label("sp.v.0").find("distance 25.000 m") != std::string::npos);
  SND_CHECK_NEAR(sp->emitter().position.x, 15.0f, 1e-5f);
  const float mpp = sp->metersPerPixel();
  t.Drag("sp.emitter", 40.0f, 0.0f, 4);
  SND_CHECK_NEAR(sp->emitter().position.x, 15.0f + 40.0f * mpp, 1e-3f);
  SND_CHECK_NEAR(sp->emitter().position.z, 20.0f, 1e-4f);
  const SpatialResult r = sp->Compute(2, 1, 0.0f);
  char b[64];
  std::snprintf(b, sizeof b, "distance %.3f m", r.distance);
  SND_CHECK(t.Label("sp.v.0").find(b) != std::string::npos);
  // The listener's handle dragged to its right: yaw 90 deg (facing +x).
  const tela::PlacedElement* l = t.El("sp.listener");
  const tela::PlacedElement* lh = t.El("sp.listener.yaw");
  SND_REQUIRE(l != nullptr && lh != nullptr);
  const tela::Point lc = Center(l), hc = Center(lh);
  t.Drag("sp.listener.yaw", lc.x + 28.0f - hc.x, lc.y - hc.y, 4);
  SND_CHECK_NEAR(sp->listener().forward.x, 1.0f, 1e-3f);
  SND_CHECK_NEAR(sp->listener().forward.z, 0.0f, 1e-3f);
  // Zoom on the background and pan.
  const tela::PlacedElement* v = t.El("sp.view.hit");
  t.ptr.Wheel(v->bounds.x + 10.0f, v->bounds.y + 10.0f, 1.0f);
  SND_CHECK_NEAR(sp->metersPerPixel(), mpp / 1.25f, 1e-6f);
  t.Frame();
  // Number fields set positions (no document step).
  const std::size_t undo0 = t.doc.UndoCount();
  SND_REQUIRE(t.Click("sp.l.x.inc"));
  SND_CHECK_NEAR(sp->listener().position.x, 0.5f, 1e-5f);
  SND_CHECK_EQ(t.doc.UndoCount(), undo0);
  SND_REQUIRE(t.Click("sp.fit"));
}

SND_TEST(ToolUiSpatial, AuditionOnThePreviewEngineFollowsTheDrag) {
  DocHolder h;
  std::string error;
  std::unique_ptr<preview::PreviewEngine> engine = preview::PreviewEngine::Create(preview::PreviewEngineDesc{}, &error);
  SND_REQUIRE_MSG(engine != nullptr, "%s", error.c_str());
  SND_REQUIRE_MSG(engine->LoadBuild(BuildDocument(h.doc), &error), "%s", error.c_str());
  AuditionRouter router;
  router.Bind(nullptr, engine.get());
  router.SetTarget(AuditionTarget::Preview);
  ui::Workspace ws;
  ws.audition = &router;
  ws.preview = engine.get();
  Editor t(ui::Screen::Spatial, 1280, 800, 1.0f, doctest::BaseFiles(), ws);
  SND_CHECK(t.Label("sp.play") == "Play");
  SND_CHECK(!t.Has("sp.why"));
  SND_REQUIRE(t.Click("sp.play"));
  SND_CHECK(t.app.view().spatial.playing);
  SND_CHECK(router.moving() != 0u);
  engine->Render(4, nullptr);
  SND_CHECK_EQ(engine->Status().activePlaybacks, 1u);
  t.Drag("sp.emitter", 30.0f, 0.0f, 3);
  engine->Render(4, nullptr);
  SND_CHECK(engine->Status().running);
  SND_REQUIRE(t.Click("sp.play"));  // Stop
  SND_CHECK(!t.app.view().spatial.playing);
  engine->Render(2, nullptr);
  SND_CHECK_EQ(engine->Status().activePlaybacks, 0u);
  // The other destination: no game connected -> disabled with the reason.
  SND_REQUIRE(t.Click("sp.target"));
  SND_CHECK(t.Label("sp.play") == "(Play)");
  SND_CHECK(t.Label("sp.why").find("not connected") != std::string::npos);
}

SND_TEST(ToolUiSpatial, ListenerAndEmitterAreDrawnWhereTheyAre) {
  for (float dpi : {1.0f, 2.0f}) {
    Editor t(ui::Screen::Spatial, static_cast<int>(1280 * dpi), static_cast<int>(800 * dpi), dpi);
    SpatialPreview* sp = t.app.spatial();
    SND_REQUIRE(sp != nullptr);
    const tela::PlacedElement* v = t.El("sp.view");
    SND_REQUIRE(v != nullptr);
    float ex = 0.0f, ey = 0.0f;
    sp->WorldToScreen(sp->emitter().position.x, sp->emitter().position.z, v->bounds.width, v->bounds.height, &ex, &ey);
    tela::PixelSurface img;
    SND_REQUIRE(ui::Compose(t.app.runtime(), ui::ComposeOptions{}, &img));
    unsigned char rgb[3];
    const tela::Color a = t.app.palette().accent;
    ui::PixelRgb(img, static_cast<int>((v->bounds.x + ex) * dpi), static_cast<int>((v->bounds.y + ey + 3.0f) * dpi), rgb);
    SND_CHECK_MSG(std::abs(rgb[0] - a.r) <= 4 && std::abs(rgb[1] - a.g) <= 4 && std::abs(rgb[2] - a.b) <= 4,
                  "emitter at dpi %g: %d %d %d", static_cast<double>(dpi), rgb[0], rgb[1], rgb[2]);
    // The maxDistance circle: 50 m around the emitter, on its left edge.
    const float rpx = 50.0f / sp->metersPerPixel();
    const tela::Color dim = t.app.palette().textDim;
    bool found = false;
    for (int dx = -2; dx <= 2 && !found; ++dx) {
      ui::PixelRgb(img, static_cast<int>((v->bounds.x + ex - rpx) * dpi) + dx, static_cast<int>((v->bounds.y + ey) * dpi), rgb);
      found = std::abs(rgb[0] - dim.r) <= 30 && std::abs(rgb[1] - dim.g) <= 30 && std::abs(rgb[2] - dim.b) <= 30;
    }
    SND_CHECK_MSG(found, "maxDistance circle at dpi %g", static_cast<double>(dpi));
  }
}
