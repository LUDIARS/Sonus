// D-162 across groups 1 and 3. Compare two parameter contributions in the
// actual stereo output against the equivalent single distance curve.
#include "indep_g1_scene.hpp"

namespace {
using namespace indep_g1;
using Target = bf::CurveTarget;
constexpr auto kControl = HashId("Control");

std::vector<std::uint8_t> SceneBytes(Target target, float distanceValue, bool withDistance, bool parameters) {
  const bool wet = target == Target::ReverbSend;
  auto bank = Signal(wet);
  if (wet) bank.buses[0].sends = {{1, -120, 0}};
  if (withDistance) Curve(bank, target, distanceValue, distanceValue);
  const auto first = static_cast<unsigned>(bank.curves.size());
  if (parameters) {
    bank.params.push_back({kControl, 0, 0, 1, 0, 0});
    for (int i = 0; i < 2; ++i) {
      bank.curves.push_back({kControl, static_cast<unsigned>(target), 4, {{0, 0}, {1, 1}}});
    }
  }
  const auto bytes = bank.Build(HashId("CombinedCurves"));
  bf::FileHeader header{};
  std::memcpy(&header, bytes.data(), sizeof(header));
  bt::BankWriter writer;
  for (unsigned i = 0; i < header.chunkCount; ++i) {
    bf::ChunkEntry entry{};
    std::memcpy(&entry, bytes.data() + header.chunkTableOffset + i * sizeof(entry), sizeof(entry));
    if (entry.fourcc == bf::chunk::EVNT) continue;
    const auto* p = bytes.data() + entry.offset;
    writer.AddBlob(entry.fourcc, {p, p + entry.bytes}, entry.flags);
  }
  bt::TableBuilder event(bf::chunk::EVNT);
  const auto row = event.AddRow();
  event.U32(bf::col::EVNT_id, row, kEvent);
  event.U32(bf::col::EVNT_trackCount, row, 1);
  event.U32(bf::col::EVNT_flags, row, bf::kEvent3D);
  if (withDistance) {
    const auto col = target == Target::Spread ? bf::col::EVNT_curveSpread :
                     target == Target::Blend3D ? bf::col::EVNT_curveBlend3D : bf::col::EVNT_curveReverbSend;
    event.U32(col, row, 0);
  }
  if (parameters) {
    event.U32(bf::col::EVNT_curveFirst, row, first);
    event.U32(bf::col::EVNT_curveCount, row, 2);
  }
  writer.AddTable(event);
  bt::BankHeaderInfo info;
  info.bankId = header.bankId;
  return writer.Finish(info);
}

void Product(Target target) {
  const snd_vec3 position = target == Target::Blend3D ? snd_vec3{0,0,4} : snd_vec3{1,0,0};
  for (bool withDistance : {false, true}) {
    const float baseline = withDistance ? 0.6f : 1.f;
    Scene scene(SceneBytes(target, baseline, withDistance, true));
    scene.Position(position);
    scene.Start();
    for (float x : {0.0f, 0.5f, 1.0f}) {
      SND_REQUIRE_EQ(scene.h.sys->SetParamGlobal(kControl, x), SND_OK);
      scene.h.Steps(8);
      Scene reference(SceneBytes(target, baseline * x * x, true, false));
      reference.Position(position);
      reference.Start();
      for (unsigned ch : {0u, 1u}) SND_CHECK_NEAR(scene.Level(ch), reference.Level(ch), 0.00005);
      if (target == Target::ReverbSend && x == 1.f) {
        SND_CHECK(scene.Level(0) + scene.Level(1) > 0.03);
      }
    }
  }
}
}

SND_TEST(IndepCrossCurves, ParameterSpreadMultipliesDistanceSpreadOrIdentity) { Product(Target::Spread); }
SND_TEST(IndepCrossCurves, ParameterBlendMultipliesDistanceBlendOrIdentity) { Product(Target::Blend3D); }
SND_TEST(IndepCrossCurves, ParameterSendMultipliesDistanceSendOrIdentity) { Product(Target::ReverbSend); }
