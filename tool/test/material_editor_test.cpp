// Material table editor model: materials and their numbers, single / pair (symmetric) /
// footstep / default events, the resolution order of 9.3 and the fallback list, strength
// curves, undo / redo, save / load, the PHYS chunk the build writes.
#include <cmath>
#include <cstring>

#include "doc_fixture.hpp"
#include "snd/format/bank_format.hpp"
#include "snd/test/test.hpp"
#include "snd/tool/material_editor.hpp"

using namespace snd::tool;
using namespace doctest;
namespace bf = snd::bank_format;

namespace {

std::map<std::string, std::string> Files() {
  std::map<std::string, std::string> f = BaseFiles();
  for (int i = 0; i < 5; ++i) {
    const std::string n = "SE/M" + std::to_string(i);
    f["events/M" + std::to_string(i) + ".json"] = "{\"type\": \"event\", \"guid\": \"" + G(200 + i) + "\", \"name\": \"" + n +
                                                  "\", \"bank\": \"Main\"}";
  }
  f["materials/Phys.json"] = "{\"type\": \"materialTable\", \"guid\": \"" + G(210) + "\", \"name\": \"Phys/Default\", \"bank\": \"Main\", "
                             "\"materials\": [\"metal\", \"wood\"], \"single\": {\"metal\": {\"impact\": \"SE/M0\"}}, "
                             "\"notes\": \"table notes\"}";
  return f;
}

}  // namespace

SND_TEST(MaterialEditor, MaterialsNumbersAndReferences) {
  ProjectDocument doc;
  LoadBase(doc, Files());
  MaterialEditor m(doc, G(210));
  SND_REQUIRE(m.valid());
  Renumbering r;
  SND_CHECK(m.AddMaterial("glass", 2, &r));
  SND_CHECK(r.oldToNew == (std::vector<int>{0, 1}));  // appending keeps the numbers
  SND_CHECK(!m.AddMaterial("wood", 0));
  SND_CHECK(!m.AddMaterial("", 0));
  SND_CHECK(!m.AddMaterial("stone", 9));
  SND_CHECK(m.SetSingle("wood", ContactKind::Impact, G(201)));
  SND_CHECK(m.SetSingle("glass", ContactKind::Break, G(202)));
  SND_CHECK(m.SetPair("glass", "metal", G(203)));
  SND_CHECK(m.SetPair("metal", "glass", G(204)));  // symmetric: replaces
  SND_CHECK_EQ(m.Pair("glass", "metal"), G(204));
  SND_CHECK_EQ(FindPath(doc.Find(G(210))->doc, "pairs")->items.size(), std::size_t{1});
  SND_CHECK(m.SetFootstep("wood", G(200)));
  SND_CHECK(m.SetDefault(ContactKind::Impact, G(204)));
  SND_CHECK(!m.SetSingle("stone", ContactKind::Impact, G(200)));   // unknown material
  SND_CHECK(!m.SetSingle("wood", ContactKind::Impact, G(3)));      // not an event
  // Rename follows everywhere; insert at 0 renumbers.
  SND_CHECK(m.RenameMaterial("glass", "crystal"));
  SND_CHECK_EQ(m.Pair("metal", "crystal"), G(204));
  SND_CHECK_EQ(m.Single("crystal", ContactKind::Break), G(202));
  SND_CHECK(m.AddMaterial("stone", 0, &r));
  SND_CHECK(r.oldToNew == (std::vector<int>{1, 2, 3}));
  SND_CHECK(m.MoveMaterial(0, 3, &r));
  SND_CHECK(r.oldToNew == (std::vector<int>{3, 0, 1, 2}));
  // Remove: its keys and pairs go, the numbers after it move down.
  SND_CHECK(m.RemoveMaterial("crystal", &r));
  SND_CHECK(r.oldToNew == (std::vector<int>{0, 1, -1, 2}));
  SND_CHECK(m.Pair("metal", "crystal").empty());
  SND_CHECK(FindPath(doc.Find(G(210))->doc, "pairs")->items.empty());
  SND_CHECK_EQ(StringAt(doc.Find(G(210))->doc, "notes"), std::string("table notes"));
  // Undo all: the file again.
  while (doc.Undo()) {
  }
  SND_CHECK(doc.PendingChanges().empty());
}

SND_TEST(MaterialEditor, ResolutionOrderAndFallbacks) {
  ProjectDocument doc;
  LoadBase(doc, Files());
  MaterialEditor m(doc, G(210));
  SND_REQUIRE(m.AddMaterial("glass", 2));
  // metal: impact M0; wood: impact M1; glass: nothing; default impact M4; pair metal x glass M3.
  SND_REQUIRE(m.SetSingle("wood", ContactKind::Impact, G(201)));
  SND_REQUIRE(m.SetDefault(ContactKind::Impact, G(204)));
  SND_REQUIRE(m.SetPair("metal", "glass", G(203)));
  MaterialResolution r = m.Resolve(0, 1, ContactKind::Impact);  // metal x wood: both singles
  SND_CHECK(r.source == "single" && r.events == (std::vector<std::string>{G(200), G(201)}));
  r = m.Resolve(2, 0, ContactKind::Impact);  // glass x metal: the pair (order does not matter)
  SND_CHECK(r.a == 0u && r.b == 2u && r.source == "pair" && r.events == (std::vector<std::string>{G(203)}));
  r = m.Resolve(1, 2, ContactKind::Impact);  // wood x glass: wood's single + the default for glass
  SND_CHECK(r.source == "single+default" && r.events == (std::vector<std::string>{G(201), G(204)}));
  r = m.Resolve(2, 2, ContactKind::Impact);  // glass x glass: the default once
  SND_CHECK(r.source == "default" && r.events.size() == 1u);
  r = m.Resolve(0, 1, ContactKind::Slide);   // nothing for slides
  SND_CHECK(r.source == "none" && r.events.empty());
  r = m.Resolve(0, 2, ContactKind::Roll);    // the pair overrides every kind (no kind in PHYS pairs)
  SND_CHECK(r.source == "pair");
  // Fallbacks: every (pair, kind) that does not sound the materials' own events.
  const std::vector<MaterialResolution> fb = m.Fallbacks();
  std::uint32_t impact = 0;
  for (const MaterialResolution& x : fb) impact += x.kind == ContactKind::Impact ? 1u : 0u;
  SND_CHECK_EQ(impact, 2u);  // wood x glass, glass x glass
  SND_CHECK_EQ(fb.size(), std::size_t{2 + 5 * 3});  // + slide / roll / break ("none") of the 5 pairs without an override
}

SND_TEST(MaterialEditor, StrengthCurvesAndThePhysChunk) {
  TempDir dir("phys");
  WriteAll(dir, Files());
  ProjectDocument doc;
  SND_REQUIRE(doc.Load(dir.path().string()));
  MaterialEditor m(doc, G(210));
  StrengthCurves c;
  c.impulseToVelocity[0] = 0.5f;
  c.impulseToVelocity[1] = 2.0f;
  c.impulseToVelocity[2] = 8.0f;
  c.impulseToVelocity[3] = 20.0f;
  c.massToPitchCents[0] = 200.0f;
  c.massToPitchCents[1] = -400.0f;
  c.speedToVolumeDb[0] = -24.0f;
  c.speedToVolumeDb[1] = 0.0f;
  SND_REQUIRE(m.SetCurves("metal", c));
  StrengthCurves bad = c;
  bad.impulseToVelocity[1] = 0.1f;  // not ascending
  SND_CHECK(!m.SetCurves("metal", bad));
  const StrengthCurves back = m.Curves("metal");
  SND_CHECK(std::memcmp(&back, &c, sizeof c) == 0);
  StrengthResult s = EvaluateStrength(c, 0.3f, 1.0f, 0.1f);
  SND_CHECK_EQ(s.layer, -1);
  s = EvaluateStrength(c, 5.0f, 10.0f, 1.0f);
  SND_CHECK_EQ(s.layer, 1);
  SND_CHECK_NEAR(s.velocity, 0.25, 1e-6);
  SND_CHECK_NEAR(s.pitchCents, -100.0, 1e-3);  // 10 kg: half way on the log scale
  SND_CHECK_NEAR(s.volumeDb, -12.0, 1e-3);     // 1 m/s: half way
  s = EvaluateStrength(c, 100.0f, 1000.0f, 100.0f);
  SND_CHECK(s.layer == 2 && s.velocity == 1.0f && s.pitchCents == -400.0f && s.volumeDb == 0.0f);
  SND_REQUIRE(m.SetSingle("wood", ContactKind::Slide, G(201)));
  SND_REQUIRE(m.SetPair("wood", "metal", G(202)));
  SND_REQUIRE(m.SetFootstep("wood", G(203)));
  SND_REQUIRE(m.SetDefault(ContactKind::Break, G(204)));
  // The build's PHYS: event ids at the table positions, the curves of "metal".
  const DocBuild b = BuildDocument(doc);
  SND_REQUIRE_MSG(b.ok, "%s", Dump(b.issues).c_str());
  std::uint64_t bytes = 0;
  const std::uint8_t* phys = b.FindBank("Main")->view()->Chunk(bf::chunk::PHYS, &bytes);
  SND_REQUIRE(phys != nullptr && bytes >= 16 + 64);
  bf::PhysTable t{};
  std::uint64_t tablesOffset = 0;
  std::memcpy(&tablesOffset, phys + 8, 8);
  std::memcpy(&t, phys + tablesOffset, sizeof t);
  SND_CHECK_EQ(t.materialCount, 2u);
  auto u32 = [&](std::uint64_t off) {
    std::uint32_t v = 0;
    std::memcpy(&v, phys + off, 4);
    return v;
  };
  SND_CHECK_EQ(u32(t.singleOffset + (0 * 4 + 0) * 4), snd::HashId("SE/M0"));   // metal impact
  SND_CHECK_EQ(u32(t.singleOffset + (1 * 4 + 1) * 4), snd::HashId("SE/M1"));   // wood slide
  SND_CHECK_EQ(u32(t.pairOffset + (0 * 2 + 1) * 4), snd::HashId("SE/M2"));     // table[min*N+max]
  SND_CHECK_EQ(u32(t.footstepOffset + 1 * 4), snd::HashId("SE/M3"));
  SND_CHECK_EQ(t.defaultEvent[3], snd::HashId("SE/M4"));
  bf::PhysCurves pc{};
  std::memcpy(&pc, phys + t.curveOffset, sizeof pc);
  SND_CHECK(pc.impulseToVelocity[3] == 20.0f && pc.massToPitchCents[1] == -400.0f && pc.speedToVolumeDb[0] == -24.0f);
  // Save / load round trip.
  SND_REQUIRE(doc.Save());
  ProjectDocument again;
  SND_REQUIRE(again.Load(dir.path().string()));
  SND_CHECK(JsonEqual(again.Find(G(210))->doc, doc.Find(G(210))->doc));
  SND_CHECK(again.PendingChanges().empty());
}
