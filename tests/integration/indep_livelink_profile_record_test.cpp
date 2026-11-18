// Independent tests: core hooks, profiler stream and recording / replay
// (design.md 2.5, 11.2 profiler, 11.3; livelink-protocol.md §6, §7).
#include <chrono>
#include <thread>

#include "indep_livelink_common.hpp"
#include "snd/livelink/record_format.hpp"

using namespace indep;
using lltest::Fixture;
using lltest::Hooks;

namespace {

constexpr std::uint32_t kAllProfiler = ll::kProfVoices | ll::kProfTiming | ll::kProfMemory | ll::kProfStreams |
                                       ll::kProfLod | ll::kProfEventLog | ll::kProfMeters;

// The same game script on a System with the given hooks; returns every rendered sample.
std::vector<float> Script(Hooks hooks, bool profile) {
  Fixture f(hooks);
  BankOpts o;
  o.withLimiter = true;
  o.limiterDb = -9.0f;
  o.withCategory = true;
  f.Load(IndepBank(o), "a.sndb");
  if (profile && f.rt != nullptr) {
    f.rt->SetProfiler(kAllProfiler, 1000);
    f.rt->RequestRecording(true);
  }
  const snd_playback_handle tone = f.sys->Play(HashId("Tone"), snd_emitter_handle{0}, nullptr);
  f.Steps(10);
  f.sys->Play(HashId("Blip"), snd_emitter_handle{0}, nullptr);
  f.sys->SetParamGlobal(HashId("Knob"), 0.25f);
  f.sys->SetCategoryVolume(HashId("Cat"), 0.5f, 0.02f);
  f.Steps(30);
  f.sys->Stop(tone, 0.01f, nullptr);
  f.Steps(20);
  return f.all;
}

}  // namespace

// H1 / H2 / H3: no hooks, hooks with the profiler off, and hooks with every profiler
// channel and the recording on render bit-identical output (the hooks only read), and
// the RT guard records no violation on the audio thread (the test runner fails a
// test that records one).
SND_TEST(IndepLivelinkHooks, HooksDoNotChangeTheOutput) {
  const std::vector<float> none = Script(Hooks::None, false);
  const std::vector<float> off = Script(Hooks::Live, false);
  const std::vector<float> on = Script(Hooks::Live, true);
  SND_REQUIRE(!none.empty());
  SND_REQUIRE_EQ(none.size(), off.size());
  SND_REQUIRE_EQ(none.size(), on.size());
  SND_CHECK(std::memcmp(none.data(), off.data(), none.size() * sizeof(float)) == 0);
  SND_CHECK(std::memcmp(none.data(), on.data(), none.size() * sizeof(float)) == 0);
  double energy = 0;
  for (float x : none) energy += static_cast<double>(x) * x;
  SND_CHECK(energy > 1.0);
}

// E2 / E7: the bus meters the tool receives match the output: a constant signal has
// peak == rms == its level; a master edit of -6 dB scales the master meter against the
// unchanged Sfx meter. The voice list shows the one voice with its event and bus.
SND_TEST(IndepLivelinkProfiler, MetersAndVoicesMatchTheOutput) {
  Fixture f;
  f.Load(IndepBank(), "a.sndb");
  f.sys->Play(HashId("Tone"), snd_emitter_handle{0}, nullptr);
  f.Steps(16);
  SND_REQUIRE_EQ(f.StartServer(), SND_OK);
  Tool t(f);
  SND_REQUIRE(t.Ready());
  t.c->SetProfilerMask(ll::kProfMeters | ll::kProfVoices, 60);
  f.Steps(400);  // > 6 RMS time constants (0.3 s)
  const double level = f.LastLeft();
  auto meters = [&](ll::MeterEntry* out, std::uint32_t cap) {
    std::uint64_t before = 0;
    t.c->CopyMeters(out, cap, &before);
    std::uint64_t now = before;
    f.StepUntil([&] {
      t.c->CopyMeters(out, cap, &now);
      return now != before;
    });
    return t.c->CopyMeters(out, cap);
  };
  ll::MeterEntry m[16];
  std::uint32_t n = meters(m, 16);
  const ll::MeterEntry* master = nullptr;
  const ll::MeterEntry* sfx = nullptr;
  for (std::uint32_t i = 0; i < n; ++i) {
    if (m[i].busId == HashId("Master") || m[i].busId == 0u) master = &m[i];  // see the report: master is id 0
    if (m[i].busId == HashId("Sfx")) sfx = &m[i];
  }
  SND_REQUIRE(master != nullptr && sfx != nullptr);
  SND_CHECK_EQ(master->channels, 2u);
  SND_CHECK_NEAR(master->peak[0], level, 1e-4);
  SND_CHECK_NEAR(master->rms[0], level, level * 0.01);
  SND_CHECK_NEAR(sfx->peak[0], level, 1e-4);
  ll::VoiceInfo v[8];
  SND_CHECK(f.StepUntil([&] { return t.c->CopyVoices(v, 8) == 1u; }));
  SND_CHECK_EQ(v[0].eventId, HashId("Tone"));
  SND_CHECK_EQ(v[0].busId, HashId("Sfx"));
  SND_CHECK_EQ(v[0].isVirtual, 0u);
  // Master -6 dB: the master meter follows, the Sfx meter does not.
  SND_CHECK_EQ(t.Ack(t.c->SetValue(lltest::BusVolume("Master", -6.0f), true)), SND_OK);
  f.Steps(400);
  n = meters(m, 16);
  master = sfx = nullptr;
  for (std::uint32_t i = 0; i < n; ++i) {
    if (m[i].busId == HashId("Master") || m[i].busId == 0u) master = &m[i];
    if (m[i].busId == HashId("Sfx")) sfx = &m[i];
  }
  SND_REQUIRE(master != nullptr && sfx != nullptr);
  SND_CHECK_NEAR(master->peak[0] / sfx->peak[0], DbRatio(-6.0), 1e-3);
  SND_CHECK_NEAR(master->rms[0] / sfx->rms[0], DbRatio(-6.0), 1e-2);
  SND_CHECK_NEAR(master->peak[0], f.LastLeft(), 1e-4);
}

// E4: the profiler stays within its byte budget per second (one second of burst at
// most) and counts what it drops; the game is never slowed down.
SND_TEST(IndepLivelinkProfiler, ByteBudgetIsRespected) {
  Fixture f;
  f.Load(IndepBank(), "a.sndb");
  for (int i = 0; i < 8; ++i) f.sys->Play(HashId("Tone"), snd_emitter_handle{0}, nullptr);
  ll::ServerDesc sd;
  constexpr std::uint32_t kBudget = 16000;
  sd.profilerBytesPerSecond = kBudget;
  sd.eventLogHz = 100;
  sd.memoryHz = 50;
  SND_REQUIRE_EQ(f.StartServer(sd), SND_OK);
  Tool t(f);
  SND_REQUIRE(t.Ready());
  t.c->SetProfilerMask(kAllProfiler, 1000);
  auto runFor = [&](double seconds) {
    const auto end = std::chrono::steady_clock::now() + std::chrono::duration<double>(seconds);
    while (std::chrono::steady_clock::now() < end) {
      f.Step();
      std::this_thread::sleep_for(std::chrono::microseconds(2500));
    }
  };
  runFor(1.2);  // warm up: the initial burst is spent
  const std::uint64_t b0 = t.c->Stats().bytesIn;
  const std::uint32_t d0 = f.server->Stats().profilerDropped;
  const auto t0 = std::chrono::steady_clock::now();
  runFor(2.0);
  const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  f.Steps(1);
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  const std::uint64_t bytes = t.c->Stats().bytesIn - b0;
  // Budget over the window, plus the bucket (one second), plus pings / pongs.
  const double bound = kBudget * secs + kBudget + 2000.0;
  SND_CHECK_MSG(static_cast<double>(bytes) <= bound, "%llu bytes in %.2f s (bound %.0f)",
                static_cast<unsigned long long>(bytes), secs, bound);
  SND_CHECK(static_cast<double>(bytes) >= kBudget * secs * 0.5);  // and it is used
  SND_CHECK(f.server->Stats().profilerDropped > d0);
}

// --- recording -----------------------------------------------------------------------------

namespace {

struct Rec {
  std::vector<std::uint8_t> bytes;
  std::vector<float> out;
  std::uint32_t blockSamples = 0;
  std::uint32_t clientErrors = 0;
};

// Records a session in which bank "Two" is registered while recording. With `extras`
// the tool also replaces a chunk and the game unloads "Two" (commands the replay skips).
void RecordWithBankMidway(Rec& rec, bool extras) {
  Fixture f;
  rec.blockSamples = f.sys->BlockFrames() * f.sys->OutputChannels();
  f.Load(IndepBank(), "a.sndb");
  f.Steps(80);  // leaves room for the replaying System to load both banks first
  SND_REQUIRE_EQ(f.StartServer(), SND_OK);
  Tool t(f);
  SND_REQUIRE(t.Ready());
  SND_REQUIRE_EQ(t.Ack(t.c->StartRecording(true)), SND_OK);
  SND_REQUIRE(f.StepUntil([&] { return f.rt->Recording(); }));
  f.sys->Play(HashId("Tone"), snd_emitter_handle{0}, nullptr);
  f.Steps(6);
  BankOpts two;
  two.name = "Two";
  two.extraEvent = "Other";
  const snd_bank_handle hb = f.Load(IndepBank(two), "b.sndb");
  f.sys->Play(HashId("Other"), snd_emitter_handle{0}, nullptr);
  f.Steps(5);
  SND_CHECK_EQ(t.Ack(t.c->SetValue(lltest::BusVolume("Sfx", -6.0f), true)), SND_OK);
  f.Steps(5);
  if (extras) {
    bt::TableBuilder bu(bf::chunk::BUSS);
    std::uint32_t r = bu.AddRow();
    bu.U32(bf::col::BUSS_id, r, HashId("Sfx"));
    bu.U32(bf::col::BUSS_parent, r, 1u);
    bu.U32(bf::col::BUSS_channels, r, 2u);
    bu.F32(bf::col::BUSS_volumeDb, r, -3.0f);
    r = bu.AddRow();
    bu.U32(bf::col::BUSS_id, r, HashId("Master"));
    bu.U32(bf::col::BUSS_parent, r, bf::kNone);
    bu.U32(bf::col::BUSS_channels, r, 2u);
    bu.U32(bf::col::BUSS_flags, r, bf::kBusMaster);
    const std::vector<std::uint8_t> chunk = bu.Serialize();
    SND_CHECK_EQ(t.Ack(t.c->ReplaceChunk(lltest::kBankId, bf::chunk::BUSS, chunk.data(), chunk.size(), true)), SND_OK);
    f.sys->UnloadBank(hb, SND_UNLOAD_STOP_AND_UNLOAD);
    f.Steps(10);
  }
  f.Steps(10);
  SND_REQUIRE_EQ(t.Ack(t.c->StopRecording(true)), SND_OK);
  SND_REQUIRE(f.StepUntil([&] { return !f.rt->Recording(); }));
  std::uint64_t last = 0;
  f.StepUntil([&] {
    const std::uint64_t n = t.c->RecordingBytes();
    const bool stable = n == last && n != 0u && f.server->Stats().recordBytes == n;
    last = n;
    return stable;
  });
  rec.bytes.resize(t.c->RecordingBytes());
  t.c->CopyRecording(rec.bytes.data(), rec.bytes.size());
  rec.out = f.all;
  rec.clientErrors = t.c->Stats().protocolErrors;
  SND_CHECK_EQ(f.server->Stats().recordsDropped, 0u);
}

int Replay(const Rec& rec, bool loadTwo, ll::ReplayInfo* info, std::vector<float>& out) {
  const snd_config cfg = lltest::Config();
  ll::ReplayDesc rd;
  rd.config = &cfg;
  rd.allocator = &snd::base::DefaultAllocator();
  rd.record = rec.bytes.data();
  rd.bytes = rec.bytes.size();
  ll::Replayer* rp = nullptr;
  const int c = ll::Replayer::Create(rd, &rp);
  if (c != SND_OK) return c;
  {
    Fixture f(Hooks::Replay, cfg, rp);
    f.Load(IndepBank(), "a.sndb");
    if (loadTwo) {
      BankOpts two;
      two.name = "Two";
      two.extraEvent = "Other";
      f.Load(IndepBank(two), "b.sndb");
    }
    const ll::ReplayInfo ri = rp->Info();
    if (f.sys->LastBlock().block + 1u > ri.firstBlock) {
      SND_CHECK_MSG(false, "the replaying System is past the first recorded block");
      return SND_ERR_NOT_READY;
    }
    while (f.sys->LastBlock().block + 1u < ri.firstBlock) f.Step();
    rp->Start();
    for (std::uint64_t i = 0; i < rp->Blocks() + 2u; ++i) {
      f.sys->RenderBlock(f.out.data());
      out.insert(out.end(), f.out.begin(), f.out.end());
    }
    SND_CHECK(rp->Finished());
    *info = rp->Info();
  }
  ll::Replayer::Destroy(rp);
  return SND_OK;
}

}  // namespace

// F1 / F2 / F3 / F6 / F8: a bank registered while recording gets its BankMap record
// in the block it was registered (before the arenas of the next block); the
// recording reaches the tool contiguously; a replay with the banks loaded renders
// what the recorded run rendered, twice bit-identically; without bank "Two" the
// replay reports the missing bank.
SND_TEST(IndepLivelinkRecord, BankRegisteredWhileRecordingAndBitIdenticalReplay) {
  Rec rec;
  RecordWithBankMidway(rec, false);
  SND_REQUIRE(!rec.bytes.empty());
  SND_CHECK_EQ(rec.clientErrors, 0u);
  ll::RecordReader rr(rec.bytes.data(), rec.bytes.size());
  ll::RecordItem it;
  std::uint32_t index = 0, maps = 0;
  std::uint64_t registerBlock = ~0ull, lastArenaBlock = 0, mapAfterBlock = ~0ull;
  bool arenaAfterMapOk = true, firstIsLod = false, nextChecked = false;
  while (rr.Next(&it)) {
    if (index++ == 0u) firstIsLod = it.kind == ll::RecordKind::Lod;
    if (it.kind == ll::RecordKind::Arena) {
      if (mapAfterBlock != ~0ull && !nextChecked) {  // first arena after the BankMap of "Two"
        nextChecked = true;
        arenaAfterMapOk = it.arena.appliedBlock > registerBlock;
      }
      lastArenaBlock = it.arena.appliedBlock;
      snd::cmd::ForEachCommand(it.normal, it.normal + it.sections.normalBytes,
                               [&](const snd::cmd::CommandHeader& h, const std::uint8_t*) {
                                 if (h.type == static_cast<std::uint16_t>(snd::cmd::CommandType::BankRegister)) {
                                   registerBlock = it.arena.appliedBlock;
                                 }
                               });
    }
    if (it.kind == ll::RecordKind::BankMap) {
      ++maps;
      if (it.bankMap.bankId == HashId("Two")) {
        mapAfterBlock = lastArenaBlock;
        SND_CHECK_MSG(lastArenaBlock == registerBlock, "BankMap of Two after block %llu, registered in block %llu",
                      static_cast<unsigned long long>(lastArenaBlock), static_cast<unsigned long long>(registerBlock));
      }
    }
  }
  SND_CHECK(rr.Error() == ll::RecordError::None);
  SND_CHECK(firstIsLod);
  SND_CHECK_EQ(maps, 2u);
  SND_CHECK(registerBlock != ~0ull);
  SND_CHECK(arenaAfterMapOk);
  ll::ReplayInfo a{}, b{}, c{};
  std::vector<float> r1, r2, r3;
  SND_REQUIRE_EQ(Replay(rec, true, &a, r1), SND_OK);
  SND_REQUIRE_EQ(Replay(rec, true, &b, r2), SND_OK);
  SND_REQUIRE_EQ(r1.size(), r2.size());
  SND_CHECK(std::memcmp(r1.data(), r2.data(), r1.size() * sizeof(float)) == 0);
  SND_CHECK_EQ(a.bankMismatches, 0u);
  // The BankRegister of "Two" is in the recording and is left out.
  SND_CHECK_EQ(a.skippedCommands, 1u);
  const std::size_t from = static_cast<std::size_t>(a.firstBlock) * rec.blockSamples;
  const std::size_t n = static_cast<std::size_t>(a.lastBlock - a.firstBlock + 1u) * rec.blockSamples;
  SND_REQUIRE(rec.out.size() >= from + n && r1.size() >= n);
  std::size_t diff = 0;
  for (std::size_t i = 0; i < n; ++i) diff += std::memcmp(&rec.out[from + i], &r1[i], sizeof(float)) != 0 ? 1u : 0u;
  SND_CHECK_MSG(diff == 0u, "%zu of %zu samples differ from the recorded run", diff, n);
  SND_REQUIRE_EQ(Replay(rec, false, &c, r3), SND_OK);
  SND_CHECK_EQ(c.bankMismatches, 1u);
}

// F4: the replay leaves out exactly the commands it cannot replay and counts them.
SND_TEST(IndepLivelinkRecord, SkippedCommandsAreCountedExactly) {
  Rec rec;
  RecordWithBankMidway(rec, true);
  SND_REQUIRE(!rec.bytes.empty());
  std::uint32_t total = 0, skip = 0, replace = 0, reg = 0, unload = 0;
  ll::RecordReader rr(rec.bytes.data(), rec.bytes.size());
  ll::RecordItem it;
  while (rr.Next(&it)) {
    if (it.kind != ll::RecordKind::Arena) continue;
    auto count = [&](const snd::cmd::CommandHeader& h, const std::uint8_t*) {
      using CT = snd::cmd::CommandType;
      ++total;
      switch (static_cast<CT>(h.type)) {
        case CT::BankRegister: ++reg; ++skip; break;
        case CT::BankUnload: ++unload; ++skip; break;
        case CT::LiveReplaceChunk: ++replace; ++skip; break;
        case CT::PlaySynth: case CT::PlayInput: case CT::PlayUserBuffer: case CT::CreateUserBuffer:
        case CT::DestroyUserBuffer: ++skip; break;
        default: break;
      }
    };
    snd::cmd::ForEachCommand(it.normal, it.normal + it.sections.normalBytes, count);
    snd::cmd::ForEachCommand(it.reserve, it.reserve + it.sections.reserveBytes, count);
  }
  SND_CHECK_EQ(reg, 1u);
  SND_CHECK_EQ(unload, 1u);
  SND_CHECK_EQ(replace, 1u);
  ll::ReplayDesc rd;
  const snd_config cfg = lltest::Config();
  rd.config = &cfg;
  rd.allocator = &snd::base::DefaultAllocator();
  rd.record = rec.bytes.data();
  rd.bytes = rec.bytes.size();
  ll::Replayer* rp = nullptr;
  SND_REQUIRE_EQ(ll::Replayer::Create(rd, &rp), SND_OK);
  SND_CHECK_EQ(rp->Info().skippedCommands, skip);
  SND_CHECK_EQ(rp->Info().commands, total - skip);
  ll::Replayer::Destroy(rp);
}

// E3 / E8: BlockTiming arrives at timingHz and describes the blocks; the event log
// shows the Play command (0x8000 | Play, event id, playback slot) and the end of that
// playback (kLogPlaybackEnded, slot, reason COMPLETED).
SND_TEST(IndepLivelinkProfiler, TimingRateAndEventLogOfAPlayback) {
  Fixture f;
  f.Load(IndepBank(), "a.sndb");
  ll::ServerDesc sd;
  sd.timingHz = 30;
  sd.eventLogHz = 50;
  SND_REQUIRE_EQ(f.StartServer(sd), SND_OK);
  Tool t(f);
  SND_REQUIRE(t.Ready());
  t.c->SetProfilerMask(ll::kProfTiming | ll::kProfEventLog, 30);
  auto runFor = [&](double seconds) {
    const auto end = std::chrono::steady_clock::now() + std::chrono::duration<double>(seconds);
    while (std::chrono::steady_clock::now() < end) {
      f.Step();
      std::this_thread::sleep_for(std::chrono::microseconds(3000));
    }
  };
  runFor(0.5);
  const std::uint64_t r0 = t.c->Received(ll::MessageType::BlockTiming);
  const auto t0 = std::chrono::steady_clock::now();
  const snd_playback_handle blip = f.sys->Play(HashId("Blip"), snd_emitter_handle{0}, nullptr);
  runFor(1.5);
  const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  const double rate = static_cast<double>(t.c->Received(ll::MessageType::BlockTiming) - r0) / secs;
  SND_CHECK_MSG(rate > 20.0 && rate < 40.0, "BlockTiming at %.1f Hz (30 expected)", rate);
  ll::TimingSample ts[256];
  const std::uint32_t nt = t.c->CopyTimings(ts, 256);
  SND_REQUIRE(nt > 2u);
  for (std::uint32_t i = 1; i < nt; ++i) {
    SND_CHECK(ts[i].timing.blockIndex > ts[i - 1].timing.blockIndex);
    SND_CHECK(ts[i].timing.budgetMicroseconds > 0.0f && ts[i].timing.budgetMicroseconds <= 5334.0f);
    SND_CHECK(ts[i].timing.renderMicroseconds >= 0.0f);
  }
  ll::LogSample ls[512];
  const std::uint32_t nl = t.c->CopyEventLog(ls, 512);
  const std::uint32_t slot = static_cast<std::uint32_t>(blip.value & 0xFFFFFFFFu);
  bool play = false, ended = false;
  for (std::uint32_t i = 0; i < nl; ++i) {
    const ll::LogEntry& e = ls[i].entry;
    if (e.kind == (ll::kLogCommandBit | static_cast<std::uint32_t>(snd::cmd::CommandType::Play)) &&
        e.id == HashId("Blip") && e.handleSlot == slot) {
      play = true;
    }
    if (e.kind == ll::kLogPlaybackEnded && e.handleSlot == slot) {
      ended = play;  // after the Play entry
      SND_CHECK_EQ(e.detail >> 16, SND_END_COMPLETED);
    }
  }
  SND_CHECK(play);
  SND_CHECK(ended);
}
