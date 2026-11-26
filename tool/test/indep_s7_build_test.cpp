// Independent tests of group M: checking and building the document, the banks the
// build writes and the live mapping (properties E*, F18, G6 of
// tool/test/indep_s7_model_properties.md).
#include <cstring>
#include <string>
#include <vector>

#include "indep_s7_fixture.hpp"
#include "indep_s7_ops.hpp"
#include "snd/buildtool/build.hpp"
#include "snd/buildtool/patch.hpp"
#include "snd/format/bank_format.hpp"
#include "snd/format/patch_program.hpp"
#include "snd/id.hpp"
#include "snd/test/test.hpp"
#include "snd/tool/doc_build.hpp"
#include "snd/tool/event_editor.hpp"
#include "snd/tool/material_editor.hpp"
#include "snd/tool/mixer_editor.hpp"
#include "snd/tool/music_editor.hpp"
#include "snd/tool/patch_editor.hpp"

using namespace indep_s7;
using namespace snd::tool;
namespace bf = snd::bank_format;

namespace {

std::uint32_t Errors(const std::vector<DocIssue>& issues) {
  std::uint32_t n = 0;
  for (const auto& i : issues) n += i.error ? 1u : 0u;
  return n;
}

const ParsedBank* Main(const DocBuild& b) {
  const BankImage* img = b.FindBank("Main");
  return img ? img->view() : nullptr;
}

float CellF(const ParsedBank& p, std::uint32_t chunk, std::uint32_t col, std::uint32_t row) { return p.F32(chunk, col, row); }

// Row of the n-th clip of track t of the event with `name`.
std::uint32_t ClipRow(const ParsedBank& p, const char* event, std::uint32_t track, std::uint32_t clip) {
  const std::uint32_t e = p.FindId(bf::chunk::EVNT, snd::HashId(event));
  if (e == bf::kNone) return bf::kNone;
  const std::uint32_t tf = p.U32(bf::chunk::EVNT, bf::col::EVNT_trackFirst, e);
  return p.U32(bf::chunk::TRAK, bf::col::TRAK_clipFirst, tf + track) + clip;
}

bool ColumnKnown(std::uint32_t chunk, std::uint16_t column) {
  for (const auto& c : bf::kColumns)
    if (c.chunk == chunk && c.columnId == column) return true;
  return false;
}

void ReadU32(const std::uint8_t* base, std::uint64_t off, std::uint32_t* out) { std::memcpy(out, base + off, 4); }

}  // namespace

// --- E1: no problem <-> builds ------------------------------------------------------------------------------

SND_TEST(IndepS7Build, E1_BaseFixtureChecksAndBuilds) {
  ProjectDocument doc;
  SND_REQUIRE(LoadBase(doc));
  const auto issues = CheckDocument(doc);
  for (const auto& i : issues)
    if (i.error) snd::test::Note("fixture problem: %s", i.text.c_str());
  SND_CHECK_EQ(Errors(issues), 0u);
  const DocBuild b = BuildDocument(doc);
  SND_CHECK(b.ok);
  SND_REQUIRE(Main(b) != nullptr);
  SND_CHECK(Main(b)->ok());
  SND_CHECK(b.buildHash != 0u);
}

SND_TEST(IndepS7Build, E1_CheckAgreesWithBuildOverRandomEdits) {
  for (std::uint64_t seed : {11ull, 12ull, 13ull}) {
    ProjectDocument doc;
    SND_REQUIRE(LoadBase(doc));
    Rng r(seed);
    for (std::uint32_t i = 0; i < 90; ++i) {
      std::string name;
      RandomOp(doc, r, &name, true, i);
      if (i % 15 != 14) continue;
      const auto issues = CheckDocument(doc);
      const DocBuild b = BuildDocument(doc);
      const std::uint32_t errors = Errors(issues);
      if ((errors == 0u) != b.ok) {
        snd::test::Note("seed %llu step %u: check has %u errors, build ok = %d", static_cast<unsigned long long>(seed), i, errors,
                        b.ok ? 1 : 0);
        SND_CHECK(false);
      }
      SND_CHECK_EQ(Errors(b.issues), errors);
      for (const auto& is : issues) {
        // Every problem of an object file names that object.
        if (!is.file.empty()) {
          const DocObject* owner = nullptr;
          for (const auto& o : doc.objects())
            if (o.file == is.file) owner = &o;
          if (owner != nullptr) SND_CHECK_EQ(is.guid, owner->guid);
        }
      }
      if (b.ok)
        for (const auto& bank : b.banks) SND_CHECK(bank.view() != nullptr && bank.view()->ok());
    }
  }
}

// --- E2, E9: problem positions ------------------------------------------------------------------------------

SND_TEST(IndepS7Build, E2_ProblemPointsAtTheMember) {
  auto files = BaseFiles();
  const std::string bad = "00000000-0000-4000-8000-0000000000ee";
  std::string& shot = files["events/SE/Shot.json"];
  const std::size_t at = shot.find(kTone);
  SND_REQUIRE(at != std::string::npos);
  shot.replace(at, std::strlen(kTone), bad);
  ProjectDocument doc;
  SND_REQUIRE(LoadBase(doc, files));
  const auto issues = IssuesOf(CheckDocument(doc), kShot);
  const DocIssue* hit = nullptr;
  for (const auto& i : issues)
    if (i.error && i.message.find(bad) != std::string::npos) hit = &i;
  SND_REQUIRE(hit != nullptr);
  SND_CHECK_EQ(hit->file, std::string("events/SE/Shot.json"));
  SND_CHECK_EQ(hit->path, std::string("tracks[0].clips[0].asset"));
  // Line of the reference in the file text (unchanged object: the file's lines).
  std::uint32_t line = 1;
  for (std::size_t k = 0; k < shot.find(bad); ++k) line += shot[k] == '\n' ? 1u : 0u;
  SND_CHECK_EQ(hit->line, line);
  // E9: the build's format.
  const std::string prefix = hit->file + ":" + std::to_string(hit->line) + ":" + std::to_string(hit->column) + ": error: ";
  SND_CHECK(hit->text.rfind(prefix, 0) == 0);
  SND_CHECK(hit->text.find(hit->message) != std::string::npos);
  // After an edit of the object, the position is in the text Save would write.
  SND_REQUIRE(doc.SetNumber(kShot, "priority", 101));
  const auto again = IssuesOf(CheckDocument(doc), kShot);
  const DocIssue* hit2 = nullptr;
  for (const auto& i : again)
    if (i.error && i.message.find(bad) != std::string::npos) hit2 = &i;
  SND_REQUIRE(hit2 != nullptr);
  SND_CHECK_EQ(hit2->path, std::string("tracks[0].clips[0].asset"));
  const std::string text = doc.TextOf(*doc.Find(kShot));
  std::uint32_t line2 = 1;
  for (std::size_t k = 0; k < text.find(bad); ++k) line2 += text[k] == '\n' ? 1u : 0u;
  SND_CHECK_EQ(hit2->line, line2);
}

SND_TEST(IndepS7Build, E2_RulesViolatedThroughRawEditsAreReported) {
  ProjectDocument doc;
  SND_REQUIRE(LoadBase(doc));
  // A second root bus (only possible with raw document edits): an error on that bus.
  const std::string root = doc.Create("bus", "Root2", nullptr);
  SND_REQUIRE(!root.empty());
  auto issues = CheckDocument(doc);
  SND_CHECK(Errors(issues) > 0u);
  SND_CHECK(Errors(IssuesOf(issues, root)) > 0u || Errors(issues) > 0u);
  SND_CHECK(!BuildDocument(doc).ok);
  SND_REQUIRE(doc.Undo());
  SND_CHECK_EQ(Errors(CheckDocument(doc)), 0u);
  // A bad enum value.
  SND_REQUIRE(doc.Set(kShot, "sequence", String("sometimes")));
  issues = IssuesOf(CheckDocument(doc), kShot);
  bool seq = false;
  for (const auto& i : issues) seq |= i.error && i.path == "sequence";
  SND_CHECK(seq);
}

// --- E3, E4: disk = memory, determinism -------------------------------------------------------------------

SND_TEST(IndepS7Build, E3_MemoryBuildEqualsDiskBuild) {
  const std::string dir = TempDir("e3");
  WriteBaseProject(dir);
  ProjectDocument doc;
  SND_REQUIRE(doc.Load(dir));
  SND_REQUIRE(doc.SetNumber(kSfx, "volumeDb", -7.5));
  EventEditor e(doc, kShot);
  SND_REQUIRE(e.AddAssetClip(0, kTone, 300));
  SND_REQUIRE(doc.Save());
  snd::buildtool::BuildOptions o;
  o.projectDir = dir;
  o.outDir = dir + "_out";
  snd::buildtool::Diagnostics diag;
  snd::buildtool::BuildResult res;
  SND_REQUIRE(snd::buildtool::Build(o, diag, &res));
  const DocBuild mem = BuildDocument(doc);
  SND_REQUIRE(mem.ok);
  SND_CHECK_EQ(mem.buildHash, res.projectBuildHash);
  for (const auto& bb : res.banks) {
    const BankImage* img = mem.FindBank(bb.name, bb.language);
    SND_REQUIRE(img != nullptr);
    const std::string disk = ReadFileText(o.outDir + "/" + bb.path);
    SND_CHECK(disk == std::string(img->bytes.begin(), img->bytes.end()));
    SND_CHECK_EQ(img->contentHash, bb.contentHash);
  }
  // The other outputs too (ids.json, header, streams).
  for (const auto& f : mem.files) {
    const std::string disk = ReadFileText(o.outDir + "/" + f.path);
    SND_CHECK(disk == std::string(f.bytes.begin(), f.bytes.end()));
  }
}

SND_TEST(IndepS7Build, E4_Deterministic) {
  ProjectDocument a, b;
  SND_REQUIRE(LoadBase(a));
  SND_REQUIRE(LoadBase(b));
  const DocBuild x = BuildDocument(a);
  const DocBuild y = BuildDocument(a);
  const DocBuild z = BuildDocument(b);
  SND_REQUIRE(x.ok && y.ok && z.ok);
  SND_CHECK_EQ(x.buildHash, y.buildHash);
  SND_CHECK_EQ(x.buildHash, z.buildHash);
  SND_REQUIRE(x.banks.size() == z.banks.size());
  for (std::size_t i = 0; i < x.banks.size(); ++i) {
    SND_CHECK(x.banks[i].bytes == y.banks[i].bytes);
    SND_CHECK(x.banks[i].bytes == z.banks[i].bytes);
  }
  // Same content after an edit and its undo.
  SND_REQUIRE(a.SetNumber(kSfx, "volumeDb", 3));
  SND_CHECK(BuildDocument(a).buildHash != x.buildHash);
  SND_REQUIRE(a.Undo());
  SND_CHECK_EQ(BuildDocument(a).buildHash, x.buildHash);
}

// --- E5, F18, G6: edited values in the bank ---------------------------------------------------------------

SND_TEST(IndepS7Build, E5_EditedValuesLandInTheirCells) {
  ProjectDocument doc;
  SND_REQUIRE(LoadBase(doc));
  MixerEditor mx(doc);
  SND_REQUIRE(mx.SetVolume(kSfx, -4.5));
  SND_REQUIRE(mx.SetSendLevel(kSfx, 0, -20.25));
  SND_REQUIRE(mx.SetDucking(kDuck, DuckField::AmountDb, -9.5));
  SND_REQUIRE(mx.SetCategoryVolume(kCatSfx, -1.25));
  EventEditor e(doc, kShot);
  SND_REQUIRE(e.SetClipNumber(0, 1, ClipNumber::StartMs, 123.5));
  SND_REQUIRE(e.SetClipRandom(0, 0, RandomField::Pan, 0.75));
  SND_REQUIRE(e.SetNumber(EventNumber::Priority, 200));
  SND_REQUIRE(doc.SetNumber(kSpeed, "rampRate", 2.5));
  MusicEditor me(doc, kBgm);
  auto rules = me.Rules();
  rules[0].fadeOutBeats = 1.75;
  SND_REQUIRE(me.SetRule(0, rules[0]));
  SegmentEditor seg(doc, kSegA);
  TempoPointDesc t = seg.Tempo()[0];
  t.bpm = 133.25;
  SND_REQUIRE(seg.SetTempo(0, t));
  const DocBuild b = BuildDocument(doc);
  SND_REQUIRE(b.ok);
  const ParsedBank* p = Main(b);
  SND_REQUIRE(p != nullptr && p->ok());
  const std::uint32_t sfx = p->FindId(bf::chunk::BUSS, snd::HashId("SFX"));
  SND_REQUIRE(sfx != bf::kNone);
  SND_CHECK_EQ(CellF(*p, bf::chunk::BUSS, bf::col::BUSS_volumeDb, sfx), -4.5f);
  const std::uint32_t send = p->U32(bf::chunk::BUSS, bf::col::BUSS_sendFirst, sfx);
  SND_CHECK_EQ(CellF(*p, bf::chunk::SEND, bf::col::SEND_levelDb, send), -20.25f);
  SND_CHECK_EQ(p->Rows(bf::chunk::DUCK), 1u);
  SND_CHECK_EQ(CellF(*p, bf::chunk::DUCK, bf::col::DUCK_amountDb, 0), -9.5f);
  SND_CHECK_EQ(CellF(*p, bf::chunk::DUCK, bf::col::DUCK_attackMs, 0), 20.0f);
  // (Categories are copied into a bank only when used: CatSFX is SE/Shot's, UI is used by nobody.)
  SND_CHECK_EQ(p->FindId(bf::chunk::CATG, snd::HashId("UI")), bf::kNone);
  const std::uint32_t ui = p->FindId(bf::chunk::CATG, snd::HashId("CatSFX"));
  SND_REQUIRE(ui != bf::kNone);
  SND_CHECK_EQ(CellF(*p, bf::chunk::CATG, bf::col::CATG_volumeDb, ui), -1.25f);
  const std::uint32_t c1 = ClipRow(*p, "SE/Shot", 0, 1);
  SND_CHECK_EQ(CellF(*p, bf::chunk::CLIP, bf::col::CLIP_startMs, c1), 123.5f);
  SND_CHECK_EQ(p->U32(bf::chunk::CLIP, bf::col::CLIP_loopCount, c1), 3u);
  const std::uint32_t c0 = ClipRow(*p, "SE/Shot", 0, 0);
  SND_CHECK_EQ(CellF(*p, bf::chunk::CLIP, bf::col::CLIP_randPan, c0), 0.75f);
  SND_CHECK_EQ(CellF(*p, bf::chunk::CLIP, bf::col::CLIP_weight, c0), 2.0f);
  const std::uint32_t ev = p->FindId(bf::chunk::EVNT, snd::HashId("SE/Shot"));
  SND_CHECK_EQ(p->U32(bf::chunk::EVNT, bf::col::EVNT_priority, ev), 200u);
  SND_CHECK_EQ(p->U32(bf::chunk::EVNT, bf::col::EVNT_sequenceKind, ev), 1u);
  const std::uint32_t speed = p->FindId(bf::chunk::PARM, snd::HashId("Speed"));
  SND_CHECK_EQ(CellF(*p, bf::chunk::PARM, bf::col::PARM_rampRate, speed), 2.5f);
  SND_CHECK_EQ(p->U32(bf::chunk::PARM, bf::col::PARM_scopeMask, speed), 3u);
  // MTRN: rows in written order, the music event's first row.
  const std::uint32_t bgm = p->FindId(bf::chunk::EVNT, snd::HashId("Music/BGM"));
  std::uint32_t first = bf::kNone;
  for (std::uint32_t r = 0; r < p->Rows(bf::chunk::MTRN); ++r)
    if (p->U32(bf::chunk::MTRN, bf::col::MTRN_musicEvent, r) == bgm) {
      first = r;
      break;
    }
  SND_REQUIRE(first != bf::kNone);
  SND_CHECK_EQ(CellF(*p, bf::chunk::MTRN, bf::col::MTRN_fadeOutBeats, first), 1.75f);
  SND_CHECK_EQ(p->U32(bf::chunk::MTRN, bf::col::MTRN_fromState, first), snd::HashId("Explore"));
  SND_CHECK_EQ(p->U32(bf::chunk::MTRN, bf::col::MTRN_toState, first + 1), 0u);  // wildcard
  const std::uint32_t sa = p->FindId(bf::chunk::MSEG, snd::HashId("Seg/A"));
  SND_REQUIRE(sa != bf::kNone);
  const std::uint32_t tf = p->U32(bf::chunk::MSEG, bf::col::MSEG_tempoFirst, sa);
  SND_CHECK_EQ(CellF(*p, bf::chunk::MTMP, bf::col::MTMP_bpm, tf), 133.25f);
}

SND_TEST(IndepS7Build, F18_PhysTableInTheBank) {
  ProjectDocument doc;
  SND_REQUIRE(LoadBase(doc));
  const DocBuild b = BuildDocument(doc);
  SND_REQUIRE(b.ok);
  const ParsedBank* p = Main(b);
  SND_REQUIRE(p != nullptr);
  std::uint64_t bytes = 0;
  const std::uint8_t* phys = p->Chunk(bf::chunk::PHYS, &bytes);
  SND_REQUIRE(phys != nullptr && bytes >= sizeof(bf::PhysChunkHeader));
  bf::PhysChunkHeader h;
  std::memcpy(&h, phys, sizeof h);
  SND_REQUIRE(h.tableCount == 1u);
  SND_REQUIRE(h.tablesOffset + sizeof(bf::PhysTable) <= bytes);
  bf::PhysTable t;
  std::memcpy(&t, phys + h.tablesOffset, sizeof t);
  SND_CHECK_EQ(t.id, snd::HashId("Phys/Default"));
  SND_REQUIRE(t.materialCount == 3u);
  const std::uint32_t N = 3;
  std::uint32_t v = 0;
  ReadU32(phys, t.pairOffset + 4u * (0 * N + 2), &v);  // metal x glass
  SND_CHECK_EQ(v, snd::HashId("SE/Sting"));
  ReadU32(phys, t.pairOffset + 4u * (0 * N + 1), &v);
  SND_CHECK_EQ(v, 0u);
  ReadU32(phys, t.singleOffset + 4u * (0 * 4 + 0), &v);  // metal impact
  SND_CHECK_EQ(v, snd::HashId("SE/Shot"));
  ReadU32(phys, t.singleOffset + 4u * (1 * 4 + 0), &v);  // wood impact
  SND_CHECK_EQ(v, snd::HashId("SE/Hit"));
  ReadU32(phys, t.footstepOffset + 4u * 1, &v);
  SND_CHECK_EQ(v, snd::HashId("SE/Hit"));
  SND_CHECK_EQ(t.defaultEvent[0], snd::HashId("SE/Engine"));
  // Material edits follow into the bank (the editor's numbering = the bank's).
  MaterialEditor m(doc, kPhys);
  SND_REQUIRE(m.MoveMaterial(2, 0));  // glass, metal, wood
  SND_REQUIRE(m.SetPair("wood", "glass", kHit));
  const DocBuild b2 = BuildDocument(doc);
  SND_REQUIRE(b2.ok);
  phys = Main(b2)->Chunk(bf::chunk::PHYS, &bytes);
  std::memcpy(&h, phys, sizeof h);
  std::memcpy(&t, phys + h.tablesOffset, sizeof t);
  ReadU32(phys, t.pairOffset + 4u * (0 * N + 1), &v);  // glass(0) x metal(1)
  SND_CHECK_EQ(v, snd::HashId("SE/Sting"));
  ReadU32(phys, t.pairOffset + 4u * (0 * N + 2), &v);  // glass(0) x wood(2)
  SND_CHECK_EQ(v, snd::HashId("SE/Hit"));
  ReadU32(phys, t.pairOffset + 4u * (2 * N + 0), &v);  // the lower triangle is not used
  SND_CHECK_EQ(v, 0u);
  ReadU32(phys, t.singleOffset + 4u * (1 * 4 + 0), &v);  // metal is now 1
  SND_CHECK_EQ(v, snd::HashId("SE/Shot"));
  for (std::uint32_t a = 0; a < 3; ++a)
    for (std::uint32_t c = a; c < 3; ++c) {
      const MaterialResolution r = m.Resolve(a, c, ContactKind::Impact);
      ReadU32(phys, t.pairOffset + 4u * (a * N + c), &v);
      if (r.source == "pair") {
        SND_REQUIRE(r.events.size() == 1u);
        SND_CHECK_EQ(v, snd::HashId(doc.Find(r.events[0])->name()));
      } else {
        SND_CHECK_EQ(v, 0u);
      }
    }
}

SND_TEST(IndepS7Build, G6_PatchProgramLinkedToSynthRow) {
  ProjectDocument doc;
  SND_REQUIRE(LoadBase(doc));
  const DocBuild b = BuildDocument(doc);
  SND_REQUIRE(b.ok);
  const ParsedBank* p = Main(b);
  std::uint64_t bytes = 0;
  const std::uint8_t* x = p->Chunk(snd::buildtool::patch::kChunkFourcc, &bytes);
  SND_REQUIRE(x != nullptr);
  std::vector<snd::buildtool::patch::Program> progs;
  std::vector<snd::buildtool::patch::Link> links;
  SND_REQUIRE(snd::buildtool::patch::ReadPatchChunk(x, bytes, &progs, &links));
  SND_REQUIRE(progs.size() == 1u && links.size() == 1u);
  SND_CHECK_EQ(progs[0].id, snd::HashId("Patch/Laser"));
  SND_REQUIRE(links[0].synpRow < p->Rows(bf::chunk::SYNP));
  SND_CHECK_EQ(p->U32(bf::chunk::SYNP, bf::col::SYNP_kind, links[0].synpRow), 3u);
  const auto offset = p->U32(bf::chunk::SYNP, bf::col::SYNP_programOffset, links[0].synpRow);
  const auto size = p->U32(bf::chunk::SYNP, bf::col::SYNP_programBytes, links[0].synpRow);
  const auto* runtime = p->Chunk(bf::chunk::PTCH, &bytes);
  SND_REQUIRE(runtime && offset <= bytes && size <= bytes - offset);
  SND_CHECK(bf::patch::ValidatePatchProgram(runtime + offset, size));
  // The clip of SE/Laser points at that SYNP row.
  const std::uint32_t c = ClipRow(*p, "SE/Laser", 0, 0);
  SND_CHECK_EQ(p->U32(bf::chunk::CLIP, bf::col::CLIP_kind, c), 2u);
  SND_CHECK_EQ(p->U32(bf::chunk::CLIP, bf::col::CLIP_target, c), links[0].synpRow);
  // The program equals what the editor compiles.
  PatchEditor pe(doc, kPatch);
  snd::buildtool::patch::Program mine;
  SND_REQUIRE(pe.Compiled(&mine));
  SND_REQUIRE(mine.nodes.size() == progs[0].nodes.size());
  for (std::size_t i = 0; i < mine.nodes.size(); ++i) {
    SND_CHECK_EQ(mine.nodes[i].kind, progs[0].nodes[i].kind);
    SND_CHECK(std::memcmp(mine.nodes[i].inputs, progs[0].nodes[i].inputs, sizeof mine.nodes[i].inputs) == 0);
    SND_CHECK(std::memcmp(mine.nodes[i].params, progs[0].nodes[i].params, sizeof mine.nodes[i].params) == 0);
  }
  // A position-only edit changes nothing in the bank (E7: None).
  ProjectDocument before = doc;
  SND_REQUIRE(pe.MoveNode("osc", 400, 400));
  SND_CHECK(ClassifyEdit(before, doc) == EditClass::None);
}

// --- E6: LocateField against the bank -------------------------------------------------------------------------

SND_TEST(IndepS7Build, E6_LocateFieldMatchesTheBank) {
  ProjectDocument doc;
  SND_REQUIRE(LoadBase(doc));
  const DocBuild b = BuildDocument(doc);
  SND_REQUIRE(b.ok);
  // The table's columns exist in the contract.
  for (const auto& fc : FieldColumns()) {
    if (fc.chunk == snd::buildtool::patch::kChunkFourcc || fc.chunk == bf::chunk::PHYS) continue;
    if (!ColumnKnown(fc.chunk, fc.column)) {
      snd::test::Note("FieldColumns: %s %s -> unknown column %u", fc.kind, fc.pattern, fc.column);
      SND_CHECK(false);
    }
  }
  struct Case {
    const char* guid;
    const char* path;
    std::uint32_t chunk;
    std::uint16_t column;
    double value;
    const char* owner;  // runtime id name of the owning object, "" = not checked
  };
  const Case cases[] = {
      {kSfx, "volumeDb", bf::chunk::BUSS, bf::col::BUSS_volumeDb, -3, "SFX"},
      {kSfx, "sends[0].levelDb", bf::chunk::SEND, bf::col::SEND_levelDb, -12, "SFX"},
      {kSfx, "effects[0].params[0]", bf::chunk::EFPV, bf::col::EFPV_value, 8000, "SFX"},
      {kMusicBus, "volumeDb", bf::chunk::BUSS, bf::col::BUSS_volumeDb, -6, "Music"},
      {kCatSfx, "volumeDb", bf::chunk::CATG, bf::col::CATG_volumeDb, -2.5, "CatSFX"},
      {kSpeed, "rampRate", bf::chunk::PARM, bf::col::PARM_rampRate, 5, "Speed"},
      {kSpeed, "max", bf::chunk::PARM, bf::col::PARM_maxValue, 10, "Speed"},
      {kShot, "retriggerMs", bf::chunk::EVNT, bf::col::EVNT_retriggerMs, 50, "SE/Shot"},
      {kShot, "fadeOutMs", bf::chunk::EVNT, bf::col::EVNT_fadeOutMs, 30, "SE/Shot"},
      {kShot, "tracks[0].clips[0].volumeDb", bf::chunk::CLIP, bf::col::CLIP_volumeDb, -1, "SE/Shot"},
      {kShot, "tracks[0].clips[1].startMs", bf::chunk::CLIP, bf::col::CLIP_startMs, 100, "SE/Shot"},
      {kShot, "tracks[0].clips[0].random.pitchCents", bf::chunk::CLIP, bf::col::CLIP_randPitchCents, 50, "SE/Shot"},
      {kShot, "tracks[0].actions[0].value", bf::chunk::ACTN, bf::col::ACTN_value, 3, "SE/Shot"},
      {kEngine, "maxDistance", bf::chunk::EVNT, bf::col::EVNT_maxDistance, 50, "SE/Engine"},
      {kEngine, "cone.outerDb", bf::chunk::EVNT, bf::col::EVNT_coneOuterDb, -12, "SE/Engine"},
      {kDuck, "amountDb", bf::chunk::DUCK, bf::col::DUCK_amountDb, -6, ""},
      {kSnap, "values[0].value", bf::chunk::SNPV, bf::col::SNPV_value, -6, "Quiet"},
      {kGroup, "maxVoices", bf::chunk::VLGR, bf::col::VLGR_maxVoices, 8, "Steps"},
  };
  for (const auto& c : cases) {
    const auto cells = LocateField(b, doc, c.guid, c.path);
    if (cells.empty()) {
      snd::test::Note("LocateField(%s) found no cell", c.path);
      SND_CHECK(false);
      continue;
    }
    for (const auto& cell : cells) {
      SND_CHECK_EQ(cell.chunk, c.chunk);
      SND_CHECK_EQ(cell.column, static_cast<std::uint32_t>(c.column));
      const BankImage* img = b.FindBank(cell.bank, cell.language);
      SND_REQUIRE(img != nullptr && img->view() != nullptr);
      std::uint64_t bits = 0;
      SND_CHECK(img->view()->Cell(cell.chunk, cell.column, cell.row, &bits));
      SND_CHECK_EQ(bits, cell.bits);
      const double got = cell.type == CellType::F32 ? static_cast<double>(BitsToFloat(bits)) : static_cast<double>(bits);
      if (got != static_cast<double>(static_cast<float>(c.value))) {
        snd::test::Note("LocateField(%s %s): bank value %g, document %g", c.guid, c.path, got, c.value);
        SND_CHECK(false);
      }
      if (cell.addressable && c.owner[0] != '\0') SND_CHECK_EQ(cell.runtimeId, snd::HashId(c.owner));
    }
  }
  // Unknown path / object: nothing.
  SND_CHECK(LocateField(b, doc, kSfx, "nope").empty());
  SND_CHECK(LocateField(b, doc, "nope", "volumeDb").empty());
  SND_CHECK(LocateField(b, doc, kSfx, "sends[9].levelDb").empty());
}

// --- E7, E8: classification ------------------------------------------------------------------------------------

SND_TEST(IndepS7Build, E7_ValueEditsAndStructure) {
  ProjectDocument base;
  SND_REQUIRE(LoadBase(base));
  const DocBuild b0 = BuildDocument(base);
  SND_REQUIRE(b0.ok);
  // E8.
  const LiveDelta self = DiffBuilds(b0, b0);
  SND_CHECK(self.kind == EditClass::None);
  SND_CHECK(self.values.empty() && self.chunks.empty() && self.banksAdded.empty() && self.banksRemoved.empty());
  // A bus volume: one live value.
  {
    ProjectDocument d = base;
    SND_REQUIRE(d.SetNumber(kSfx, "volumeDb", -8));
    LiveDelta delta;
    SND_CHECK(ClassifyEdit(base, d, &delta) == EditClass::Value);
    SND_REQUIRE(delta.values.size() == 1u);
    const ValueEdit& v = delta.values[0];
    SND_CHECK_EQ(v.cell.chunk, bf::chunk::BUSS);
    SND_CHECK_EQ(v.cell.column, static_cast<std::uint32_t>(bf::col::BUSS_volumeDb));
    SND_CHECK(v.cell.addressable && v.cell.liveWritable);
    SND_CHECK_EQ(v.cell.runtimeId, snd::HashId("SFX"));
    SND_CHECK_EQ(BitsToFloat(v.cell.bits), -8.0f);
    SND_CHECK_EQ(BitsToFloat(v.oldBits), -3.0f);
    SND_CHECK(delta.chunks.empty());
  }
  // A clip's volume: one live value on the event (ACTN / CLIP owned by EVNT).
  {
    ProjectDocument d = base;
    EventEditor e(d, kShot);
    SND_REQUIRE(e.SetClipNumber(0, 1, ClipNumber::VolumeDb, -2));
    LiveDelta delta;
    SND_CHECK(ClassifyEdit(base, d, &delta) == EditClass::Value);
    SND_REQUIRE(delta.values.size() == 1u);
    SND_CHECK_EQ(delta.values[0].cell.chunk, bf::chunk::CLIP);
    SND_CHECK_EQ(delta.values[0].cell.runtimeId, snd::HashId("SE/Shot"));
  }
  // Notes only: nothing reaches the bank.
  {
    ProjectDocument d = base;
    SND_REQUIRE(d.Set(kShot, "notes", String("x")));
    SND_CHECK(ClassifyEdit(base, d) == EditClass::None);
  }
  // Structure: a clip added, a bus added, an event renamed, a material added.
  {
    ProjectDocument d = base;
    EventEditor e(d, kShot);
    SND_REQUIRE(e.AddAssetClip(0, kTone, 0));
    LiveDelta delta;
    SND_CHECK(ClassifyEdit(base, d, &delta) == EditClass::Structure);
    bool clip = false;
    for (const auto& c : delta.chunks) clip |= c.fourcc == bf::chunk::CLIP;
    SND_CHECK(clip);
  }
  {
    ProjectDocument d = base;
    MixerEditor m(d);
    const std::string nb = m.AddBus("New", kMaster);
    SND_REQUIRE(!nb.empty());
    // A bus nobody uses is in no bank (project-format.md §3): nothing changes.
    SND_CHECK(ClassifyEdit(base, d) == EditClass::None);
    EventEditor e(d, kHit);
    SND_REQUIRE(e.SetBus(nb));
    SND_CHECK(ClassifyEdit(base, d) == EditClass::Structure);
  }
  {
    ProjectDocument d = base;
    SND_REQUIRE(d.Rename(kHit, "SE/Hit2"));
    SND_CHECK(ClassifyEdit(base, d) == EditClass::Structure);
  }
  {
    ProjectDocument d = base;
    MaterialEditor m(d, kPhys);
    SND_REQUIRE(m.AddMaterial("stone", 3));
    LiveDelta delta;
    SND_CHECK(ClassifyEdit(base, d, &delta) == EditClass::Structure);
    bool phys = false;
    for (const auto& c : delta.chunks) phys |= c.fourcc == bf::chunk::PHYS;
    SND_CHECK(phys);
  }
  // Priority is not live-writable (decision 7): structure.
  {
    ProjectDocument d = base;
    SND_REQUIRE(d.SetNumber(kShot, "priority", 7));
    SND_CHECK(ClassifyEdit(base, d) == EditClass::Structure);
  }
  // A new bank: a bank added.
  {
    ProjectDocument d = base;
    SND_REQUIRE(!d.Create("bank", "Extra", nullptr).empty());
    json::Value init = json::MakeObject();
    json::Value tracks = json::MakeArray();
    json::Value tr = json::MakeObject();
    json::Value clips = json::MakeArray();
    json::Value clip = json::MakeObject();
    json::Add(clip, "asset", String(kTone));
    clips.items.push_back(clip);
    json::Add(tr, "clips", clips);
    tracks.items.push_back(tr);
    json::Add(init, "bank", String("Extra"));
    json::Add(init, "tracks", tracks);
    SND_REQUIRE(!d.Create("event", "SE/Extra", &init).empty());
    LiveDelta delta;
    SND_CHECK(ClassifyEdit(base, d, &delta) == EditClass::Structure);
    SND_CHECK_EQ(delta.banksAdded.size(), 1u);
  }
}

SND_TEST(IndepS7Build, E7_ValueEditsMatchLocateField) {
  // For each live-writable field: the ValueEdit of a value edit addresses the same cell as LocateField.
  ProjectDocument base;
  SND_REQUIRE(LoadBase(base));
  const DocBuild b0 = BuildDocument(base);
  const char* const fields[][2] = {{kSfx, "volumeDb"},
                                   {kSfx, "sends[0].levelDb"},
                                   {kSfx, "effects[0].params[0]"},
                                   {kCatSfx, "volumeDb"},
                                   {kShot, "tracks[0].clips[0].pitchCents"},
                                   {kShot, "fadeOutMs"},
                                   {kSpeed, "rampRate"},
                                   {kDuck, "amountDb"},
                                   {kSnap, "values[0].value"}};
  for (const auto& f : fields) {
    const auto cells = LocateField(b0, base, f[0], f[1]);
    SND_REQUIRE(!cells.empty());
    ProjectDocument d = base;
    const double old = NumberAt(d.Find(f[0])->doc, f[1], 0);
    SND_REQUIRE(d.SetNumber(f[0], f[1], old == 7000 ? 6000 : (old > 100 ? old - 1000 : old - 1.5)));
    LiveDelta delta;
    const EditClass k = ClassifyEdit(base, d, &delta);
    const BankImage* img0 = b0.FindBank(cells[0].bank, cells[0].language);
    const bool present = img0 != nullptr && img0->view()->HasColumn(cells[0].chunk, cells[0].column);
    // (A column absent from the bank: Defect_LocateFieldLiveWritableForAbsentColumn.)
    if (cells[0].liveWritable && cells[0].addressable && present) {
      if (k != EditClass::Value) {
        snd::test::Note("%s %s: LocateField says live, ClassifyEdit says %d (%u chunks)", f[0], f[1], static_cast<int>(k),
                        static_cast<unsigned>(delta.chunks.size()));
        for (const auto& c : delta.chunks) snd::test::Note("  chunk %08x", c.fourcc);
      }
      SND_CHECK(k == EditClass::Value);
      SND_REQUIRE(delta.values.size() == 1u);
      const CellRef& c = delta.values[0].cell;
      SND_CHECK(c.chunk == cells[0].chunk && c.column == cells[0].column && c.row == cells[0].row);
      SND_CHECK(c.runtimeId == cells[0].runtimeId && c.index == cells[0].index);
    } else {
      snd::test::Note("%s: not live or column absent (%s)", f[1], k == EditClass::Structure ? "structure" : "other");
      SND_CHECK(k == EditClass::Structure);
    }
  }
}

// --- after reading the implementation: the editor's warnings are the build's ----------------------

SND_TEST(IndepS7Build, F13_UnreachableRowsAreTheBuildWarnings) {
  Rng r(99);
  const char* const names[] = {"Explore", "Battle", "*"};
  for (int iter = 0; iter < 25; ++iter) {
    ProjectDocument doc;
    SND_REQUIRE(LoadBase(doc));
    MusicEditor me(doc, kBgm);
    while (!me.Rules().empty()) SND_REQUIRE(me.RemoveRule(0));
    const std::uint32_t rows = 1 + r.Below(6);
    for (std::uint32_t i = 0; i < rows; ++i) {
      TransitionRule rule;
      rule.from = names[r.Below(3)];
      rule.to = names[r.Below(3)];
      rule.timing = static_cast<Timing>(r.Below(3));
      SND_REQUIRE(me.AddRule(rule, i));
    }
    std::vector<bool> editor(rows, false);
    for (const auto& u : me.Unreachable()) editor[u.first] = true;
    std::vector<bool> build(rows, false);
    for (const auto& is : IssuesOf(CheckDocument(doc), kBgm)) {
      if (is.error) {
        snd::test::Note("unexpected error: %s", is.text.c_str());
        SND_CHECK(false);
        continue;
      }
      for (std::uint32_t k = 0; k < rows; ++k)
        if (is.path.rfind("music.transitions[" + std::to_string(k) + "]", 0) == 0) build[k] = true;
    }
    for (std::uint32_t k = 0; k < rows; ++k) {
      if (editor[k] != build[k]) {
        snd::test::Note("table %d row %u: editor unreachable = %d, build warning = %d", iter, k, editor[k] ? 1 : 0,
                        build[k] ? 1 : 0);
        SND_CHECK(false);
      }
    }
  }
}

SND_TEST(IndepS7Build, E2_NestedCycleMadeByRawEditsIsAnError) {
  ProjectDocument doc;
  SND_REQUIRE(LoadBase(doc));
  // SE/Hit nests SE/Shot and SE/Shot nests SE/Hit (only possible with raw edits).
  json::Value c = json::MakeObject();
  json::Add(c, "event", String(kShot));
  SND_REQUIRE(doc.Insert(kHit, "tracks[0].clips", 1, c));
  json::Value d = json::MakeObject();
  json::Add(d, "event", String(kHit));
  SND_REQUIRE(doc.Insert(kShot, "tracks[0].clips", 2, d));
  const auto issues = CheckDocument(doc);
  SND_CHECK(Errors(issues) > 0u);
  SND_CHECK(Errors(IssuesOf(issues, kHit)) + Errors(IssuesOf(issues, kShot)) > 0u);
  SND_CHECK(!BuildDocument(doc).ok);
  // The editor refuses the same cycle.
  SND_REQUIRE(doc.Undo());
  EventEditor e(doc, kShot);
  SND_CHECK(!e.AddEventClip(0, kHit, 0));
}
