// The editors' computations against the runtime itself: curve evaluation against core's
// EvaluateCurve on the built bank, parameter-curve composition and the 3D preview against
// the output of a real core System playing the built bank (DC material: the output is the
// gain times the pan matrix exactly).
#include <cmath>
#include <cstring>
#include <memory>

#include "doc_fixture.hpp"
#include "harness.hpp"
#include "snd/bank/bank_view.hpp"
#include "snd/bank/validate.hpp"
#include "snd/format/bank_format.hpp"
#include "snd/test/test.hpp"
#include "snd/tool/curve_editor.hpp"
#include "snd/tool/event_editor.hpp"
#include "snd/tool/spatial_preview.hpp"

// core's curve evaluation (core/src/core_impl.hpp; external linkage in sonus_core).
namespace snd::core {
float EvaluateCurve(const snd::bank::BankView& v, std::uint32_t curveRow, float x) noexcept;
}

using namespace snd::tool;
using namespace doctest;
namespace bf = snd::bank_format;

namespace {

constexpr float kDc = 0.25f;

// A parsed copy of a built bank (64-byte aligned).
struct View {
  std::vector<std::uint8_t> storage;
  snd::bank::BankView view;
  bool ok = false;
  explicit View(const std::vector<std::uint8_t>& bytes) {
    storage.assign(bytes.size() + 64, 0);
    const std::uintptr_t a = reinterpret_cast<std::uintptr_t>(storage.data());
    const std::size_t off = static_cast<std::size_t>((64u - (a & 63u)) & 63u);
    std::memcpy(storage.data() + off, bytes.data(), bytes.size());
    snd::bank::ParseOptions po;
    po.platform = bf::kPlatformAny;
    snd::bank::ParseError e{};
    ok = snd::bank::ParseBank(storage.data() + off, bytes.size(), po, &view, &e) == SND_OK;
  }
};

// The base project with a looping DC asset (PCMF, played exactly by the test sample kind).
void LoadDc(ProjectDocument& doc) {
  std::map<std::string, std::string> f = BaseFiles();
  f["assets/dc.json"] = "{\"type\": \"asset\", \"guid\": \"" + G(70) + "\", \"name\": \"dc\", \"file\": \"audio/dc.wav\", "
                        "\"convert\": {\"default\": {\"codec\": \"PCMF\"}}, \"loop\": {\"start\": 0, \"end\": 4800}}";
  f["events/Dc3D.json"] =
      "{\"type\": \"event\", \"guid\": \"" + G(71) + "\", \"name\": \"Dc3D\", \"bank\": \"Main\", \"is3D\": true, "
      "\"cone\": {\"innerDeg\": 90, \"outerDeg\": 270, \"outerDb\": -12},"
      " \"distanceCurves\": {\"volume\": {\"points\": [[1, 0], [10, -6, 2], [40, -30]]},"
      " \"spread\": {\"points\": [[0, 1], [20, 0]]}, \"blend3D\": {\"points\": [[0, 0.2], [8, 1]]}},"
      " \"tracks\": [{\"clips\": [{\"asset\": \"dc\", \"loopCount\": 0}]}]}";
  f["events/DcInv.json"] =
      "{\"type\": \"event\", \"guid\": \"" + G(72) + "\", \"name\": \"DcInv\", \"bank\": \"Main\", \"is3D\": true, \"maxDistance\": 12, "
      " \"tracks\": [{\"clips\": [{\"asset\": \"dc\", \"loopCount\": 0}]}]}";
  f["events/DcParam.json"] =
      "{\"type\": \"event\", \"guid\": \"" + G(73) + "\", \"name\": \"DcParam\", \"bank\": \"Main\", "
      " \"curves\": [{\"parameter\": \"Speed\", \"target\": \"volume\", \"points\": [[0, 0], [10, -24, 0.5]]},"
      "              {\"parameter\": \"Speed\", \"target\": \"volume\", \"points\": [[0, -3], [10, 3]]},"
      "              {\"parameter\": \"Speed\", \"target\": \"pan\", \"points\": [[0, -0.5], [10, 0.5]]}],"
      " \"tracks\": [{\"clips\": [{\"asset\": \"dc\", \"loopCount\": 0}]}]}";
  f["parameters/Speed.json"] = "{\"type\": \"parameter\", \"guid\": \"" + G(6) + "\", \"name\": \"Speed\", \"min\": 0, \"max\": 10, \"default\": 1}";
  LoadBase(doc, f);
  doc.SetBinaryFile("audio/dc.wav", ToneWav(4800, 48000, 1, kDc));
}

// Steady output of the last rendered frame.
void LastFrame(const coretest::Harness& h, float* l, float* r) {
  const std::size_t n = h.TotalFrames();
  *l = h.At(n - 1, 0);
  *r = h.At(n - 1, 1);
}

}  // namespace

SND_TEST(RuntimeCompare, CurveEvaluationIsTheCores) {
  ProjectDocument doc;
  LoadDc(doc);
  const DocBuild b = BuildDocument(doc);
  SND_REQUIRE_MSG(b.ok, "%s", Dump(b.issues).c_str());
  View v(b.FindBank("Main")->bytes);
  SND_REQUIRE(v.ok);
  // Dc3D's distance volume curve and DcParam's parameter curves.
  const std::uint32_t evRows = v.view.Rows(bf::chunk::EVNT);
  std::uint32_t dc3d = bf::kNone, dcParam = bf::kNone;
  for (std::uint32_t r = 0; r < evRows; ++r) {
    const std::uint32_t id = v.view.Column<snd::bank::col::EVNT_id>()[r];
    if (id == snd::HashId("Dc3D")) dc3d = r;
    if (id == snd::HashId("DcParam")) dcParam = r;
  }
  SND_REQUIRE(dc3d != bf::kNone && dcParam != bf::kNone);
  struct Pair {
    std::uint32_t row;
    std::vector<CurvePoint> points;
  };
  std::vector<Pair> pairs;
  pairs.push_back({v.view.Column<snd::bank::col::EVNT_curveVolume>()[dc3d], CurveEditor(doc, G(71), "distanceCurves.volume").Points()});
  pairs.push_back({v.view.Column<snd::bank::col::EVNT_curveSpread>()[dc3d], CurveEditor(doc, G(71), "distanceCurves.spread").Points()});
  const std::uint32_t first = v.view.Column<snd::bank::col::EVNT_curveFirst>()[dcParam];
  for (std::uint32_t k = 0; k < 3; ++k) {
    pairs.push_back({first + k, CurveEditor(doc, G(73), "curves[" + std::to_string(k) + "]").Points()});
  }
  std::uint32_t compared = 0;
  for (const Pair& p : pairs) {
    for (float x = -5.0f; x <= 60.0f; x += 0.37f) {
      const float a = snd::core::EvaluateCurve(v.view, p.row, x);
      const float t = EvaluateCurve(p.points, x);
      SND_CHECK_MSG(std::memcmp(&a, &t, sizeof a) == 0, "row %u x %g: core %.9g tool %.9g", p.row, x, a, t);
      ++compared;
    }
  }
  SND_CHECK(compared > 500u);
}

SND_TEST(RuntimeCompare, SpatialPreviewMatchesTheCoreOutput) {
  ProjectDocument doc;
  LoadDc(doc);
  const DocBuild b = BuildDocument(doc);
  SND_REQUIRE_MSG(b.ok, "%s", Dump(b.issues).c_str());
  coretest::Harness h;
  SND_REQUIRE(h.created == SND_OK);
  h.Load("Main.sndb", b.FindBank("Main")->bytes);
  snd_listener_desc ld{};
  ld.position = snd_vec3{0, 0, 0};
  ld.forward = snd_vec3{0, 0, 1};
  ld.up = snd_vec3{0, 1, 0};
  ld.enabled = 1;
  SND_REQUIRE(h.sys->SetListener(0, &ld) == SND_OK);
  struct Pose {
    const char* event;
    std::string guid;
    float x, z, yaw;
  };
  const Pose poses[] = {
      {"Dc3D", G(71), 0.0f, 5.0f, 3.14159265f},   // ahead, facing the listener
      {"Dc3D", G(71), 4.0f, 3.0f, 0.4f},          // right-front, cone partly off
      {"Dc3D", G(71), -12.0f, -9.0f, 1.2f},       // left-behind, beyond the blend ramp
      {"Dc3D", G(71), 0.3f, 0.4f, 0.0f},          // inside 1 m
      {"DcInv", G(72), -3.0f, 4.0f, 0.0f},        // inverse distance (5 m)
      {"DcInv", G(72), 9.0f, 12.0f, 0.0f},        // beyond maxDistance (15 m > 12)
  };
  for (const Pose& p : poses) {
    SpatialPreview prev(doc, p.guid);
    prev.MoveEmitter(p.x, p.z);
    prev.SetEmitterYaw(p.yaw);
    const SpatialResult want = prev.Compute(h.Ch(), 1);
    const snd_emitter_handle em = h.sys->CreateEmitter();
    const snd_vec3 pos{p.x, 0.0f, p.z};
    const snd_vec3 fwd{std::sin(p.yaw), 0.0f, std::cos(p.yaw)};
    const snd_vec3 vel{0, 0, 0};
    SND_REQUIRE(h.sys->SetTransform(em, &pos, &fwd, &vel) == SND_OK);
    const snd_playback_handle pb = h.sys->Play(snd::HashId(p.event), em, nullptr);
    h.Clear();
    h.Steps(8);  // ramps settle (gain and pan move once per block)
    float l = 0, r = 0;
    LastFrame(h, &l, &r);
    SND_CHECK_MSG(std::fabs(l - kDc * want.outputGain[0]) < 2e-5f && std::fabs(r - kDc * want.outputGain[1]) < 2e-5f,
                  "%s at (%g, %g): core L %.7f R %.7f, model L %.7f R %.7f (gain %.6f az %.4f spread %.3f blend %.3f)",
                  p.event, p.x, p.z, l, r, kDc * want.outputGain[0], kDc * want.outputGain[1], want.linearGain,
                  want.azimuthRad, want.spread, want.blend3D);
    if (want.linearGain > 1e-3f) SND_CHECK(std::fabs(l) + std::fabs(r) > 1e-4f);  // not vacuous
    h.sys->Stop(pb, 0.0f, nullptr);
    h.sys->DestroyEmitter(em);
    h.Steps(4);
  }
}

SND_TEST(RuntimeCompare, ParameterCompositionMatchesTheCoreOutput) {
  ProjectDocument doc;
  LoadDc(doc);
  const DocBuild b = BuildDocument(doc);
  SND_REQUIRE_MSG(b.ok, "%s", Dump(b.issues).c_str());
  coretest::Harness h;
  SND_REQUIRE(h.created == SND_OK);
  h.Load("Main.sndb", b.FindBank("Main")->bytes);
  const snd_playback_handle pb = h.sys->Play(snd::HashId("DcParam"), snd_emitter_handle{}, nullptr);
  for (float speed : {-1.0f, 0.0f, 2.5f, 7.0f, 10.0f, 12.0f}) {
    SND_REQUIRE(h.sys->SetParamGlobal(snd::HashId("Speed"), speed) == SND_OK);
    h.Clear();
    h.Steps(6);
    SpatialPreview prev(doc, G(73));
    ParamScopeValues s;
    s.hasGlobal = true;
    s.global = speed;
    prev.parameters()[G(6)] = s;
    const SpatialResult want = prev.Compute(h.Ch(), 1);
    float l = 0, r = 0;
    LastFrame(h, &l, &r);
    SND_CHECK_MSG(std::fabs(l - kDc * want.outputGain[0]) < 2e-5f && std::fabs(r - kDc * want.outputGain[1]) < 2e-5f,
                  "Speed %g: core L %.7f R %.7f, model L %.7f R %.7f (volume %.4f dB, pan %.4f)", speed, l, r,
                  kDc * want.outputGain[0], kDc * want.outputGain[1], want.gainDb, want.pan);
    SND_CHECK(std::fabs(l) + std::fabs(r) > 1e-3f);
  }
  h.sys->Stop(pb, 0.0f, nullptr);
}
