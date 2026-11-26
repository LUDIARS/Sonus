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
#include "snd/tool/mixer_editor.hpp"
#include "snd/tool/curve_editor.hpp"
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

// (Former defect 4.) D-177d: the editor refuses to remove the last material (the document does
// not change); D-180d: a table with no materials, written straight into the document, is a
// build error reached without undefined behaviour (memcpy from null; run under UBSan).
SND_TEST(IndepS7Defect, Defect_EmptyMaterialTableBuildIsUndefinedBehaviour) {
  ProjectDocument doc;
  SND_REQUIRE(LoadBase(doc));
  MaterialEditor m(doc, kPhys);
  while (m.Materials().size() > 1u) SND_REQUIRE(m.RemoveMaterial(m.Materials()[0]));
  const auto image = Image(doc);
  const std::size_t steps = doc.UndoCount();
  SND_CHECK(!m.RemoveMaterial(m.Materials()[0]));
  SND_CHECK(Image(doc) == image);
  SND_CHECK_EQ(doc.UndoCount(), steps);
  // Zero materials through the raw document (not an editor rule).
  SND_REQUIRE(doc.Set(kPhys, "materials", json::MakeArray()));
  const DocBuild b = BuildDocument(doc);
  SND_CHECK(!b.ok);
  bool reported = false;
  for (const auto& i : IssuesOf(b.issues, kPhys)) reported |= i.error;
  SND_CHECK(reported);
}

// Found in the re-check. D-177b: "値が変わらない Set は成功（true）。履歴に手を足さず、dirty にもしない。
// 全エディタの設定関数で揃える." Setting the value a member already has, where the file writes it
// explicitly at its default ("preFader": false, "kind": "continuous", a marker's "kind": "user", a
// rule's "late": "nextGrid"), erases the member: the document changes (a step, dirty) although no
// value does. Moving a material to its own position returns false.
SND_TEST(IndepS7Defect, Defect_SameValueSettersRewriteExplicitDefaults) {
  ProjectDocument doc;
  SND_REQUIRE(LoadBase(doc));
  const auto image = Image(doc);
  MixerEditor mx(doc);
  ParameterEditor speed(doc, kSpeed);
  SegmentEditor seg(doc, kSegA);
  MusicEditor music(doc, kBgm);
  MaterialEditor mat(doc, kPhys);
  const struct {
    const char* name;
    bool ok;
  } r[] = {{"SetSendPreFader(false)", mx.SetSendPreFader(kSfx, 0, false)},
           {"SetSwitch(false)", speed.SetSwitch(false)},
           {"SetMarker(same)", seg.SetMarker(0, seg.Markers()[0])},
           {"SetRule(same)", music.SetRule(0, music.Rules()[0])},
           {"MoveMaterial(1, 1)", mat.MoveMaterial(1, 1)}};
  for (const auto& x : r) {
    if (!x.ok) {
      snd::test::Note("%s returned false", x.name);
      SND_CHECK(false);
    }
  }
  SND_CHECK_EQ(doc.UndoCount(), 0u);
  SND_CHECK(doc.PendingChanges().empty());
  SND_CHECK(Image(doc) == image);
}

// Found in the re-check. D-176e: SaveTo the project's own directory is a save (R2''). The test
// `dir == dir_` compares text, so the same directory spelled otherwise ("dir/", "dir/.",
// "dir/../name") takes the "save as" path: it writes every object and copies, but does not delete
// the files of removed objects nor the old files of moved ones, and reports success with the
// document clean. On disk the removed object comes back and a moved object is there twice (the
// next Load drops one as a duplicate GUID).
SND_TEST(IndepS7Defect, Defect_SaveToSameDirectoryOtherSpellingLeavesStaleFiles) {
  for (const char* suffix : {"/", "/."}) {
    const std::string dir = TempDir("defect_same");
    WriteBaseProject(dir);
    ProjectDocument doc;
    SND_REQUIRE(doc.Load(dir));
    SND_REQUIRE(doc.Remove(kCatUi));
    SND_REQUIRE(doc.Rename(kReverb, "Hall"));
    SND_REQUIRE(doc.SaveTo(dir + suffix));
    SND_CHECK(!fs::exists(dir + "/categories/UI.json"));
    SND_CHECK(!fs::exists(dir + "/buses/Reverb.json"));
    ProjectDocument back;
    SND_REQUIRE(back.Load(dir));
    SND_CHECK(back.loadIssues().empty());
    SND_CHECK_EQ(back.objects().size(), doc.objects().size());
  }
}
