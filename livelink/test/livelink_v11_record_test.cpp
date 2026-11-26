// Recording and replay, protocol 1.1 (design.md 2.5, 11.3; record_format.hpp): the
// starting state (FromCreation / MidRun, override cells, live overrides), the data of
// ReplaceChunk and structural edits in the recording, the decision log and the output
// digest, and the 2.5 conditions the replay reports. Levels are analytic ("Tone" plays a
// constant through the Sfx bus).
#include "livelink_v11_common.hpp"
#include "snd/internal/rt_check.hpp"

using namespace v11;

namespace {

struct Recorded {
  std::vector<std::uint8_t> bytes;
  std::vector<float> out;  // the recorded run, all blocks
  std::uint32_t blockSamples = 0;
};

enum class During { Plays, ReplaceChunk, StructEdit, Mixer, StaticRoutesBefore, Audition, ToneBefore, TwoReplaceChunks };

// A session recorded through the tool. `before` edits come before the recording starts.
void RecordSession(Recorded& rec, bool editsBefore, During during, ll::IStructEditor* editor = nullptr, snd_config cfg = Config()) {
  ll::RuntimeDesc rd;
  rd.structEditor = editor;
  Fixture f(Hooks::Live, cfg, nullptr, rd);
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
  if (during == During::StaticRoutesBefore) {
    const auto aux = HashId("PreExistingAux");
    const std::uint64_t bus[]{HashId("Master"), 2u, Bits(-6)};
    const std::uint64_t send[]{aux, Bits(0)};
    SND_REQUIRE_EQ(ack(c->StructEdit(ll::StructOp::AddBus, aux, bus, 3)), SND_OK);
    SND_REQUIRE_EQ(ack(c->StructEdit(ll::StructOp::AddSend, HashId("Sfx"), send, 2)), SND_OK);
  }
  if (during == During::ToneBefore) {
    f.sys->Play(HashId("Tone"), snd_emitter_handle{0}, nullptr);
    f.Steps(4);
  }
  SND_REQUIRE_EQ(ack(c->StartRecording(true)), SND_OK);
  SND_REQUIRE(f.StepUntil([&] { return f.rt->Recording(); }));
  f.sys->Play(HashId("Tone"), snd_emitter_handle{0}, nullptr);
  f.Steps(4);
  switch (during) {
    case During::StaticRoutesBefore: {
      f.Steps(4);
      const cmd::LiveSetValue revert{0, ll::kFieldRevertAll, 0, 0, 0};
      SND_REQUIRE_EQ(f.sys->RecordLive(cmd::CommandType::LiveSetValue, &revert, sizeof(revert)), SND_OK);
      break;
    }
    case During::Plays:
    case During::ToneBefore:
      f.sys->Play(HashId("Blip"), snd_emitter_handle{0}, nullptr);
      break;
    case During::TwoReplaceChunks: {  // the same slot (same key) twice, other values
      const std::vector<std::uint8_t> first = BussVolumes({-3.0f, 0.0f});
      SND_CHECK_EQ(ack(c->ReplaceChunk(kBankId, bf::chunk::BUSS, first.data(), first.size())), SND_OK);
      f.Steps(3);
      [[fallthrough]];
    }
    case During::ReplaceChunk: {
      const std::vector<std::uint8_t> chunk = BussVolumes({-6.0f, 0.0f});
      SND_CHECK_EQ(ack(c->ReplaceChunk(kBankId, bf::chunk::BUSS, chunk.data(), chunk.size())), SND_OK);
      break;
    }
    case During::Mixer: {
      const auto aux = HashId("LiveAux");
      const std::uint64_t bus[]{HashId("Master"), 2u, Bits(-6)};
      const std::uint64_t send[]{aux, Bits(0)};
      const std::uint64_t fx[]{HashId("snd.fx.echo"), 0u, Bits(4), Bits(0.3f), Bits(0), Bits(1), Bits(20)};
      SND_REQUIRE_EQ(ack(c->StructEdit(ll::StructOp::AddBus, aux, bus, 3)), SND_OK);
      SND_REQUIRE_EQ(ack(c->StructEdit(ll::StructOp::AddSend, HashId("Sfx"), send, 2)), SND_OK);
      SND_REQUIRE_EQ(ack(c->StructEdit(ll::StructOp::AddEffect, aux, fx, 7)), SND_OK);
      f.Steps(16);
      const std::uint64_t index = 0;
      SND_REQUIRE_EQ(ack(c->StructEdit(ll::StructOp::RemoveEffect, aux, &index, 1)), SND_OK);
      // The same core command emitted on disconnect; keep the connection to collect its recording.
      f.Steps(3);
      const cmd::LiveSetValue revert{0, ll::kFieldRevertAll, 0, 0, 0};
      SND_REQUIRE_EQ(f.sys->RecordLive(cmd::CommandType::LiveSetValue, &revert, sizeof(revert)), SND_OK);
      f.Steps(4);
      // A rejected edit must have no effect even in a replay with a different editor.
      const std::uint64_t cycle[]{HashId("Sfx"), Bits(0)};
      SND_REQUIRE_EQ(ack(c->StructEdit(ll::StructOp::AddSend, HashId("Master"), cycle, 2)), SND_ERR_INVALID_ARGUMENT);
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
  bool reproduces = false;
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
      SND_CHECK_EQ(r->Finished(), i >= r->Blocks());  // D-175: right after the Blocks()-th block
      f.sys->RenderBlock(f.out.data());
      if (i < lead) continue;
      rp->out.insert(rp->out.end(), f.out.begin(), f.out.end());
      if (i < r->Blocks()) d.Add(f.out.data(), f.out.size());
    }
    SND_CHECK(r->Finished());
    rp->info = r->Info();
    rp->digest = d.value;
    rp->bitExact = r->BitExactExpected();
    rp->reproduces = r->ReproducesRecordedRun();
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
  SND_CHECK(a.reproduces);  // MidRun with its whole starting state (nothing played before)
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

// The replaying System itself changes its LOD (a tiny budget): whenever it does, the replay
// reports it and does not promise bit identity. (Whether the LOD changes depends on the
// machine's speed; the report itself is fixed by LivelinkV11Unit.ReplayLodEventIsReported.)
SND_TEST(LivelinkV11Record, LodInTheReplayIsReported) {
  Recorded rec;
  RecordSession(rec, false, During::Plays);
  snd_config cfg = Config();
  cfg.tuning.lod_down_blocks = 1;
  cfg.tuning.load_target_percent = 1;
  Replayed r;
  Replay(rec, &r, nullptr, true, cfg);
  SND_CHECK_EQ((r.info.conditions & ll::kReplayLodInReplay) != 0u, r.info.replayLodChanges != 0u);
  if (r.info.replayLodChanges != 0u) SND_CHECK(!r.bitExact);
}

// D-175: a playback alive in core at the start is state the recording does not carry:
// the replays still agree with each other, but they do not reproduce the recorded run.
SND_TEST(LivelinkV11Record, PlaybackAliveAtTheStartIsLostState) {
  Recorded rec;
  RecordSession(rec, false, During::ToneBefore);
  ll::RecordReader rr(rec.bytes.data(), rec.bytes.size());
  ll::RecordItem it;
  std::uint32_t lost = 0;
  while (rr.Next(&it)) {
    if (ll::IsKind(it, ll::kRecordBegin)) lost = it.begin.lostState;
  }
  SND_CHECK_EQ(lost, 1u);
  Replayed a, b;
  Replay(rec, &a);
  Replay(rec, &b);
  SND_CHECK((a.info.conditions & ll::kReplayStateLost) != 0u);
  SND_CHECK(a.bitExact);
  SND_CHECK(!a.reproduces);
  SND_CHECK_EQ(a.digest, b.digest);
  SND_CHECK(Differences(rec, a) > 0u);
}

// Two replacements through the same slot (the same pairing key) replay in order.
SND_TEST(LivelinkV11Record, TwoReplacementsWithTheSameKeyReplayInOrder) {
  Recorded rec;
  RecordSession(rec, false, During::TwoReplaceChunks);
  std::vector<std::uint64_t> keys;
  ll::RecordReader rr(rec.bytes.data(), rec.bytes.size());
  ll::RecordItem it;
  while (rr.Next(&it)) {
    if (ll::IsKind(it, ll::kRecordLiveData)) keys.push_back(it.liveData.key);
  }
  SND_REQUIRE_EQ(keys.size(), 2u);
  SND_CHECK_EQ(keys[0], keys[1]);
  Replayed a;
  Replay(rec, &a);
  SND_CHECK_EQ(a.info.replacedChunks, 2u);
  SND_CHECK_EQ(a.info.skippedCommands, 0u);
  SND_CHECK(a.reproduces);
  SND_CHECK_EQ(Differences(rec, a), 0u);
}

SND_TEST(LivelinkV11Record, RealMixerMatchesEveryRecordedSampleAndRepeatedReplay) {
  Recorded rec;
  auto* editor = &ll::CoreStructEditor();
  RecordSession(rec, false, During::Mixer, editor);
  Replayed a, b;
  Replay(rec, &a, editor);
  Replay(rec, &b, editor);
  SND_REQUIRE_EQ(a.info.structEdits, 4u);
  SND_CHECK_EQ(a.info.skippedCommands, 0u);  // a refused cycle was an observed no-op
  SND_CHECK(a.reproduces);
  SND_CHECK_EQ(a.info.structEditsNotApplied, 0u);
  SND_CHECK_EQ(Differences(rec, a), 0u);
  SND_CHECK_EQ(Differences(rec, b), 0u);
  SND_CHECK_EQ(a.digest, b.digest);
}

// Override a standard type. This plugin stores a self-pointer: moving its initialised
// bytes to a different address would break processing even if the parameters matched.
SND_TEST(LivelinkV11Record, ExternalDspPreparesOffAudioAndReplaysAtItsFinalAddress) {
  struct State { const void* self; float gain; };
  struct Counters { std::atomic<unsigned> sizes{0}, inits{0}, processed{0}, errors{0}; } counters;
  snd_dsp_effect fx{};
  fx.user = &counters;
  fx.effect_type = HashId("snd.fx.echo");
  fx.state_bytes = [](void* u, const snd_effect_desc* d) -> std::uint32_t {
    auto& c = *static_cast<Counters*>(u);
    ++c.sizes;
    if (snd::rt::InRtScope() || !d->sample_rate || d->channels != 2) ++c.errors;
    return sizeof(State);
  };
  fx.init = [](void* u, void* state, const snd_effect_desc* d, std::uint32_t rate) {
    auto& c = *static_cast<Counters*>(u);
    ++c.inits;
    if (snd::rt::InRtScope() || rate != d->sample_rate || d->param_count != 5 ||
        reinterpret_cast<std::uintptr_t>(state) % 64 != 0) ++c.errors;
    new (state) State{state, d->initial_params[1]};
  };
  fx.process = [](void* u, void* state, const float* const* in, float* const* out,
                  std::uint32_t channels, std::uint32_t frames, const snd_param_block*) {
    auto& c = *static_cast<Counters*>(u);
    const auto& s = *static_cast<State*>(state);
    ++c.processed;
    if (s.self != state) ++c.errors;
    for (std::uint32_t i = 0; i < channels; ++i)
      for (std::uint32_t j = 0; j < frames; ++j) out[i][j] = in[i][j] * s.gain;
  };
  fx.is_tail_done = [](void*, const void*) -> std::int32_t { return 1; };
  auto cfg = Config();
  cfg.plugins.effects = &fx;
  cfg.plugins.effect_count = 1;
  auto* editor = &ll::CoreStructEditor();
  Recorded rec;
  RecordSession(rec, false, During::Mixer, editor, cfg);
  // This recording used an external override of a standard ID; silently substituting
  // the builtin on a different replay host would produce the wrong samples.
  auto missing = Config();
  ll::ReplayDesc rd;
  rd.config = &missing;
  rd.allocator = &snd::base::DefaultAllocator();
  rd.record = rec.bytes.data();
  rd.bytes = rec.bytes.size();
  rd.structEditor = editor;
  ll::Replayer* bad = nullptr;
  SND_CHECK_EQ(ll::Replayer::Create(rd, &bad), SND_ERR_UNSUPPORTED);
  SND_CHECK(bad == nullptr);
  const auto usedLive = counters.processed.load();
  SND_REQUIRE(usedLive > 0u);
  Replayed a, b;
  Replay(rec, &a, editor, true, cfg);
  Replay(rec, &b, editor, true, cfg);
  SND_CHECK_EQ(counters.inits.load(), 3u);
  SND_CHECK_EQ(counters.sizes.load(), 3u);
  SND_CHECK_EQ(counters.errors.load(), 0u);
  SND_CHECK_EQ(counters.processed.load(), 3u * usedLive);
  SND_CHECK(a.reproduces && b.reproduces);
  SND_CHECK_EQ(Differences(rec, a), 0u);
  SND_CHECK_EQ(Differences(rec, b), 0u);
}

SND_TEST(LivelinkV11Record, StartingRoutingTopologyReplaysIncludingLaterUndo) {
  Recorded rec;
  auto* editor = &ll::CoreStructEditor();
  RecordSession(rec, false, During::StaticRoutesBefore, editor);
  Replayed a;
  Replay(rec, &a, editor);
  SND_CHECK_EQ(a.info.lostState, 0u);
  SND_CHECK_EQ(a.info.structEdits, 2u);
  SND_CHECK(a.reproduces);
  SND_CHECK_EQ(Differences(rec, a), 0u);
}
