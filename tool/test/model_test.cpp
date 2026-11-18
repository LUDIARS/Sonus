// The tool's models without a game: project reading, mixer rules (fader range,
// mute, solo over the bus tree, effect parameters), undo / redo, overrides and
// revert, the value sync while offline, recording files, peak hold.
#include <cmath>
#include <cstring>

#include "snd/format/bank_format.hpp"
#include "snd/test/test.hpp"
#include "snd/tool/cell_sync.hpp"
#include "snd/tool/meter_ballistics.hpp"
#include "snd/tool/mixer.hpp"
#include "snd/tool/names.hpp"
#include "snd/tool/recording_file.hpp"
#include "tool_fixture.hpp"

using namespace snd::tool;
using tooltest::Bus;
using tooltest::Cat;
namespace bf = snd::bank_format;

namespace {

// A deeper bus tree for the solo rule: Master <- A <- A1, Master <- B.
void WriteTree(const tooltest::ProjectDir& p) {
  using tooltest::G;
  p.Write("buses/Master.json", "{\"type\": \"bus\", \"guid\": \"" + G(1) + "\", \"name\": \"Master\", \"master\": true}");
  p.Write("buses/A.json", "{\"type\": \"bus\", \"guid\": \"" + G(2) + "\", \"name\": \"A\", \"parent\": \"Master\", \"volumeDb\": -3}");
  p.Write("buses/A1.json", "{\"type\": \"bus\", \"guid\": \"" + G(3) + "\", \"name\": \"A1\", \"parent\": \"" + G(2) + "\"}");
  p.Write("buses/B.json", "{\"type\": \"bus\", \"guid\": \"" + G(4) + "\", \"name\": \"B\", \"parent\": \"Master\", "
                          "\"effects\": [{\"type\": \"snd.fx.lowpass\", \"params\": [1000]}, {\"type\": \"my.plugin\", \"params\": [3, 7]},"
                          " {\"type\": \"snd.fx.limiter\", \"params\": [-6, 50, 2, 0]}]}");
  p.Write("categories/Top.json", "{\"type\": \"category\", \"guid\": \"" + G(5) + "\", \"name\": \"Top\", \"volumeDb\": -2}");
  p.Write("categories/Sub.json", "{\"type\": \"category\", \"guid\": \"" + G(6) + "\", \"name\": \"Sub\", \"parent\": \"Top\"}");
  p.Write("parameters/Speed.json", "{\"type\": \"parameter\", \"guid\": \"" + G(7) + "\", \"name\": \"Speed\", \"min\": 0, \"max\": 10, \"default\": 1}");
  p.Write("parameters/Local.json", "{\"type\": \"parameter\", \"guid\": \"" + G(8) + "\", \"name\": \"Local\", \"scope\": [\"emitter\"]}");
}

}  // namespace

SND_TEST(ToolProject, GuidParsing) {
  Guid g;
  SND_CHECK(ParseGuid("01234567-89ab-cdef-0123-456789abcdef", &g));
  SND_CHECK_EQ(g.hi, 0x0123456789abcdefull);
  SND_CHECK_EQ(g.lo, 0x0123456789abcdefull);
  SND_CHECK(ParseGuid("{00000000-0000-0000-0000-000000000001}", &g));
  SND_CHECK_EQ(g.hi, 0ull);
  SND_CHECK_EQ(g.lo, 1ull);
  SND_CHECK(!ParseGuid("00000000-0000-0000-0000-00000000000", &g));
  SND_CHECK(!ParseGuid("0000000000000000000000000000000000000", &g));
  SND_CHECK(!ParseGuid("g0000000-0000-0000-0000-000000000000", &g));
}

SND_TEST(ToolProject, ReadsTheBusTreeCategoriesParametersAndEffects) {
  tooltest::ProjectDir p("tree");
  WriteTree(p);
  Project pr;
  SND_REQUIRE(pr.Load(p.dir().string()));
  SND_REQUIRE_EQ(pr.buses().size(), 4u);
  // Tree order: master, then depth first by name.
  SND_CHECK_EQ(pr.buses()[0].name, std::string("Master"));
  SND_CHECK_EQ(pr.buses()[1].name, std::string("A"));
  SND_CHECK_EQ(pr.buses()[2].name, std::string("A1"));
  SND_CHECK_EQ(pr.buses()[3].name, std::string("B"));
  SND_CHECK(pr.buses()[0].master);
  SND_CHECK_EQ(pr.buses()[0].parent, -1);
  SND_CHECK_EQ(pr.buses()[2].parent, 1);
  SND_CHECK_EQ(pr.buses()[2].depth, 2u);
  SND_CHECK_EQ(pr.buses()[1].volumeDb, -3.0f);
  SND_CHECK_EQ(pr.buses()[1].id, snd::HashId("A"));
  SND_CHECK_EQ(pr.buses()[1].guid.lo, 2ull | (0x8000ull << 48));
  SND_CHECK(pr.IsBusUnder(2, 0) && pr.IsBusUnder(2, 1) && !pr.IsBusUnder(1, 2) && !pr.IsBusUnder(3, 1));
  // Effects: the dsp table names the parameters and fills the missing ones with defaults.
  const BusDesc& b = pr.buses()[3];
  SND_REQUIRE_EQ(b.effects.size(), 3u);
  SND_CHECK(b.effects[0].known);
  SND_REQUIRE_EQ(b.effects[0].params.size(), 2u);
  SND_CHECK_EQ(b.effects[0].params[0].name, std::string("CutoffHz"));
  SND_CHECK_EQ(b.effects[0].params[0].projectValue, 1000.0f);
  SND_CHECK(b.effects[0].params[0].logScale);
  SND_CHECK_EQ(b.effects[0].params[1].projectValue, 1.0f);  // dsp default (Resonance)
  SND_CHECK(!b.effects[1].known);
  SND_CHECK_EQ(b.effects[1].params.size(), 2u);
  SND_CHECK_EQ(b.effects[1].params[1].projectValue, 7.0f);
  SND_CHECK(b.effects[2].params[2].initOnly);  // LookaheadMs
  // Categories (tree order) and parameters (name order).
  SND_REQUIRE_EQ(pr.categories().size(), 2u);
  SND_CHECK_EQ(pr.categories()[0].name, std::string("Top"));
  SND_CHECK_EQ(pr.categories()[1].parent, 0);
  SND_REQUIRE_EQ(pr.parameters().size(), 2u);
  SND_CHECK_EQ(pr.parameters()[0].name, std::string("Local"));
  SND_CHECK(!pr.parameters()[0].global);
  SND_CHECK(pr.parameters()[1].global);
  SND_CHECK_EQ(pr.parameters()[1].defaultValue, 1.0f);
}

SND_TEST(ToolProject, ReportsErrorsAndKeepsWhatItCouldRead) {
  tooltest::ProjectDir p("bad");
  WriteTree(p);
  p.Write("buses/Orphan.json", "{\"type\": \"bus\", \"guid\": \"" + tooltest::G(20) + "\", \"name\": \"Orphan\", \"parent\": \"Nope\"}");
  p.Write("broken.json", "{\"type\": ");
  Project pr;
  SND_CHECK(!pr.Load(p.dir().string()));
  SND_CHECK(pr.errorCount() >= 2u);
  SND_CHECK(pr.buses().size() >= 4u);
  Project none;
  SND_CHECK(!none.Load((p.root() / "missing").string()));
}

SND_TEST(ToolProject, BuildHashIsTheBuildToolsAndChangesWithTheProject) {
  tooltest::ToolProject a, a2;
  SND_REQUIRE(a.ok && a2.ok);
  SND_CHECK(a.project.buildHash() != 0u);
  SND_CHECK_EQ(a.project.buildHash(), a2.project.buildHash());
  tooltest::MixVariant v;
  v.sfxDb = -3.0f;
  tooltest::ToolProject b(v);
  SND_REQUIRE(b.ok);
  SND_CHECK(b.project.buildHash() != a.project.buildHash());
  SND_REQUIRE_EQ(a.project.banks().size(), 1u);
  SND_CHECK_EQ(a.project.banks()[0].bankId, snd::HashId("Main"));
}

SND_TEST(ToolMixer, FaderMuteAndSoloRules) {
  tooltest::ProjectDir p("rules");
  WriteTree(p);
  Project pr;
  SND_REQUIRE(pr.Load(p.dir().string()));
  MixerModel m;
  m.Bind(pr);
  const std::int32_t A = Bus(pr, "A"), A1 = Bus(pr, "A1"), B = Bus(pr, "B"), M = Bus(pr, "Master");
  // Range: clamped to [-inf, +12].
  SND_CHECK(m.Set(Target{Control::BusFader, A, 0, 0}, 40.0f));
  SND_CHECK_EQ(m.Get(Target{Control::BusFader, A, 0, 0}), kFaderMaxDb);
  SND_CHECK(m.Set(Target{Control::BusFader, A, 0, 0}, -1000.0f));
  SND_CHECK_EQ(m.EffectiveBusDb(A), kFaderOffDb);
  SND_CHECK(!m.Set(Target{Control::BusFader, A, 0, 0}, -500.0f));  // no change: no edit
  SND_CHECK(m.Set(Target{Control::BusFader, A, 0, 0}, -6.0f));
  // Mute sends -inf, keeps the fader.
  SND_CHECK(m.Set(Target{Control::BusMute, A, 0, 0}, 1.0f));
  SND_CHECK_EQ(m.EffectiveBusDb(A), kFaderOffDb);
  SND_CHECK_EQ(m.Get(Target{Control::BusFader, A, 0, 0}), -6.0f);
  SND_CHECK(m.Set(Target{Control::BusMute, A, 0, 0}, 0.0f));
  // Solo A1: its ancestors (A, Master) carry it, B is muted.
  SND_CHECK(m.Set(Target{Control::BusSolo, A1, 0, 0}, 1.0f));
  SND_CHECK(!m.SoloMuted(A1) && !m.SoloMuted(A) && !m.SoloMuted(M));
  SND_CHECK(m.SoloMuted(B));
  SND_CHECK_EQ(m.EffectiveBusDb(B), kFaderOffDb);
  SND_CHECK_EQ(m.EffectiveBusDb(A), -6.0f);
  // Solo A: A1 is below it (carried), B still muted.
  SND_CHECK(m.Set(Target{Control::BusSolo, A1, 0, 0}, 0.0f));
  SND_CHECK(m.Set(Target{Control::BusSolo, A, 0, 0}, 1.0f));
  SND_CHECK(!m.SoloMuted(A1) && m.SoloMuted(B));
  // Solo B as well: everyone audible.
  SND_CHECK(m.Set(Target{Control::BusSolo, B, 0, 0}, 1.0f));
  SND_CHECK(!m.SoloMuted(A1) && !m.SoloMuted(B) && !m.SoloMuted(A));
  // Effect parameters: clamped into the dsp range; init-only and plugin ranges.
  const Target cutoff{Control::EffectParam, B, 0, 0};
  SND_CHECK(m.Set(cutoff, 1.0e9f));
  SND_CHECK_EQ(m.Get(cutoff), 22000.0f);
  SND_CHECK(!m.IsEditable(Target{Control::EffectParam, B, 2, 2}));  // limiter LookaheadMs (kInit)
  SND_CHECK(!m.Set(Target{Control::EffectParam, B, 2, 2}, 5.0f));
  SND_CHECK(!m.IsEditable(Target{Control::EffectParam, B, 3, 0}));
  // Category and parameter targets.
  SND_CHECK(m.Set(Target{Control::CategoryMute, 0, 0, 0}, 1.0f));
  SND_CHECK_EQ(m.EffectiveCategoryDb(0), kFaderOffDb);
  SND_CHECK(!m.IsEditable(Target{Control::ParamOverride, 0, 0, 0}));  // "Local" is not global
  SND_CHECK(m.Set(Target{Control::ParamOverride, 1, 0, 0}, 50.0f));
  SND_CHECK_EQ(m.Get(Target{Control::ParamOverride, 1, 0, 0}), 10.0f);
}

SND_TEST(ToolMixer, CellsAddressTheBankColumns) {
  tooltest::ProjectDir p("cells");
  WriteTree(p);
  Project pr;
  SND_REQUIRE(pr.Load(p.dir().string()));
  MixerModel m;
  m.Bind(pr);
  const std::vector<Cell> cells = m.Cells();
  // 4 bus volumes + lowpass 2 + plugin 2 + limiter 3 (LookaheadMs is init-only) + 2 categories.
  SND_CHECK_EQ(cells.size(), 4u + 2u + 2u + 3u + 2u);
  SND_CHECK_EQ(cells[1].address.chunk, bf::chunk::BUSS);
  SND_CHECK_EQ(cells[1].address.column, static_cast<std::uint32_t>(bf::col::BUSS_volumeDb));
  SND_CHECK_EQ(cells[1].address.runtimeId, snd::HashId("A"));
  SND_CHECK_EQ(cells[1].bankValue, -3.0f);
  bool sawLimiterRelease = false;
  for (const Cell& c : cells) {
    if (c.address.chunk == bf::chunk::EFPV && c.address.index == ((2u << 16) | 1u)) {
      sawLimiterRelease = true;
      SND_CHECK_EQ(c.address.runtimeId, snd::HashId("B"));
      SND_CHECK_EQ(c.value, 50.0f);
    }
    SND_CHECK(!(c.address.chunk == bf::chunk::EFPV && c.address.index == ((2u << 16) | 2u)));
  }
  SND_CHECK(sawLimiterRelease);
  SND_CHECK_EQ(cells.back().address.chunk, bf::chunk::CATG);
  SND_CHECK_EQ(m.ParamCells().size(), 1u);  // only the global parameter
}

SND_TEST(ToolMixer, UndoRedoGroupsAndMerges) {
  tooltest::ProjectDir p("undo");
  WriteTree(p);
  Project pr;
  SND_REQUIRE(pr.Load(p.dir().string()));
  MixerModel m;
  m.Bind(pr);
  const Target fa{Control::BusFader, Bus(pr, "A"), 0, 0};
  // A drag: many values, one edit.
  m.BeginGroup();
  for (int i = 1; i <= 10; ++i) m.Set(fa, -3.0f - static_cast<float>(i));
  m.EndGroup();
  SND_CHECK_EQ(m.history().UndoCount(), 1u);
  SND_CHECK_EQ(m.Get(fa), -13.0f);
  SND_CHECK(m.Undo());
  SND_CHECK_EQ(m.Get(fa), -3.0f);
  SND_CHECK(m.Redo());
  SND_CHECK_EQ(m.Get(fa), -13.0f);
  // A group that ends where it started is no edit.
  m.BeginGroup();
  m.Set(fa, 0.0f);
  m.Set(fa, -13.0f);
  m.EndGroup();
  SND_CHECK_EQ(m.history().UndoCount(), 1u);
  // Merge keys: consecutive changes with the same key and target merge.
  m.Set(fa, -12.0f, 77);
  m.Set(fa, -11.0f, 77);
  m.Set(fa, -10.0f, 77);
  SND_CHECK_EQ(m.history().UndoCount(), 2u);
  m.Set(Target{Control::BusMute, fa.a, 0, 0}, 1.0f);
  m.Set(fa, -9.0f, 77);  // after another edit: a new step
  SND_CHECK_EQ(m.history().UndoCount(), 4u);
  SND_CHECK(m.Undo() && m.Undo() && m.Undo());
  SND_CHECK_EQ(m.Get(fa), -13.0f);
  SND_CHECK_EQ(m.Get(Target{Control::BusMute, fa.a, 0, 0}), 0.0f);
  // A new edit clears the redo list.
  m.Set(fa, -1.0f);
  SND_CHECK(!m.history().CanRedo());
}

SND_TEST(ToolMixer, OverridesListAndRevert) {
  tooltest::ProjectDir p("ovr");
  WriteTree(p);
  Project pr;
  SND_REQUIRE(pr.Load(p.dir().string()));
  MixerModel m;
  m.Bind(pr);
  SND_CHECK(m.Overrides().empty());
  const std::int32_t A = Bus(pr, "A"), B = Bus(pr, "B");
  m.Set(Target{Control::BusFader, A, 0, 0}, -10.0f);
  m.Set(Target{Control::EffectParam, B, 0, 0}, 2000.0f);
  m.Set(Target{Control::BusSolo, A, 0, 0}, 1.0f);  // B solo-muted
  m.Set(Target{Control::ParamOverrideOn, 1, 0, 0}, 1.0f);
  std::vector<OverrideItem> items = m.Overrides();
  SND_REQUIRE_EQ(items.size(), 4u);  // A volume, B volume (muted by solo), B cutoff, parameter
  SND_CHECK_EQ(items[0].label, std::string("A volume"));
  SND_CHECK_EQ(items[0].value, -10.0f);
  SND_CHECK_EQ(items[0].bankValue, -3.0f);
  SND_CHECK(items[1].muted);
  SND_CHECK_EQ(items[2].key, "fx." + Hex32(snd::HashId("B")) + ".0.0");
  // Revert B's volume: clears the solo that silences it (one undo step).
  const std::size_t before = m.history().UndoCount();
  SND_CHECK(m.Revert(items[1]));
  SND_CHECK_EQ(m.history().UndoCount(), before + 1u);
  SND_CHECK(!m.AnySolo());
  SND_CHECK_EQ(m.Overrides().size(), 3u);
  SND_CHECK(m.Undo());
  SND_CHECK(m.AnySolo());
  // Revert all: back to the bank, one step.
  SND_CHECK(m.RevertAll());
  SND_CHECK(m.Overrides().empty());
  SND_CHECK(m.Undo());
  SND_CHECK_EQ(m.Overrides().size(), 4u);
}

SND_TEST(ToolSync, OfflineEditsWaitAsUnsent) {
  tooltest::ProjectDir p("sync");
  WriteTree(p);
  Project pr;
  SND_REQUIRE(pr.Load(p.dir().string()));
  MixerModel m;
  m.Bind(pr);
  CellSync s;
  s.Update(nullptr, m.Cells(), m.ParamCells(), false, false);
  SND_CHECK_EQ(s.Count(CellState::Synced), static_cast<std::uint32_t>(m.Cells().size() + m.ParamCells().size()));
  m.Set(Target{Control::BusFader, Bus(pr, "A"), 0, 0}, -9.0f);
  m.Set(Target{Control::ParamOverrideOn, 1, 0, 0}, 1.0f);
  s.Update(nullptr, m.Cells(), m.ParamCells(), false, false);
  SND_CHECK_EQ(s.Count(CellState::Unsent), 2u);
  SND_CHECK_EQ(s.stats().sent, 0u);
}

SND_TEST(ToolRecordingFile, EncodeDecodeAndDamage) {
  Recording r;
  r.info.projectBuildHash = 0x1122334455667788ull;
  r.info.runtimeBuildHash = 0x99;
  r.info.sampleRate = 48000;
  r.info.blockSize = 256;
  r.info.platform = 7;
  r.info.contractVersion = 0x10001;
  r.info.flags = kRecordingOverflow;
  {  // two readable records (protocol §7): a LOD record and a bank map
    const snd::livelink::RecordLod lod{static_cast<std::uint32_t>(snd::livelink::RecordKind::Lod), 1u, 10u};
    const snd::livelink::RecordBankMap bm{static_cast<std::uint32_t>(snd::livelink::RecordKind::BankMap), 3u, 4u};
    const auto* a = reinterpret_cast<const std::uint8_t*>(&lod);
    const auto* b = reinterpret_cast<const std::uint8_t*>(&bm);
    r.records.insert(r.records.end(), a, a + sizeof lod);
    r.records.insert(r.records.end(), b, b + sizeof bm);
  }
  const std::vector<std::uint8_t> bytes = EncodeRecording(r);
  SND_CHECK_EQ(bytes.size(), 64u + 32u);
  SND_CHECK(std::memcmp(bytes.data(), "SNRC", 4) == 0);
  Recording back;
  std::string err;
  SND_REQUIRE(DecodeRecording(bytes.data(), bytes.size(), &back, &err));
  SND_CHECK(back.info == r.info);
  SND_CHECK(back.records == r.records);
  std::vector<std::uint8_t> bad = bytes;
  bad[80] ^= 1u;  // a record byte
  SND_CHECK(!DecodeRecording(bad.data(), bad.size(), &back, &err));
  SND_CHECK_EQ(err, std::string("checksum mismatch"));
  bad = bytes;
  bad[33] ^= 1u;  // the header's sample rate: covered by the checksum (version 2)
  SND_CHECK(!DecodeRecording(bad.data(), bad.size(), &back, &err));
  SND_CHECK_EQ(err, std::string("checksum mismatch"));
  SND_CHECK(!DecodeRecording(bytes.data(), bytes.size() - 1, &back, &err));
  bad = bytes;
  bad[0] = 'X';
  SND_CHECK(!DecodeRecording(bad.data(), bad.size(), &back, &err));
  // Unreadable records (an unknown kind) with a right checksum: refused.
  Recording broken = r;
  broken.records[16] = 99;
  const RecordingSummary s = Summarize(broken.records.data(), broken.records.size());
  SND_CHECK(!s.valid);
  SND_CHECK(!s.error.empty());
  const std::vector<std::uint8_t> bb = EncodeRecording(broken);
  SND_CHECK(!DecodeRecording(bb.data(), bb.size(), &back, &err));
  SND_CHECK_EQ(err, std::string("unreadable records"));
}

SND_TEST(ToolMeter, PeakHoldHoldsThenFalls) {
  PeakHoldParams prm;
  prm.holdNs = 1000000000ull;
  prm.fallDbPerSecond = 20.0f;
  PeakHold h;
  h.Update(0.5f, 0, prm);
  const float db = 20.0f * std::log10(0.5f);
  SND_CHECK_NEAR(h.HeldDb(0, prm), db, 1e-5);
  h.Update(0.1f, 500000000ull, prm);  // lower peak inside the hold: ignored
  SND_CHECK_NEAR(h.HeldDb(900000000ull, prm), db, 1e-5);
  SND_CHECK_NEAR(h.HeldDb(1500000000ull, prm), db - 10.0f, 1e-4);  // 0.5 s of fall at 20 dB/s
  h.Update(0.25f, 1500000000ull, prm);  // -12 dB catches the falling value (-16 dB)
  SND_CHECK_NEAR(h.HeldDb(1500000000ull, prm), 20.0f * std::log10(0.25f), 1e-4);
  SND_CHECK(!h.clipped());
  h.Update(1.0f, 2000000000ull, prm);
  SND_CHECK(h.clipped());
  SND_CHECK_EQ(LinearToDb(0.0f), kMeterFloorDb);
}

SND_TEST(ToolMixer, ParameterRevertTurnsOffAndResetsTheValue) {
  tooltest::ProjectDir p("parrev");
  WriteTree(p);
  Project pr;
  SND_REQUIRE(pr.Load(p.dir().string()));
  MixerModel m;
  m.Bind(pr);
  const Target on{Control::ParamOverrideOn, 1, 0, 0}, val{Control::ParamOverride, 1, 0, 0};  // Speed, default 1
  m.Set(val, 7.0f);
  m.Set(on, 1.0f);
  std::vector<OverrideItem> items = m.Overrides();
  SND_REQUIRE_EQ(items.size(), 1u);
  const std::size_t undo = m.history().UndoCount();
  SND_CHECK(m.Revert(items[0]));
  SND_CHECK_EQ(m.Get(on), 0.0f);
  SND_CHECK_EQ(m.Get(val), 1.0f);  // a new override starts from the bank default
  SND_CHECK_EQ(m.history().UndoCount(), undo + 1u);
  SND_CHECK(m.Undo());
  SND_CHECK_EQ(m.Get(val), 7.0f);
  SND_CHECK(m.RevertAll());
  SND_CHECK_EQ(m.Get(val), 1.0f);
}

SND_TEST(ToolMixer, KeyedSeriesBackToTheStartLeavesNoStep) {
  tooltest::ProjectDir p("wheelback");
  WriteTree(p);
  Project pr;
  SND_REQUIRE(pr.Load(p.dir().string()));
  MixerModel m;
  m.Bind(pr);
  const Target fa{Control::BusFader, Bus(pr, "A"), 0, 0};
  const std::size_t undo = m.history().UndoCount();
  m.Set(fa, -2.0f, 5);
  m.Set(fa, -1.0f, 5);
  m.Set(fa, -3.0f, 5);  // the bank value again
  SND_CHECK_EQ(m.history().UndoCount(), undo);
  m.Set(fa, -4.0f, 5);  // the series goes on: one step from -3
  SND_CHECK_EQ(m.history().UndoCount(), undo + 1u);
  SND_CHECK(m.Undo());
  SND_CHECK_EQ(m.Get(fa), -3.0f);
}

SND_TEST(ToolProject, DuplicateNamesGetUniqueKeys) {
  tooltest::ProjectDir p("dup");
  WriteTree(p);
  p.Write("buses/A2.json", "{\"type\": \"bus\", \"guid\": \"" + tooltest::G(30) + "\", \"name\": \"A\", \"parent\": \"Master\"}");
  p.Write("buses/A3.json", "{\"type\": \"bus\", \"guid\": \"" + tooltest::G(30) + "\", \"name\": \"A\", \"parent\": \"Master\"}");
  Project pr;
  pr.Load(p.dir().string());
  std::vector<std::string> keys;
  for (const BusDesc& b : pr.buses()) {
    for (const std::string& k : keys) SND_CHECK(k != b.key);
    keys.push_back(b.key);
    if (b.name == "B") SND_CHECK_EQ(b.key, Hex32(snd::HashId("B")));  // unique: the runtime id
  }
  bool guidKey = false, serialKey = false;
  for (const std::string& k : keys) {
    guidKey = guidKey || (k.size() == 33 && k[0] == 'g');
    serialKey = serialKey || k[0] == 'n';
  }
  SND_CHECK(guidKey && serialKey);
}

SND_TEST(ToolSync, NoAnswerAfterTwoSeconds) {
  CellStatus s;
  s.state = CellState::Pending;
  s.sentNs = 1000;
  SND_CHECK(!CellSync::IsNoAnswer(s, 1000 + kAckTimeoutNs));
  SND_CHECK(CellSync::IsNoAnswer(s, 1001 + kAckTimeoutNs));
  s.state = CellState::Synced;
  SND_CHECK(!CellSync::IsNoAnswer(s, 1001 + kAckTimeoutNs));
}
