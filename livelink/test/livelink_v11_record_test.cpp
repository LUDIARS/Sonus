// Recording and replay, protocol 1.1 (design.md 2.5, 11.3; record_format.hpp): the
// starting state (FromCreation / MidRun, override cells, live overrides), the data of
// ReplaceChunk and structural edits in the recording, the decision log and the output
// digest, and the 2.5 conditions the replay reports. Levels are analytic ("Tone" plays a
// constant through the Sfx bus).
#include "livelink_v11_common.hpp"

using namespace v11;

namespace {

struct Recorded {
  std::vector<std::uint8_t> bytes;
  std::vector<float> out;  // the recorded run, all blocks
  std::uint32_t blockSamples = 0;
};

enum class During { Plays, ReplaceChunk, StructEdit, Audition };

// A session recorded through the tool. `before` edits come before the recording starts.
void RecordSession(Recorded& rec, bool editsBefore, During during, ll::IStructEditor* editor = nullptr) {
  ll::RuntimeDesc rd;
  rd.structEditor = editor;
  Fixture f(Hooks::Live, Config(), nullptr, rd);
  rec.blockSamples = f.sys->BlockFrames() * f.sys->OutputChannels();
  f.Load(DevBank());
  SND_REQUIRE_EQ(f.StartServer(), SND_OK);
  ll::Client* c = nullptr;
  SND_REQUIRE_EQ(ll::Client::Create(MemoryClient(f.link), &c), SND_OK);
  c->Start();
  SND_REQUIRE(f.StepUntil([&] { return c->State() == ll::ClientState::Ready; }));
  std::int32_t r = 1;
  auto ack = [&](std::uint32_t seq) -> std::int32_t {
    r = 1;
    if (seq == 0u) return 2;
    if (!f.StepUntil([&] { return c->TakeAck(seq, &r); })) return 3;
    return r;
  };
  if (editsBefore) {
    SND_CHECK_EQ(ack(c->SetValue(BusVolume("Sfx", -6.0f))), SND_OK);
    SND_CHECK_EQ(ack(c->OverrideParam(HashId("Knob"), 0.75f, true, true)), SND_OK);
  }
  SND_REQUIRE_EQ(ack(c->StartRecording(true)), SND_OK);
  SND_REQUIRE(f.StepUntil([&] { return f.rt->Recording(); }));
  f.sys->Play(HashId("Tone"), snd_emitter_handle{0}, nullptr);
  f.Steps(4);
  switch (during) {
    case During::Plays:
      f.sys->Play(HashId("Blip"), snd_emitter_handle{0}, nullptr);
      break;
    case During::ReplaceChunk: {
      const std::vector<std::uint8_t> chunk = BussVolumes({-6.0f, 0.0f});
      SND_CHECK_EQ(ack(c->ReplaceChunk(kBankId, bf::chunk::BUSS, chunk.data(), chunk.size())), SND_OK);
      break;
    }
    case During::StructEdit: {
      const std::uint64_t args[2] = {HashId("Master"), 2u};
      SND_CHECK_EQ(ack(c->StructEdit(ll::StructOp::AddBus, HashId("New"), args, 2)), editor != nullptr ? SND_OK : SND_ERR_UNSUPPORTED);
      break;
    }
    case During::Audition:
      SND_CHECK_EQ(ack(c->Audition(HashId("Tone"), nullptr, false, true)), SND_OK);
      f.Steps(6);
      SND_CHECK_EQ(ack(c->StopAuditions(true)), SND_OK);
      break;
  }
  f.Steps(12);
  f.sys->Play(HashId("Blip"), snd_emitter_handle{0}, nullptr);
  f.Steps(16);
  SND_REQUIRE_EQ(ack(c->StopRecording(true)), SND_OK);
  SND_REQUIRE(f.StepUntil([&] { return !f.rt->Recording(); }));
  std::uint64_t last = 0;
  f.StepUntil([&] {
    const std::uint64_t n = c->RecordingBytes();
    const bool stable = n == last && n != 0u && f.server->Stats().recordBytes == n;
    last = n;
    return stable;
  });
  rec.bytes.resize(c->RecordingBytes());
  c->CopyRecording(rec.bytes.data(), rec.bytes.size());
  SND_CHECK_EQ(f.server->Stats().recordsDropped, 0u);
  rec.out = f.all;
  ll::Client::Destroy(c);
}

struct Replayed {
  std::vector<float> out;
  ll::ReplayInfo info{};
  std::vector<ll::ReplayDecision> decisions;
  std::uint64_t digest = 0;
  bool bitExact = false;
};

void Replay(const Recorded& rec, Replayed* rp, ll::IStructEditor* editor = nullptr, bool applyStart = true,
            snd_config cfg = Config()) {
  ll::ReplayDesc rd;
  rd.config = &cfg;
  rd.allocator = &snd::base::DefaultAllocator();
  rd.record = rec.bytes.data();
  rd.bytes = rec.bytes.size();
  rd.structEditor = editor;
  rd.applyStartState = applyStart;
  ll::Replayer* r = nullptr;
  SND_REQUIRE_EQ(ll::Replayer::Create(rd, &r), SND_OK);
  {
    Fixture f(Hooks::Replay, cfg, r);
    f.Load(DevBank());
    // Same block numbering as the recorded run: the lead blocks (starting state) come first.
    const std::uint64_t lead = r->Info().leadBlocks;
    SND_REQUIRE(f.sys->LastBlock().block + 1u + lead <= r->Info().firstBlock);
    while (f.sys->LastBlock().block + 1u + lead < r->Info().firstBlock) f.Step();
    r->Start();
    ll::OutputDigest d;
    for (std::uint64_t i = 0; i < r->Blocks() + 4u; ++i) {
      f.sys->RenderBlock(f.out.data());
      if (i < lead) continue;
      rp->out.insert(rp->out.end(), f.out.begin(), f.out.end());
      if (i < r->Blocks()) d.Add(f.out.data(), f.out.size());
    }
    SND_CHECK(r->Finished());
    rp->info = r->Info();
    rp->digest = d.value;
    rp->bitExact = r->BitExactExpected();
    rp->decisions.resize(rp->info.decisions);
    r->CopyDecisions(rp->decisions.data(), static_cast<std::uint32_t>(rp->decisions.size()));
  }
  ll::Replayer::Destroy(r);
}

// Samples of the replay that differ from the recorded run (from the first recorded block).
std::size_t Differences(const Recorded& rec, const Replayed& rp) {
  const std::size_t from = static_cast<std::size_t>(rp.info.firstBlock) * rec.blockSamples;
  const std::size_t n = static_cast<std::size_t>(rp.info.lastBlock - rp.info.firstBlock + 1u) * rec.blockSamples;
  if (rec.out.size() < from + n || rp.out.size() < n) return ~std::size_t{0};
  std::size_t diff = 0;
  for (std::size_t i = 0; i < n; ++i) diff += std::memcmp(&rec.out[from + i], &rp.out[i], sizeof(float)) != 0 ? 1u : 0u;
  return diff;
}

}  // namespace

SND_TEST(LivelinkV11Record, MidRunRecordingCarriesTheStartState) {
  Recorded rec;
  RecordSession(rec, true, During::Plays);
  SND_REQUIRE(!rec.bytes.empty());
  ll::RecordReader rr(rec.bytes.data(), rec.bytes.size());
  ll::RecordItem it;
  std::uint32_t index = 0, begins = 0, cells = 0, overrides = 0;
  bool beginAfterMaps = true;
  while (rr.Next(&it)) {
    if (index++ == 0u) SND_CHECK(it.kind == ll::RecordKind::Lod);  // still first (1.0 readers)
    if (ll::IsKind(it, ll::kRecordBegin)) {
      ++begins;
      SND_CHECK_EQ(it.begin.origin, static_cast<std::uint16_t>(ll::RecordOrigin::MidRun));
      SND_CHECK_EQ(it.begin.lostState, 0u);
    }
    if (it.kind == ll::RecordKind::BankMap && begins != 0u) beginAfterMaps = false;
    if (ll::IsKind(it, ll::kRecordCell)) {
      ++cells;
      SND_CHECK_EQ(it.cell.bankId, kBankId);
      SND_CHECK_EQ(it.cell.fourcc, bf::chunk::BUSS);
      SND_CHECK_EQ(it.cell.column, static_cast<std::uint32_t>(bf::col::BUSS_volumeDb));
      SND_CHECK_EQ(it.cell.row, 0u);
      SND_CHECK_EQ(it.cell.value, Bits(-6.0f));
      SND_CHECK_EQ(it.cell.original, Bits(0.0f));
    }
    if (ll::IsKind(it, ll::kRecordOverride)) {
      ++overrides;
      SND_CHECK_EQ(it.override_.overrideKind, static_cast<std::uint32_t>(cmd::LiveOverrideKind::Param));
      SND_CHECK_EQ(it.override_.id, HashId("Knob"));
      SND_CHECK_EQ(it.override_.value, Bits(0.75f));
    }
  }
  SND_CHECK(rr.Error() == ll::RecordError::None);
  SND_CHECK_EQ(begins, 1u);
  SND_CHECK(beginAfterMaps);
  SND_CHECK_EQ(cells, 1u);
  SND_CHECK_EQ(overrides, 1u);
  // The replay starts from the recorded state: -6 dB on Sfx with no SetValue in the recording.
  Replayed a, b, off;
  Replay(rec, &a);
  Replay(rec, &b);
  SND_CHECK_EQ(a.info.origin, static_cast<std::uint32_t>(ll::RecordOrigin::MidRun));
  SND_CHECK_EQ(a.info.startCells, 1u);
  SND_CHECK_EQ(a.info.startOverrides, 1u);
  SND_CHECK_EQ(a.info.startStateFailures, 0u);
  SND_CHECK_EQ(a.info.leadBlocks, 1u);
  SND_CHECK((a.info.conditions & ll::kReplayMidRun) != 0u);
  SND_CHECK(a.bitExact);
  SND_CHECK_EQ(a.digest, b.digest);
  SND_CHECK_EQ(Differences(rec, a), 0u);
  // Without the starting state the level is the bank's (0 dB), not the recorded run's.
  Replay(rec, &off, nullptr, false);
  SND_CHECK(Differences(rec, off) > 0u);
  SND_CHECK(a.digest != off.digest);
}

SND_TEST(LivelinkV11Record, RecordingFromCreationSaysSo) {
  snd_config cfg = Config();
  cfg.livelink.record_commands = 1;
  std::vector<std::uint8_t> bytes;
  {
    Fixture f(Hooks::Live, cfg);
    ll::ServerDesc sd;
    sd.recordUser = &bytes;
    sd.recordSink = [](void* u, const std::uint8_t* p, std::size_t n) noexcept {
      auto* v = static_cast<std::vector<std::uint8_t>*>(u);
      v->insert(v->end(), p, p + n);
    };
    SND_REQUIRE_EQ(f.StartServer(sd), SND_OK);  // before any command
    SND_REQUIRE(f.StepUntil([&] { return f.rt->Recording(); }));
    f.Load(DevBank());
    f.sys->Play(HashId("Tone"), snd_emitter_handle{0}, nullptr);
    f.Steps(8);
    f.StopServer();
  }
  SND_REQUIRE(!bytes.empty());
  ll::RecordReader rr(bytes.data(), bytes.size());
  ll::RecordItem it;
  std::uint32_t begins = 0, registers = 0;
  while (rr.Next(&it)) {
    if (ll::IsKind(it, ll::kRecordBegin)) {
      ++begins;
      SND_CHECK_EQ(it.begin.origin, static_cast<std::uint16_t>(ll::RecordOrigin::FromCreation));
    }
    if (it.kind != ll::RecordKind::Arena) continue;
    snd::cmd::ForEachCommand(it.normal, it.normal + it.sections.normalBytes, [&](const cmd::CommandHeader& h, const std::uint8_t*) {
      registers += h.type == static_cast<std::uint16_t>(cmd::CommandType::BankRegister) ? 1u : 0u;
    });
  }
  SND_CHECK(rr.Error() == ll::RecordError::None);
  SND_CHECK_EQ(begins, 1u);
  SND_CHECK_EQ(registers, 1u);  // the bank load is in the recording
}

SND_TEST(LivelinkV11Record, ReplaceChunkIsReplayedFromTheRecording) {
  Recorded rec;
  RecordSession(rec, false, During::ReplaceChunk);
  std::uint32_t liveData = 0;
  ll::RecordReader rr(rec.bytes.data(), rec.bytes.size());
  ll::RecordItem it;
  bool arenaNext = false, paired = true;
  while (rr.Next(&it)) {
    if (arenaNext) paired = paired && it.kind == ll::RecordKind::Arena;  // the data precedes its arena
    arenaNext = false;
    if (ll::IsKind(it, ll::kRecordLiveData)) {
      ++liveData;
      arenaNext = true;
      SND_CHECK_EQ(it.liveData.fourcc, bf::chunk::BUSS);
      SND_CHECK_EQ(it.liveData.bankId, kBankId);
    }
  }
  SND_CHECK_EQ(liveData, 1u);
  SND_CHECK(paired);
  Replayed a, b;
  Replay(rec, &a);
  Replay(rec, &b);
  SND_CHECK_EQ(a.info.liveData, 1u);
  SND_CHECK_EQ(a.info.replacedChunks, 1u);
  SND_CHECK_EQ(a.info.skippedCommands, 0u);
  SND_CHECK(a.bitExact);
  SND_CHECK_EQ(a.digest, b.digest);
  SND_CHECK_EQ(Differences(rec, a), 0u);
  // The replaced value is audible at the end of the replay (-6 dB on the constant).
  const std::size_t lastBlock = static_cast<std::size_t>(a.info.lastBlock - a.info.firstBlock) * rec.blockSamples;
  SND_CHECK(a.out[lastBlock] > 0.0f);
}

SND_TEST(LivelinkV11Record, TwoReplaysMakeTheSameDecisions) {
  Recorded rec;
  RecordSession(rec, false, During::Audition);
  Replayed a, b;
  Replay(rec, &a);
  Replay(rec, &b);
  SND_REQUIRE_EQ(a.decisions.size(), b.decisions.size());
  SND_CHECK(std::memcmp(a.decisions.data(), b.decisions.data(), a.decisions.size() * sizeof(ll::ReplayDecision)) == 0);
  SND_CHECK_EQ(a.digest, b.digest);
  SND_CHECK_EQ(Differences(rec, a), 0u);
  std::uint32_t tone = 0, blip = 0, audition = 0, stops = 0, ended = 0, voices = 0;
  for (const ll::ReplayDecision& d : a.decisions) {
    if (d.kind == (ll::kLogCommandBit | static_cast<std::uint32_t>(cmd::CommandType::Play))) {
      tone += d.id == HashId("Tone") ? 1u : 0u;
      blip += d.id == HashId("Blip") ? 1u : 0u;
    }
    audition += d.kind == (ll::kLogCommandBit | static_cast<std::uint32_t>(cmd::CommandType::LiveAudition)) ? 1u : 0u;
    stops += d.kind == (ll::kLogCommandBit | static_cast<std::uint32_t>(cmd::CommandType::Stop)) ? 1u : 0u;
    ended += d.kind == ll::kLogPlaybackEnded ? 1u : 0u;
    voices += d.kind == ll::kDecisionVoices ? 1u : 0u;
  }
  SND_CHECK_EQ(tone, 1u);
  SND_CHECK_EQ(blip, 1u);
  SND_CHECK_EQ(audition, 1u);
  SND_CHECK_EQ(stops, 1u);    // the stop-all reached the recording as a Stop of the audition
  SND_CHECK(ended >= 1u);     // the audition (the last Blip outlives the recording's span)
  SND_CHECK(voices >= 3u);    // the voice list changed several times
  SND_CHECK_EQ(a.info.decisionsLost, 0u);
  // Blocks are relative and ordered.
  for (std::size_t i = 1; i < a.decisions.size(); ++i) SND_CHECK(a.decisions[i].block >= a.decisions[i - 1].block);
}

SND_TEST(LivelinkV11Record, StructEditsAreRecordedAndReplayedThroughAnEditor) {
  FakeEditor live;
  Recorded rec;
  RecordSession(rec, false, During::StructEdit, &live);
  SND_REQUIRE_EQ(live.calls.size(), 1u);
  Replayed without, with;
  Replay(rec, &without);
  SND_CHECK_EQ(without.info.structEdits, 1u);
  SND_CHECK_EQ(without.info.structEditsNotApplied, 1u);
  SND_CHECK((without.info.conditions & ll::kReplayStructEdits) != 0u);
  SND_CHECK(!without.bitExact);
  FakeEditor replayEd;
  Replay(rec, &with, &replayEd);
  SND_REQUIRE_EQ(replayEd.calls.size(), 1u);
  SND_CHECK(replayEd.calls[0].op == ll::StructOp::AddBus);
  SND_CHECK_EQ(replayEd.calls[0].target, HashId("New"));
  SND_CHECK_EQ(replayEd.calls[0].arg0, static_cast<std::uint64_t>(HashId("Master")));
  SND_CHECK_EQ(with.info.structEditsNotApplied, 0u);
  SND_CHECK(with.bitExact);
}

// The replaying System itself changes its LOD (a tiny budget): reported, not bit exact.
SND_TEST(LivelinkV11Record, LodInTheReplayIsReported) {
  Recorded rec;
  RecordSession(rec, false, During::Plays);
  snd_config cfg = Config();
  cfg.tuning.lod_down_blocks = 1;
  cfg.tuning.load_target_percent = 1;
  Replayed r;
  Replay(rec, &r, nullptr, true, cfg);
  SND_CHECK(r.info.replayLodChanges >= 1u);
  SND_CHECK((r.info.conditions & ll::kReplayLodInReplay) != 0u);
  SND_CHECK(!r.bitExact);
}
