// Independent tests, protocol 1.1 (组 L, indep_s7): recording and replay (design.md 2.5,
// 11.3; protocol §7). Properties I1-I6, I10 of indep_s7_properties.md:
//   - record order (Lod, BankMap, Begin, Cell / Override, then arenas), origin;
//   - two replays of one recording: equal decision logs and equal output digests;
//   - the replay renders what the recorded run rendered (from the recording's start);
//   - ReplaceChunk, structural edits and hot reloads inside a recording.
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <thread>
#include <vector>

#include "indep_s7_common.hpp"

namespace {

using namespace s7;
using indep::IndepBank;
using indep::Tool;
using lltest::Fixture;
using lltest::Hooks;
constexpr snd::Id kLive = lltest::kBankId;

struct Recorded {
  std::vector<std::uint8_t> bytes;
  std::vector<float> all;        // every block the recorded run rendered
  std::uint32_t blockSamples = 0;
  bool blockNumbersMatch = false;  // LastBlock().block == index of the block in `all`
  std::uint32_t clientErrors = 0;
};

struct RecordOpts {
  bool stateBefore = false;  // SetValue + OverrideParam before the recording starts
  bool replace = false;      // a ReplaceChunk during the recording
  bool structEdit = false;   // an AddBus (fake editor) during the recording
  bool reload = false;       // a ReloadBank during the recording
};

// Waits until the client's recording stopped growing after the recording ended.
void DrainRecording(Fixture& f, Tool& t) {
  std::uint64_t last = ~0ull;
  std::uint32_t stable = 0;
  f.StepUntil([&] {
    const std::uint64_t n = t.c->RecordingBytes();
    stable = (n == last && n != 0u) ? stable + 1u : 0u;
    last = n;
    return stable >= 20u;
  });
}

void Record(Recorded& rec, const RecordOpts& o) {
  FakeEditor ed;
  ll::RuntimeDesc rd;
  rd.structEditor = &ed;
  Fixture f(Hooks::Live, ReserveConfig(4, 4, 1u << 20), nullptr, rd);
  rec.blockSamples = f.sys->BlockFrames() * f.sys->OutputChannels();
  f.Load(IndepBank(), "a.sndb");
  SND_REQUIRE_EQ(f.StartServer(), SND_OK);
  Tool t(f);
  SND_REQUIRE(t.Ready());
  if (o.stateBefore) {
    SND_CHECK_EQ(t.Ack(t.c->SetValue(lltest::BusVolume("Sfx", -6.0f))), SND_OK);
    SND_CHECK_EQ(t.Ack(t.c->OverrideParam(HashId("Knob"), 0.5f, true, true)), SND_OK);
    f.Steps(10);  // the volume settles before the recording
  }
  SND_REQUIRE_EQ(t.Ack(t.c->StartRecording(true)), SND_OK);
  SND_REQUIRE(f.StepUntil([&] { return f.rt->Recording(); }));
  const snd_playback_handle tone = f.sys->Play(HashId("Tone"), snd_emitter_handle{0}, nullptr);
  f.Steps(6);
  SND_CHECK_EQ(t.Ack(t.c->SetValue(lltest::BusVolume("Sfx", -12.0f))), SND_OK);
  f.Steps(3);
  if (o.replace) {
    const std::vector<std::uint8_t> ch = BussChunk(-3.0f);
    SND_CHECK_EQ(t.Ack(t.c->ReplaceChunk(kLive, bf::chunk::BUSS, ch.data(), ch.size())), SND_OK);
    f.Steps(3);
  }
  if (o.structEdit) {
    const std::uint64_t args[2] = {HashId("Master"), 2};
    SND_CHECK_EQ(t.Ack(t.c->StructEdit(ll::StructOp::AddBus, HashId("B1"), args, 2)), SND_OK);
    f.Steps(3);
  }
  f.sys->Play(HashId("Blip"), snd_emitter_handle{0}, nullptr);
  SND_CHECK_EQ(t.Ack(t.c->Audition(HashId("Blip"), nullptr, false, true)), SND_OK);
  f.Steps(5);
  SND_CHECK_EQ(t.Ack(t.c->RevertValue(SfxVolumeCell())), SND_OK);
  f.Steps(3);
  if (o.reload) {
    SND_CHECK_EQ(t.Ack(t.c->ReloadBank(kLive, 0, "a.sndb")), SND_OK);
    f.sys->Play(HashId("Tone"), snd_emitter_handle{0}, nullptr);
    f.Steps(5);
  }
  f.sys->Stop(tone, 0.0f, nullptr);
  f.Steps(10);
  SND_REQUIRE_EQ(t.Ack(t.c->StopRecording(true)), SND_OK);
  SND_REQUIRE(f.StepUntil([&] { return !f.rt->Recording(); }));
  DrainRecording(f, t);
  rec.bytes.resize(t.c->RecordingBytes());
  bool truncated = false;
  t.c->CopyRecording(rec.bytes.data(), rec.bytes.size(), &truncated);
  SND_CHECK(!truncated);
  rec.all = f.all;
  rec.blockNumbersMatch = f.sys->LastBlock().block + 1u == f.all.size() / rec.blockSamples;
  ll::Error e[8];
  rec.clientErrors = t.c->CopyErrors(e, 8);
}

struct ReplayOut {
  ll::ReplayInfo info{};
  ll::OutputDigest digest;        // of the replay from the first recorded block on
  std::vector<ll::ReplayDecision> decisions;
  bool bitExact = false;
  bool finished = false;
  bool finishedAtBlocks = false;  // Finished() right after Blocks() blocks
  std::uint64_t blocks = 0;
  std::uint32_t editorCalls = 0;
};

snd_result Replay(const Recorded& rec, ReplayOut* out, ll::IStructEditor* editor = nullptr) {
  const snd_config cfg = ReserveConfig(4, 4, 1u << 20);
  ll::ReplayDesc d;
  d.config = &cfg;
  d.allocator = &snd::base::DefaultAllocator();
  d.record = rec.bytes.data();
  d.bytes = rec.bytes.size();
  d.structEditor = editor;
  ll::Replayer* rp = nullptr;
  const int r = ll::Replayer::Create(d, &rp);
  if (r != SND_OK) return static_cast<snd_result>(r);
  {
    Fixture g(Hooks::Replay, cfg, rp);
    g.Load(IndepBank(), "a.sndb");
    g.Steps(4);
    const std::size_t from = g.all.size();
    rp->Start();
    out->blocks = rp->Blocks();
    g.Steps(static_cast<std::uint32_t>(out->blocks));
    out->finishedAtBlocks = rp->Finished();
    const std::size_t to = g.all.size();
    g.Step();  // Finished() turns true when the block after the span starts (spec gap, see the notes)
    out->finished = rp->Finished();
    out->info = rp->Info();
    const std::size_t lead = std::size_t{out->info.leadBlocks} * rec.blockSamples;
    out->digest.Add(g.all.data() + from + lead, to - from - lead);
    out->decisions.resize(out->info.decisions);
    out->decisions.resize(rp->CopyDecisions(out->decisions.data(), static_cast<std::uint32_t>(out->decisions.size())));
    out->bitExact = rp->BitExactExpected();
  }
  ll::Replayer::Destroy(rp);
  return SND_OK;
}

// Digest of the recorded run's blocks [first, last].
bool RecordedDigest(const Recorded& rec, std::uint64_t first, std::uint64_t last, ll::OutputDigest* out) {
  const std::size_t a = static_cast<std::size_t>(first) * rec.blockSamples;
  const std::size_t b = static_cast<std::size_t>(last + 1u) * rec.blockSamples;
  if (b > rec.all.size() || a >= b) return false;
  out->Add(rec.all.data() + a, b - a);
  return true;
}

bool SameDecisions(const std::vector<ll::ReplayDecision>& a, const std::vector<ll::ReplayDecision>& b) {
  if (a.size() != b.size()) return false;
  for (std::size_t i = 0; i < a.size(); ++i)
    if (std::memcmp(&a[i], &b[i], sizeof(ll::ReplayDecision)) != 0) return false;
  return true;
}

struct Order {
  std::vector<std::uint32_t> kinds;
  std::vector<ll::RecordCell> cells;
  std::vector<ll::RecordOverride> overrides;
  ll::RecordBegin begin{};
  std::uint32_t liveData = 0, liveDataStruct = 0;
  bool ok = false;
};
Order Walk(const std::vector<std::uint8_t>& bytes) {
  Order o;
  ll::RecordReader rr(bytes.data(), bytes.size());
  ll::RecordItem it;
  while (rr.Next(&it)) {
    const auto k = static_cast<std::uint32_t>(it.kind);
    o.kinds.push_back(k);
    if (k == ll::kRecordCell) o.cells.push_back(it.cell);
    if (k == ll::kRecordOverride) o.overrides.push_back(it.override_);
    if (k == ll::kRecordBegin) o.begin = it.begin;
    if (k == ll::kRecordLiveData) {
      ++o.liveData;
      if (it.liveData.fourcc == ll::kStructEditTag) ++o.liveDataStruct;
    }
  }
  o.ok = rr.Error() == ll::RecordError::None;
  return o;
}

}  // namespace

// I1, I2: order and starting state of a mid-run recording.
SND_TEST(IndepS7Record, MidRunOrderAndStartingState) {
  Recorded rec;
  RecordOpts o;
  o.stateBefore = true;
  Record(rec, o);
  SND_REQUIRE(!rec.bytes.empty());
  const Order w = Walk(rec.bytes);
  SND_REQUIRE(w.ok);
  SND_REQUIRE(w.kinds.size() > 4u);
  SND_CHECK_EQ(w.kinds[0], static_cast<std::uint32_t>(ll::RecordKind::Lod));
  std::size_t i = 1;
  std::uint32_t maps = 0;
  while (i < w.kinds.size() && w.kinds[i] == static_cast<std::uint32_t>(ll::RecordKind::BankMap)) ++i, ++maps;
  SND_CHECK_EQ(maps, 1u);  // the one bank loaded at the start
  SND_REQUIRE(i < w.kinds.size());
  SND_CHECK_EQ(w.kinds[i], ll::kRecordBegin);
  ++i;
  std::uint32_t cells = 0, overrides = 0;
  while (i < w.kinds.size() && (w.kinds[i] == ll::kRecordCell || w.kinds[i] == ll::kRecordOverride)) {
    cells += w.kinds[i] == ll::kRecordCell ? 1u : 0u;
    overrides += w.kinds[i] == ll::kRecordOverride ? 1u : 0u;
    ++i;
  }
  SND_CHECK_EQ(cells, 1u);
  SND_CHECK_EQ(overrides, 1u);
  for (; i < w.kinds.size(); ++i) {  // none of the start-only kinds later
    SND_CHECK(w.kinds[i] != ll::kRecordBegin && w.kinds[i] != ll::kRecordCell && w.kinds[i] != ll::kRecordOverride);
  }
  SND_CHECK_EQ(w.begin.origin, static_cast<std::uint16_t>(ll::RecordOrigin::MidRun));
  SND_CHECK_EQ(w.begin.lostState, 0u);
  SND_REQUIRE_EQ(w.cells.size(), 1u);
  SND_CHECK_EQ(w.cells[0].bankId, kLive);
  SND_CHECK_EQ(w.cells[0].fourcc, bf::chunk::BUSS);
  SND_CHECK_EQ(w.cells[0].column, bf::col::BUSS_volumeDb);
  SND_CHECK_EQ(w.cells[0].row, 0u);
  SND_CHECK_EQ(w.cells[0].value, F32Bits(-6.0f));
  SND_CHECK_EQ(w.cells[0].original, F32Bits(0.0f));
  SND_REQUIRE_EQ(w.overrides.size(), 1u);
  SND_CHECK_EQ(w.overrides[0].overrideKind, static_cast<std::uint32_t>(cmd::LiveOverrideKind::Param));
  SND_CHECK_EQ(w.overrides[0].id, HashId("Knob"));
  SND_CHECK_EQ(w.overrides[0].value, static_cast<std::uint32_t>(F32Bits(0.5f)));
  ReplayOut r;
  SND_REQUIRE_EQ(Replay(rec, &r), SND_OK);
  SND_CHECK_EQ(r.info.origin, static_cast<std::uint32_t>(ll::RecordOrigin::MidRun));
  SND_CHECK((r.info.conditions & ll::kReplayMidRun) != 0u);
  SND_CHECK_EQ(r.info.startCells, 1u);
  SND_CHECK_EQ(r.info.startOverrides, 1u);
  SND_CHECK_EQ(r.info.leadBlocks, 1u);
  SND_CHECK_EQ(r.blocks, r.info.lastBlock - r.info.firstBlock + 1u + r.info.leadBlocks);  // I10
  SND_CHECK_EQ(r.info.startStateFailures, 0u);
  SND_CHECK(r.finished);
  std::printf("[indep_s7] Finished() after exactly Blocks()=%llu blocks: %d (after one more: %d)\n",
              static_cast<unsigned long long>(r.blocks), r.finishedAtBlocks ? 1 : 0, r.finished ? 1 : 0);
}

// I2: a recording that starts before any command was applied (record_commands from Start).
namespace {
struct Sink {
  std::vector<std::uint8_t> bytes;
};
void SinkFn(void* user, const std::uint8_t* b, std::size_t n) noexcept {
  auto* s = static_cast<Sink*>(user);
  s->bytes.insert(s->bytes.end(), b, b + n);  // test-only sink (live link thread, not RT)
}
}  // namespace

SND_TEST(IndepS7Record, FromCreationOrigin) {
  snd_config cfg = lltest::Config();
  cfg.livelink.record_commands = 1;
  Sink sink;
  {
    Fixture f(Hooks::Live, cfg);
    ll::ServerDesc sd;
    sd.recordSink = &SinkFn;
    sd.recordUser = &sink;
    SND_REQUIRE_EQ(f.StartServer(sd), SND_OK);
    SND_REQUIRE(f.StepUntil([&] { return f.rt->Recording(); }));
    f.Load(IndepBank(), "a.sndb");
    f.sys->Play(HashId("Tone"), snd_emitter_handle{0}, nullptr);
    f.Steps(10);
    f.StopServer();
    f.Steps(5);
  }
  SND_REQUIRE(!sink.bytes.empty());
  const Order w = Walk(sink.bytes);
  SND_CHECK(w.ok);
  SND_REQUIRE(!w.kinds.empty());
  SND_CHECK_EQ(w.kinds[0], static_cast<std::uint32_t>(ll::RecordKind::Lod));
  SND_CHECK_EQ(w.begin.kind, ll::kRecordBegin);
  SND_CHECK_EQ(w.begin.origin, static_cast<std::uint16_t>(ll::RecordOrigin::FromCreation));
  SND_CHECK(w.cells.empty());
  SND_CHECK(w.overrides.empty());
}

// I3, I4: two replays agree; the replay equals the recorded run.
SND_TEST(IndepS7Record, TwoReplaysAgreeAndEqualTheRecordedRun) {
  Recorded rec;
  Record(rec, RecordOpts{});
  SND_REQUIRE(!rec.bytes.empty());
  SND_CHECK_EQ(rec.clientErrors, 0u);
  ReplayOut a, b;
  SND_REQUIRE_EQ(Replay(rec, &a), SND_OK);
  SND_REQUIRE_EQ(Replay(rec, &b), SND_OK);
  SND_CHECK(a.finished && b.finished);
  SND_CHECK(a.bitExact);
  SND_CHECK_EQ(a.info.conditions & ll::kReplayBreaksBitExact, 0u);
  SND_CHECK_EQ(a.info.skippedCommands, 0u);
  SND_CHECK_EQ(a.info.invalidCommands, 0u);
  SND_CHECK(!a.decisions.empty());
  SND_CHECK_EQ(a.info.decisionsLost, 0u);
  SND_CHECK(SameDecisions(a.decisions, b.decisions));
  SND_CHECK_EQ(a.digest.value, b.digest.value);
  SND_CHECK_EQ(a.digest.samples, b.digest.samples);
  // the decision log has the two Plays, the audition and the Stop of the recording
  std::uint32_t plays = 0, auditions = 0, stops = 0;
  for (const auto& dd : a.decisions) {
    if (dd.kind == (0x8000u | static_cast<std::uint32_t>(cmd::CommandType::Play))) ++plays;
    if (dd.kind == (0x8000u | static_cast<std::uint32_t>(cmd::CommandType::LiveAudition))) ++auditions;
    if (dd.kind == (0x8000u | static_cast<std::uint32_t>(cmd::CommandType::Stop))) ++stops;
  }
  SND_CHECK_EQ(plays, 2u);
  SND_CHECK_EQ(auditions, 1u);
  SND_CHECK_EQ(stops, 1u);
  // I4
  SND_REQUIRE(rec.blockNumbersMatch);
  ll::OutputDigest recorded;
  SND_REQUIRE(RecordedDigest(rec, a.info.firstBlock, a.info.lastBlock, &recorded));
  SND_CHECK_EQ(recorded.samples, a.digest.samples);
  SND_CHECK_MSG(recorded.value == a.digest.value, "recorded %016llx replay %016llx",
                static_cast<unsigned long long>(recorded.value), static_cast<unsigned long long>(a.digest.value));
}

// I4 with a starting state (lead block).
SND_TEST(IndepS7Record, MidRunWithStateEqualsTheRecordedRun) {
  Recorded rec;
  RecordOpts o;
  o.stateBefore = true;
  Record(rec, o);
  SND_REQUIRE(!rec.bytes.empty());
  ReplayOut a, b;
  SND_REQUIRE_EQ(Replay(rec, &a), SND_OK);
  SND_REQUIRE_EQ(Replay(rec, &b), SND_OK);
  SND_CHECK(a.bitExact);
  SND_CHECK(SameDecisions(a.decisions, b.decisions));
  SND_CHECK_EQ(a.digest.value, b.digest.value);
  SND_REQUIRE(rec.blockNumbersMatch);
  ll::OutputDigest recorded;
  SND_REQUIRE(RecordedDigest(rec, a.info.firstBlock, a.info.lastBlock, &recorded));
  SND_CHECK_MSG(recorded.value == a.digest.value, "recorded %016llx replay %016llx",
                static_cast<unsigned long long>(recorded.value), static_cast<unsigned long long>(a.digest.value));
}

// I5: a ReplaceChunk inside the recording is replayed.
SND_TEST(IndepS7Record, ReplaceChunkIsReplayed) {
  Recorded rec;
  RecordOpts o;
  o.replace = true;
  Record(rec, o);
  SND_REQUIRE(!rec.bytes.empty());
  const Order w = Walk(rec.bytes);
  SND_CHECK_EQ(w.liveData, 1u);
  ReplayOut a, b;
  SND_REQUIRE_EQ(Replay(rec, &a), SND_OK);
  SND_REQUIRE_EQ(Replay(rec, &b), SND_OK);
  SND_CHECK_EQ(a.info.liveData, 1u);
  SND_CHECK_EQ(a.info.replacedChunks, 1u);
  SND_CHECK_EQ(a.info.skippedCommands, 0u);
  SND_CHECK(a.bitExact);
  SND_CHECK_EQ(a.digest.value, b.digest.value);
  SND_CHECK(SameDecisions(a.decisions, b.decisions));
  SND_REQUIRE(rec.blockNumbersMatch);
  ll::OutputDigest recorded;
  SND_REQUIRE(RecordedDigest(rec, a.info.firstBlock, a.info.lastBlock, &recorded));
  SND_CHECK_MSG(recorded.value == a.digest.value, "recorded %016llx replay %016llx",
                static_cast<unsigned long long>(recorded.value), static_cast<unsigned long long>(a.digest.value));
}

// I6: structural edits inside a recording.
SND_TEST(IndepS7Record, StructEditIsReportedOrReplayed) {
  Recorded rec;
  RecordOpts o;
  o.structEdit = true;
  Record(rec, o);
  SND_REQUIRE(!rec.bytes.empty());
  const Order w = Walk(rec.bytes);
  SND_CHECK_EQ(w.liveDataStruct, 1u);
  ReplayOut none;
  SND_REQUIRE_EQ(Replay(rec, &none), SND_OK);
  SND_CHECK_EQ(none.info.structEdits, 1u);
  SND_CHECK((none.info.conditions & ll::kReplayStructEdits) != 0u);
  SND_CHECK_EQ(none.info.structEditsNotApplied, 1u);
  SND_CHECK(!none.bitExact);
  FakeEditor ed;
  ReplayOut with;
  SND_REQUIRE_EQ(Replay(rec, &with, &ed), SND_OK);
  SND_CHECK_EQ(ed.Count(), 1u);
  if (ed.Count() == 1u) {
    SND_CHECK_EQ(ed.At(0).op, static_cast<std::uint32_t>(ll::StructOp::AddBus));
    SND_CHECK_EQ(ed.At(0).targetId, HashId("B1"));
    SND_CHECK_EQ(ed.At(0).argCount, 2u);
    SND_CHECK_EQ(ed.At(0).arg0, HashId("Master"));
  }
  SND_CHECK_EQ(with.info.structEditsNotApplied, 0u);
  SND_CHECK_EQ(with.info.conditions & ll::kReplayStructEdits, 0u);
  // the same decisions whether or not the edit is applied by a fake editor that changes nothing
  SND_CHECK(SameDecisions(none.decisions, with.decisions));
}

// A hot reload inside a recording: the replay reports the bank change.
SND_TEST(IndepS7Record, HotReloadInsideRecordingIsABankChange) {
  Recorded rec;
  RecordOpts o;
  o.reload = true;
  Record(rec, o);
  SND_REQUIRE(!rec.bytes.empty());
  ReplayOut a;
  SND_REQUIRE_EQ(Replay(rec, &a), SND_OK);
  SND_CHECK(a.info.bankChanges >= 1u);
  SND_CHECK((a.info.conditions & ll::kReplayBankChanges) != 0u);
  SND_CHECK(!a.bitExact);
  SND_CHECK(a.finished);
  ReplayOut b;
  SND_REQUIRE_EQ(Replay(rec, &b), SND_OK);
  SND_CHECK(SameDecisions(a.decisions, b.decisions));  // still deterministic between replays
}
