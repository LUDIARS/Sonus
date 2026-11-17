// Recording and replay (design.md 2.5, 11.3). A live session (game commands
// and a tool's value edits) is recorded through the live link; replaying the
// recording into fresh Systems renders bit-identical output twice, and the
// same output as the recorded run from the recording's first block.
#include <cstring>

#include "livelink_fixture.hpp"
#include "snd/livelink/record_format.hpp"

using namespace lltest;
using snd::HashId;

namespace {

struct Recorded {
  std::vector<std::uint8_t> bytes;
  std::vector<float> out;  // the recorded run, all blocks
  std::uint32_t blockSamples = 0;
};

void RecordSession(Recorded& rec) {
  Fixture f;
  rec.blockSamples = f.sys->BlockFrames() * f.sys->OutputChannels();
  f.Load(DevBank());
  SND_REQUIRE_EQ(f.StartServer(), SND_OK);
  ll::Client* c = nullptr;
  SND_REQUIRE_EQ(ll::Client::Create(MemoryClient(f.link), &c), SND_OK);
  c->Start();
  SND_REQUIRE(f.StepUntil([&] { return c->State() == ll::ClientState::Ready; }));
  const std::uint32_t seq = c->StartRecording(true);
  std::int32_t r = 1;
  SND_REQUIRE(f.StepUntil([&] { return c->TakeAck(seq, &r); }));
  SND_REQUIRE_EQ(r, SND_OK);
  SND_REQUIRE(f.StepUntil([&] { return f.rt->Recording(); }));
  // Game commands from the owner, edits from the tool, over many blocks.
  const snd_playback_handle tone = f.sys->Play(HashId("Tone"), snd_emitter_handle{0}, nullptr);
  f.Steps(5);
  std::uint32_t s2 = c->SetValue(BusVolume("Sfx", -6.0f));
  f.StepUntil([&] { return c->TakeAck(s2, &r); });
  SND_CHECK_EQ(r, SND_OK);
  f.Steps(4);  // the tone alone at -6 dB (checked in the replay)
  f.sys->Play(HashId("Blip"), snd_emitter_handle{0}, nullptr);
  f.Steps(3);
  f.sys->SetParamGlobal(HashId("Knob"), 0.5f);
  f.sys->Play(HashId("Blip"), snd_emitter_handle{0}, nullptr);
  f.Steps(7);
  s2 = c->SetValue(BusVolume("Master", -3.0f));
  f.StepUntil([&] { return c->TakeAck(s2, &r); });
  f.sys->Stop(tone, 0.01f, nullptr);
  f.Steps(10);
  const std::uint32_t s3 = c->StopRecording(true);
  SND_REQUIRE(f.StepUntil([&] { return c->TakeAck(s3, &r); }));
  SND_REQUIRE(f.StepUntil([&] { return !f.rt->Recording(); }));
  // Everything recorded has reached the tool once the server drained the ring.
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

void Replay(const Recorded& rec, ll::ReplayInfo* info, std::vector<float>& out) {
  const snd_config cfg = Config();
  ll::ReplayDesc rd;
  rd.config = &cfg;
  rd.allocator = &snd::base::DefaultAllocator();
  rd.record = rec.bytes.data();
  rd.bytes = rec.bytes.size();
  ll::Replayer* rp = nullptr;
  SND_REQUIRE_EQ(ll::Replayer::Create(rd, &rp), SND_OK);
  {
    Fixture f(Hooks::Replay, cfg, rp);
    f.Load(DevBank());
    const ll::ReplayInfo ri = rp->Info();
    // Same block numbering as the recorded run (the load takes the same number of blocks).
    SND_REQUIRE(f.sys->LastBlock().block + 1u <= ri.firstBlock);
    while (f.sys->LastBlock().block + 1u < ri.firstBlock) f.Step();
    rp->Start();
    for (std::uint64_t i = 0; i < rp->Blocks() + 4u; ++i) {
      f.sys->RenderBlock(f.out.data());  // no Update: the replay is the only producer
      out.insert(out.end(), f.out.begin(), f.out.end());
    }
    SND_CHECK(rp->Finished());
    *info = rp->Info();
  }
  ll::Replayer::Destroy(rp);
}

}  // namespace

SND_TEST(LivelinkRecord, RecordingIsWellFormedAndStartsWithLodAndBankMap) {
  Recorded rec;
  RecordSession(rec);
  SND_REQUIRE(!rec.bytes.empty());
  SND_REQUIRE(!rec.bytes.empty());
  ll::RecordReader rr(rec.bytes.data(), rec.bytes.size());
  ll::RecordItem it;
  std::uint32_t n = 0, arenas = 0, maps = 0, liveEdits = 0;
  while (rr.Next(&it)) {
    if (n == 0u) SND_CHECK(it.kind == snd::livelink::RecordKind::Lod);
    if (it.kind == snd::livelink::RecordKind::BankMap) {
      ++maps;
      SND_CHECK_EQ(it.bankMap.bankId, kBankId);
    }
    if (it.kind == snd::livelink::RecordKind::Arena) {
      ++arenas;
      snd::cmd::ForEachCommand(it.normal, it.normal + it.sections.normalBytes,
                               [&](const snd::cmd::CommandHeader& h, const std::uint8_t*) {
                                 liveEdits += h.type == static_cast<std::uint16_t>(snd::cmd::CommandType::LiveSetValue);
                               });
    }
    ++n;
  }
  SND_CHECK(rr.Error() == ll::RecordError::None);
  SND_CHECK_EQ(maps, 1u);
  SND_CHECK(arenas >= 6u);
  SND_CHECK_EQ(liveEdits, 2u);
}

SND_TEST(LivelinkRecord, ReplayIsBitIdenticalTwiceAndMatchesTheRecordedRun) {
  Recorded rec;
  RecordSession(rec);
  SND_REQUIRE(!rec.bytes.empty());
  ll::ReplayInfo a{}, b{};
  std::vector<float> r1, r2;
  Replay(rec, &a, r1);
  Replay(rec, &b, r2);
  SND_REQUIRE_EQ(r1.size(), r2.size());
  SND_CHECK(std::memcmp(r1.data(), r2.data(), r1.size() * sizeof(float)) == 0);
  SND_CHECK_EQ(a.bankMismatches, 0u);
  SND_CHECK_EQ(a.skippedCommands, 0u);
  SND_CHECK(a.commands >= 7u);
  // The recorded run from the first recorded block.
  const std::size_t from = static_cast<std::size_t>(a.firstBlock) * rec.blockSamples;
  const std::size_t n = static_cast<std::size_t>(a.lastBlock - a.firstBlock + 1u) * rec.blockSamples;
  SND_REQUIRE(rec.out.size() >= from + n && r1.size() >= n);
  std::size_t diff = 0;
  for (std::size_t i = 0; i < n; ++i) diff += std::memcmp(&rec.out[from + i], &r1[i], sizeof(float)) != 0 ? 1u : 0u;
  SND_CHECK_MSG(diff == 0u, "%zu samples differ from the recorded run", diff);
  // The edits are audible in the replay: -6 dB then -9 dB on the constant tone.
  bool sawEdit = false;
  for (std::size_t i = 0; i + 1 < n; i += rec.blockSamples) sawEdit = sawEdit || (r1[i] > 0.0f && r1[i] < 0.3f);
  SND_CHECK(sawEdit);
}

SND_TEST(LivelinkRecord, ReplayRejectsMalformedRecordings) {
  Recorded rec;
  RecordSession(rec);
  SND_REQUIRE(!rec.bytes.empty());
  const snd_config cfg = Config();
  ll::ReplayDesc rd;
  rd.config = &cfg;
  rd.allocator = &snd::base::DefaultAllocator();
  auto create = [&](const std::vector<std::uint8_t>& bytes) {
    rd.record = bytes.data();
    rd.bytes = bytes.size();
    ll::Replayer* rp = nullptr;
    const int r = ll::Replayer::Create(rd, &rp);
    ll::Replayer::Destroy(rp);
    return r;
  };
  SND_CHECK_EQ(create(rec.bytes), SND_OK);
  std::vector<std::uint8_t> t(rec.bytes.begin(), rec.bytes.end() - 8);
  SND_CHECK_EQ(create(t), SND_ERR_CORRUPT);
  std::vector<std::uint8_t> k = rec.bytes;
  k[0] = 9;  // unknown kind
  SND_CHECK_EQ(create(k), SND_ERR_CORRUPT);
  snd_config other = cfg;
  other.max_playbacks = 128;  // fallback capacities of another Config: only matters with a fallback section
  rd.config = &other;
  rd.record = rec.bytes.data();
  rd.bytes = rec.bytes.size();
  ll::Replayer* rp = nullptr;
  SND_CHECK_EQ(ll::Replayer::Create(rd, &rp), SND_OK);
  ll::Replayer::Destroy(rp);
}

// Config.livelink.record_commands: the server records from Start to Stop into a sink.
SND_TEST(LivelinkRecord, RecordCommandsConfigRecordsIntoTheSink) {
  snd_config cfg = Config();
  cfg.livelink.record_commands = 1;
  Fixture f(Hooks::Live, cfg);
  f.Load(DevBank());
  std::vector<std::uint8_t> sink;
  ll::ServerDesc sd;
  sd.recordUser = &sink;
  sd.recordSink = [](void* u, const std::uint8_t* b, std::size_t n) noexcept {
    auto* v = static_cast<std::vector<std::uint8_t>*>(u);
    v->insert(v->end(), b, b + n);
  };
  SND_REQUIRE_EQ(f.StartServer(sd), SND_OK);
  SND_REQUIRE(f.StepUntil([&] { return f.rt->Recording(); }));
  f.sys->Play(HashId("Tone"), snd_emitter_handle{0}, nullptr);
  f.Steps(4);
  // A tool cannot start another recording meanwhile.
  ll::Client* c = nullptr;
  SND_REQUIRE_EQ(ll::Client::Create(MemoryClient(f.link), &c), SND_OK);
  c->Start();
  SND_REQUIRE(f.StepUntil([&] { return c->State() == ll::ClientState::Ready; }));
  const std::uint32_t seq = c->StartRecording(true);
  std::int32_t r = 0;
  SND_CHECK(f.StepUntil([&] { return c->TakeAck(seq, &r); }));
  SND_CHECK_EQ(r, SND_ERR_LIMIT_REACHED);
  ll::Client::Destroy(c);
  f.StepUntil([&] { return f.server->Stats().recordBytes > 0u; });
  f.StopServer();  // the sink gets the rest
  ll::RecordReader rr(sink.data(), sink.size());
  ll::RecordItem it;
  bool play = false;
  while (rr.Next(&it)) {
    if (it.kind != ll::RecordKind::Arena) continue;
    snd::cmd::ForEachCommand(it.normal, it.normal + it.sections.normalBytes,
                             [&](const snd::cmd::CommandHeader& h, const std::uint8_t*) {
                               play = play || h.type == static_cast<std::uint16_t>(snd::cmd::CommandType::Play);
                             });
  }
  SND_CHECK(rr.Error() == ll::RecordError::None);
  SND_CHECK(play);
}
