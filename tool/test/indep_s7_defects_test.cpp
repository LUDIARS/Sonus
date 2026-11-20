// Defects found by the independent tests of group M (tool/test/indep_s7_model_properties.md,
// spec/handoff/parallel/C-notes/model-indep.md). Every test here FAILS ON PURPOSE against the
// implementation it was written for: each documents one defect, with the expectation taken
// from the spec / the public header. They are not to be relaxed; the implementation is to be
// fixed. Each is its own CTest test (tool_indep_s7_Defect_*).
#include <cmath>
#include <cstdlib>
#include <string>
#include <system_error>
#include <vector>

#include "indep_s7_fixture.hpp"
#include "snd/test/test.hpp"
#include "snd/tool/doc_build.hpp"
#include "snd/tool/document.hpp"
#include "snd/tool/material_editor.hpp"
#include "snd/tool/music_editor.hpp"

using namespace indep_s7;
using namespace snd::tool;

// Brief M-test / M-impl: numbers must survive a round trip, "負のゼロ" included.
// document.hpp: "edited numbers are written with the shortest text that reads back to the same
// double (FormatNumber)". FormatNumber(-0.0) returns "0", which reads back as +0.
SND_TEST(IndepS7Defect, Defect_NegativeZeroLosesItsSign) {
  const std::string s = FormatNumber(-0.0);
  SND_CHECK(std::signbit(std::strtod(s.c_str(), nullptr)));
  ProjectDocument doc;
  SND_REQUIRE(LoadBase(doc));
  SND_REQUIRE(doc.SetNumber(kSfx, "volumeDb", -0.0));
  json::Value v;
  json::ParseError pe;
  SND_REQUIRE(json::Parse(doc.TextOf(*doc.Find(kSfx)), &v, &pe));
  SND_CHECK(std::signbit(NumberAt(v, "volumeDb", 1.0)));
}

// document.hpp, ProjectDocument::Set: "No step if the value does not change. false: no object /
// bad path." Setting the value a member already has returns false (object and path are fine).
SND_TEST(IndepS7Defect, Defect_SetSameValueReturnsFalse) {
  ProjectDocument doc;
  SND_REQUIRE(LoadBase(doc));
  SND_CHECK(doc.SetNumber(kSfx, "volumeDb", -3));  // the loaded value
  SND_CHECK_EQ(doc.UndoCount(), 0u);
}

// music_editor.hpp: "stems inside the material's channels". A separate-file stem (StemDesc::asset)
// is not checked against its own file: channels 0..1 of a mono file are accepted (AddStem), and
// SetStem checks no channel range at all (channels 1..2 of a stereo segment material).
SND_TEST(IndepS7Defect, Defect_StemChannelsOutsideTheMaterialAccepted) {
  ProjectDocument doc;
  SND_REQUIRE(LoadBase(doc));
  SegmentEditor s(doc, kSegA);  // music.wav: stereo
  StemDesc ok;
  ok.channelFirst = 0;
  ok.channelCount = 2;
  SND_REQUIRE(s.AddStem(ok));
  StemDesc own;
  own.channelFirst = 1;
  own.channelCount = 2;  // channel 2 does not exist
  SND_CHECK(!s.AddStem(own));
  SND_CHECK(!s.SetStem(0, own));
  StemDesc sep;
  sep.channelFirst = 0;
  sep.channelCount = 2;
  sep.asset = kTone;  // tone.wav: mono
  SND_CHECK(!s.AddStem(sep));
  SND_CHECK(!s.SetStem(0, sep));
}

// doc_build.hpp: LocateField gives "which bank, chunk, row and column a member is written to";
// M-impl: "どのオブジェクトの編集が、どのバンクのどのチャンク・どの行・どの列に当たるかを引ける対応表".
// The table (FieldColumns) has no entry for the music event's rules and stingers, the segment's
// tempo map / markers / bars / entry and exit, the stems, nor the distance curves' points, so
// LocateField returns nothing for members that are in the bank.
SND_TEST(IndepS7Defect, Defect_LocateFieldMissesMusicSegmentAndDistanceCurveMembers) {
  ProjectDocument doc;
  SND_REQUIRE(LoadBase(doc));
  json::Value curve = json::MakeObject();
  json::Value pts = json::MakeArray();
  for (int k = 0; k < 2; ++k) {
    json::Value p = json::MakeArray();
    p.items.push_back(Number(k * 50.0));
    p.items.push_back(Number(k * -30.0));
    pts.items.push_back(p);
  }
  json::Add(curve, "points", pts);
  SND_REQUIRE(doc.Set(kEngine, "distanceCurves.volume", curve));
  const DocBuild b = BuildDocument(doc);
  SND_REQUIRE(b.ok);
  const char* const fields[][2] = {{kBgm, "music.transitions[0].fadeOutBeats"},
                                   {kBgm, "music.transitions[1].fadeInBeats"},
                                   {kBgm, "music.stingers[0].timingCount"},
                                   {kSegA, "tempo[0].bpm"},
                                   {kSegA, "markers[0].frame"},
                                   {kSegA, "bars"},
                                   {kEngine, "distanceCurves.volume.points[1][1]"}};
  for (const auto& f : fields) {
    if (LocateField(b, doc, f[0], f[1]).empty()) {
      snd::test::Note("LocateField(%s) gives no cell", f[1]);
      SND_CHECK(false);
    }
  }
}

// doc_build.hpp CellRef::liveWritable: "the runtime accepts SetValue of this column (core
// CheckEdit)". An optional column the build left out of the table (all rows at the default) is
// reported live-writable by LocateField, but the runtime cannot write it (bank
// FindWritableColumn: SND_ERR_NOT_FOUND for an absent column); DiffBuilds treats the same edit
// as a chunk change (the column appears). LocateField and ClassifyEdit disagree.
SND_TEST(IndepS7Defect, Defect_LocateFieldLiveWritableForAbsentColumn) {
  ProjectDocument doc;
  SND_REQUIRE(LoadBase(doc));
  const DocBuild b = BuildDocument(doc);
  SND_REQUIRE(b.ok);
  std::uint32_t absent = 0;
  for (const char* path : {"tracks[0].clips[0].pitchCents", "tracks[0].clips[0].fadeInMs", "tracks[0].clips[0].random.filterHz",
                           "comboWindowMs", "tracks[0].clips[0].layer[1]"}) {
    for (const CellRef& c : LocateField(b, doc, kShot, path)) {
      const BankImage* img = b.FindBank(c.bank, c.language);
      SND_REQUIRE(img != nullptr && img->view() != nullptr);
      if (img->view()->HasColumn(c.chunk, c.column)) continue;
      ++absent;
      if (c.liveWritable) {
        snd::test::Note("%s: column %u of %08x is not in the bank, but liveWritable", path, c.column, c.chunk);
        SND_CHECK(false);
      }
    }
  }
  SND_CHECK(absent > 0u);  // the case exists in this fixture
}

// document.hpp Save: "Applies PendingChanges ... Afterwards the document is clean." A deletion
// that fails (here: the file was replaced by a non-empty directory; in practice a locked or
// read-only file) is ignored: Save returns true and marks the document clean, the old file stays.
// After a rename that moved the file, both files then hold the same GUID and the next Load drops
// one of them as a duplicate. Expected: Save reports the failure and keeps the deletion pending.
SND_TEST(IndepS7Defect, Defect_SaveIgnoresAFailedDeletion) {
  const std::string dir = TempDir("defect_delete");
  WriteBaseProject(dir);
  ProjectDocument doc;
  SND_REQUIRE(doc.Load(dir));
  SND_REQUIRE(doc.Remove(kCatUi));
  std::error_code ec;
  fs::remove(dir + "/categories/UI.json", ec);
  fs::create_directories(dir + "/categories/UI.json", ec);
  WriteFile(dir + "/categories/UI.json/keep", "x");
  std::string err;
  const bool saved = doc.Save(&err);
  SND_CHECK(!saved);
  SND_CHECK(!err.empty());
  SND_CHECK(doc.IsDirty());
}

// A material table without materials (the material editor makes one when every material is
// removed) is built through `put(vec.data(), 0)` in buildtool/src/build.cpp (PHYS): memcpy with a
// null source, undefined behaviour (UBSan: "null pointer passed as argument 2"). Fails only in
// the -DSND_SANITIZE=address,undefined build; IndepS7Build.E1_CheckAgreesWithBuildOverRandomEdits
// hits the same line there. Expected: no undefined behaviour (skip the copy for 0 bytes).
SND_TEST(IndepS7Defect, Defect_EmptyMaterialTableBuildIsUndefinedBehaviour) {
  ProjectDocument doc;
  SND_REQUIRE(LoadBase(doc));
  MaterialEditor m(doc, kPhys);
  while (!m.Materials().empty()) SND_REQUIRE(m.RemoveMaterial(m.Materials()[0]));
  const DocBuild b = BuildDocument(doc);
  SND_CHECK(b.ok);
}
