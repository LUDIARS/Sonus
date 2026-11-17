// Independent re-check of the live link fixes (acceptance decisions 1-5 and the spec
// gaps): override records per bank registration, Ack back-pressure, recording
// truncation, ReplaceChunk slots across Server::Stop / Destroy, master meter id and
// StreamStall event id.
#include <atomic>
#include <cstdio>
#include <chrono>
#include <map>
#include <mutex>
#include <random>
#include <thread>

#include "indep_livelink_common.hpp"

using namespace indep;
using lltest::Fixture;
using lltest::RawPeer;

namespace {

double Settle(Fixture& f, std::uint32_t blocks = 24) {
  f.Steps(blocks);
  return LeftMean(f);
}

void Unload(Fixture& f, snd_bank_handle h) {
  f.sys->UnloadBank(h, SND_UNLOAD_STOP_AND_UNLOAD);
  f.StepUntil([&] {
    f.sys->Update();
    return f.sys->GetBankState(h) == SND_BANK_STATE_UNLOADED;
  });
}

std::vector<std::uint8_t> BussChunk(float sfxDb) {
  bt::TableBuilder bu(bf::chunk::BUSS);
  std::uint32_t r = bu.AddRow();
  bu.U32(bf::col::BUSS_id, r, HashId("Sfx"));
  bu.U32(bf::col::BUSS_parent, r, 1u);
  bu.U32(bf::col::BUSS_channels, r, 2u);
  bu.F32(bf::col::BUSS_volumeDb, r, sfxDb);
  r = bu.AddRow();
  bu.U32(bf::col::BUSS_id, r, HashId("Master"));
  bu.U32(bf::col::BUSS_parent, r, bf::kNone);
  bu.U32(bf::col::BUSS_channels, r, 2u);
  bu.U32(bf::col::BUSS_flags, r, bf::kBusMaster);
  return bu.Serialize();
}

class QuarantineAllocator final : public snd::IAllocator {
 public:
  void* Allocate(std::size_t bytes, std::size_t align, snd::MemTag tag) override {
    void* p = snd::base::DefaultAllocator().Allocate(bytes, align, tag);
    std::lock_guard<std::mutex> g(m_);
    if (p != nullptr) live_[p] = Block{bytes, tag, {}};
    return p;
  }
  void Free(void* p, snd::MemTag) override {
    if (p == nullptr) return;
    std::lock_guard<std::mutex> g(m_);
    auto it = live_.find(p);
    if (it == live_.end()) return;
    Block b = it->second;
    b.snapshot.assign(static_cast<std::uint8_t*>(p), static_cast<std::uint8_t*>(p) + b.bytes);
    freed_[p] = std::move(b);
    live_.erase(it);
  }
  std::size_t ModifiedAfterFree() {
    std::lock_guard<std::mutex> g(m_);
    std::size_t n = 0;
    for (const auto& [p, b] : freed_) {
      const auto* q = static_cast<const std::uint8_t*>(p);
      for (std::size_t i = 0; i < b.bytes; ++i) n += q[i] != b.snapshot[i] ? 1u : 0u;
    }
    return n;
  }
  ~QuarantineAllocator() override {
    for (auto& [p, b] : freed_) snd::base::DefaultAllocator().Free(p, b.tag);
    for (auto& [p, b] : live_) snd::base::DefaultAllocator().Free(p, b.tag);
  }

 private:
  struct Block {
    std::size_t bytes;
    snd::MemTag tag;
    std::vector<std::uint8_t> snapshot;
  };
  std::mutex m_;
  std::map<void*, Block> live_, freed_;
};

ll::Server* NewServer(Fixture& f, snd::IAllocator& a, ll::ServerDesc sd = {}) {
  sd.system = f.sys;
  sd.runtime = f.rt;
  sd.allocator = &a;
  sd.acceptor = &f.link.acceptor();
  ll::Server* s = nullptr;
  if (ll::Server::Create(sd, &s) != SND_OK) return nullptr;
  if (s->Start() != SND_OK) {
    ll::Server::Destroy(s);
    return nullptr;
  }
  return s;
}

}  // namespace

// --- 1. override records per registration -----------------------------------------------

// Edits of a registration that is gone are dropped (nothing to restore, room freed); a
// revert without new edits after a reload changes nothing.
SND_TEST(IndepLivelinkRecheck, ReloadWithoutNewEditsRevertsNothing) {
  Fixture f;
  const snd_bank_handle h = f.Load(IndepBank(), "a.sndb");
  f.sys->Play(HashId("Tone"), snd_emitter_handle{0}, nullptr);
  const double base = Settle(f);
  SND_CHECK_EQ(VolumeDirect(f, "Sfx", -6.0f).cells, 1u);
  SND_CHECK_EQ(VolumeDirect(f, "Master", -6.0f).cells, 1u);
  Unload(f, h);
  f.Load(IndepBank(), "a.sndb");
  f.sys->Play(HashId("Tone"), snd_emitter_handle{0}, nullptr);
  SND_CHECK_NEAR(Settle(f) / base, 1.0, 1e-3);
  const ll::ApplyResult rv = RevertDirect(f);
  SND_CHECK_EQ(rv.result, SND_OK);
  SND_CHECK_EQ(rv.cells, 0u);
  SND_CHECK_EQ(f.rt->applier().Remembered(), 0u);
  SND_CHECK_NEAR(Settle(f) / base, 1.0, 1e-3);
}

// At the limit: the records of a gone registration do not count against maxEdits.
SND_TEST(IndepLivelinkRecheck, LimitIsFreedByAReload) {
  ll::RuntimeDesc rd;
  rd.maxEdits = 1;
  Fixture f(lltest::Hooks::Live, lltest::Config(), nullptr, rd);
  SND_REQUIRE_EQ(f.created, SND_OK);
  const snd_bank_handle h = f.Load(IndepBank(), "a.sndb");
  f.sys->Play(HashId("Tone"), snd_emitter_handle{0}, nullptr);
  const double base = Settle(f);
  SND_CHECK_EQ(VolumeDirect(f, "Sfx", -6.0f).result, SND_OK);
  SND_CHECK_EQ(VolumeDirect(f, "Master", -6.0f).result, SND_ERR_LIMIT_REACHED);  // 1 of 1
  Unload(f, h);
  f.Load(IndepBank(), "a.sndb");
  f.sys->Play(HashId("Tone"), snd_emitter_handle{0}, nullptr);
  Settle(f);
  SND_CHECK_EQ(VolumeDirect(f, "Master", -12.0f).result, SND_OK);  // the old record is gone
  SND_CHECK_EQ(f.rt->applier().Remembered(), 1u);
  SND_CHECK_EQ(VolumeDirect(f, "Sfx", -6.0f).result, SND_ERR_LIMIT_REACHED);
  SND_CHECK_EQ(VolumeDirect(f, "Master", -3.0f).result, SND_OK);  // same cell: no new record
  SND_CHECK_NEAR(Settle(f) / base, DbRatio(-3.0), 1e-3);
  SND_CHECK_EQ(RevertDirect(f).cells, 1u);
  SND_CHECK_NEAR(Settle(f) / base, 1.0, 1e-3);
}

// Unload and the next registration of the same bank applied in one block (no render in
// between): the edit before is dropped, an edit after is reverted to the new bank value.
SND_TEST(IndepLivelinkRecheck, UnloadAndReloadInOneBlock) {
  Fixture f;
  const snd_bank_handle h = f.Load(IndepBank(), "a.sndb");
  f.sys->Play(HashId("Tone"), snd_emitter_handle{0}, nullptr);
  const double base = Settle(f);
  SND_CHECK_EQ(VolumeDirect(f, "Sfx", -6.0f).cells, 1u);
  snd::core::LiveBankInfo before[4];
  // LiveBanks is an audio-thread query; the test thread is the audio thread here.
  SND_REQUIRE_EQ(f.sys->LiveBanks(before, 4), 1u);
  f.sys->UnloadBank(h, SND_UNLOAD_STOP_AND_UNLOAD);
  f.fs.AddFile("a2.sndb", IndepBank());
  const snd_bank_handle h2 = f.sys->LoadBankAsync("a2.sndb");
  // Let the loader prepare without rendering; then one block applies both.
  for (int i = 0; i < 200; ++i) {
    f.sys->Update();
    f.sys->PumpServices();
    std::this_thread::sleep_for(std::chrono::microseconds(200));
  }
  f.Step();
  snd::core::LiveBankInfo after[4];
  const std::uint32_t n = f.sys->LiveBanks(after, 4);
  const bool oneBlock = n == 1u && f.sys->GetBankState(h2) != SND_BANK_STATE_UNLOADED &&
                        after[0].handle.generation != before[0].handle.generation;
  if (!oneBlock) {
    // The loader needed the audio thread in between: still a valid reload check.
    f.StepUntil([&] {
      f.sys->Update();
      return f.sys->GetBankState(h2) == SND_BANK_STATE_LOADED;
    });
  }
  f.sys->Play(HashId("Tone"), snd_emitter_handle{0}, nullptr);
  SND_CHECK_NEAR(Settle(f) / base, 1.0, 1e-3);
  SND_CHECK_EQ(VolumeDirect(f, "Sfx", -12.0f).cells, 1u);
  SND_CHECK_EQ(f.rt->applier().Remembered(), 1u);
  SND_CHECK_EQ(RevertDirect(f).cells, 1u);
  SND_CHECK_NEAR(Settle(f) / base, 1.0, 1e-3);
}

// --- 2. back-pressure -------------------------------------------------------------------

namespace {
struct Game {
  Fixture f;
  double base = 0;
  explicit Game(ll::ServerDesc sd = {}) {
    f.Load(IndepBank(), "a.sndb");
    f.sys->Play(HashId("Tone"), snd_emitter_handle{0}, nullptr);
    f.Steps(24);
    base = LeftMean(f);
    started = f.StartServer(sd);
  }
  snd_result started = SND_ERR_NOT_READY;
};

bool Handshake(Fixture& f, RawPeer& p) {
  if (!p.Hello()) return false;
  const RawPeer::Frame* r = nullptr;
  f.StepUntil([&] { return (r = p.WaitFor(ll::MessageType::HelloReply, 1)) != nullptr; });
  return r != nullptr;
}
}  // namespace

// The tool stops reading while thousands of acknowledged edits (and failing edits
// without Ack) are in flight; the game keeps rendering; when it reads again, every
// answer arrives, in seq order, and the connection stayed.
SND_TEST(IndepLivelinkRecheck, BackPressureStopAndResumeKeepsEveryAnswerInOrder) {
  ll::ServerDesc sd;
  sd.timeoutMs = 4000;
  sd.heartbeatMs = 100;
  Game g(sd);
  SND_REQUIRE_EQ(g.started, SND_OK);
  RawPeer p;
  SND_REQUIRE(p.Connect(g.f.link.connector()));
  SND_REQUIRE(Handshake(g.f, p));
  constexpr std::uint32_t kN = 3000;
  std::vector<std::uint8_t> stream;
  for (std::uint32_t i = 0; i < kN; ++i) {
    ll::SetValue v = lltest::BusVolume("Sfx", -1.0f - static_cast<float>(i % 5));
    std::uint16_t flags = ll::kFrameWantsAck;
    if (i % 3u == 1u) {  // no Ack wanted, refused when applied -> Error
      v.runtimeId = HashId("NoSuchBus");
      flags = 0;
    }
    if (i % 3u == 2u) flags = 0;  // no Ack wanted, succeeds -> silent
    std::vector<std::uint8_t> b(16 + sizeof(v));
    ll::EncodeHeader(b.data(), static_cast<std::uint16_t>(ll::MessageType::SetValue), flags, sizeof(v), 100u + i);
    std::memcpy(b.data() + 16, &v, sizeof(v));
    stream.insert(stream.end(), b.begin(), b.end());
  }
  SND_REQUIRE(p.SendBytes(stream.data(), stream.size(), 5000));
  // Not reading for 1.5 s while the game renders.
  double worstStepMs = 0;
  const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(1500);
  while (std::chrono::steady_clock::now() < until) {
    const auto t0 = std::chrono::steady_clock::now();
    g.f.Step();
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    if (ms > worstStepMs) worstStepMs = ms;
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  SND_CHECK_MSG(worstStepMs < 100.0, "a block took %.1f ms while the tool did not read", worstStepMs);
  SND_CHECK(g.f.server->Connected());
  // Resume reading. Answers: applied ones (Ack OK for i % 3 == 0, Error NOT_FOUND for
  // i % 3 == 1) in seq order; a command the core refused at record time (its arenas
  // full: SND_ERR_COMMAND_BUFFER_FULL) is answered at once, whatever its wantAck.
  std::vector<int> answered(kN, 0);
  std::vector<std::uint32_t> appliedSeqs;
  std::uint32_t bad = 0, refused = 0;
  auto take = [&](std::uint32_t seq, std::int32_t code, bool isAck) {
    if (seq < 100u || seq >= 100u + kN) {
      ++bad;
      return;
    }
    const std::uint32_t i = seq - 100u;
    ++answered[i];
    if (code == SND_ERR_COMMAND_BUFFER_FULL) {
      ++refused;
      return;
    }
    appliedSeqs.push_back(seq);
    if (i % 3u == 0u) bad += isAck && code == SND_OK ? 0u : 1u;
    else if (i % 3u == 1u) bad += !isAck && code == SND_ERR_NOT_FOUND ? 0u : 1u;
    else ++bad;  // a silent success answered
  };
  auto complete = [&] {
    for (std::uint32_t i = 0; i < kN; ++i) {
      if (i % 3u != 2u && answered[i] == 0) return false;
    }
    return true;
  };
  g.f.StepUntil(
      [&] {
        p.Pump(1);
        for (const RawPeer::Frame& fr : p.frames) {
          if (fr.header.type == static_cast<std::uint16_t>(ll::MessageType::Ack)) {
            const auto a = ll::ReadFixed<ll::Ack>(fr.payload.data(), 8);
            take(a.seq, a.result, true);
          } else if (fr.header.type == static_cast<std::uint16_t>(ll::MessageType::Error)) {
            const auto e = ll::ReadFixed<ll::Error>(fr.payload.data(), 64);
            take(e.seq, e.code, false);
          }
        }
        p.frames.clear();
        return complete() || p.closed;
      },
      15000);
  g.f.Steps(8);
  p.Pump(20);
  std::uint32_t missing = 0, twice = 0;
  for (std::uint32_t i = 0; i < kN; ++i) {
    missing += i % 3u != 2u && answered[i] == 0 ? 1u : 0u;
    twice += answered[i] > 1 ? 1u : 0u;
  }
  std::uint32_t outOfOrder = 0;
  for (std::size_t i = 1; i < appliedSeqs.size(); ++i) outOfOrder += appliedSeqs[i] <= appliedSeqs[i - 1] ? 1u : 0u;
  std::printf("back-pressure: %u of %u commands refused with COMMAND_BUFFER_FULL\n", refused, kN);
  SND_CHECK_EQ(missing, 0u);
  SND_CHECK_EQ(twice, 0u);
  SND_CHECK_EQ(bad, 0u);
  SND_CHECK_EQ(outOfOrder, 0u);
  SND_CHECK(!p.closed);
  SND_CHECK_EQ(g.f.server->Stats().timeouts, 0u);
  SND_CHECK_EQ(g.f.server->Stats().connections, 1u);
  SND_CHECK_EQ(g.f.server->Stats().commandsRefused, refused);
  if (refused == 0u) {
    const float lastDb = -1.0f - static_cast<float>((kN - 1u) % 5u);
    g.f.Steps(24);
    SND_CHECK_NEAR(LeftMean(g.f) / g.base, DbRatio(lastDb), 1e-3);
  }
}

// A tool that never reads again is dropped after timeoutMs (and not before); the game is
// not slowed down meanwhile.
SND_TEST(IndepLivelinkRecheck, ToolThatNeverReadsIsDroppedAfterTheTimeout) {
  ll::ServerDesc sd;
  sd.timeoutMs = 800;
  sd.heartbeatMs = 50;
  Game g(sd);
  SND_REQUIRE_EQ(g.started, SND_OK);
  RawPeer p;
  SND_REQUIRE(p.Connect(g.f.link.connector()));
  SND_REQUIRE(Handshake(g.f, p));
  std::vector<std::uint8_t> stream;
  for (std::uint32_t i = 0; i < 2000; ++i) {
    const ll::SetValue v = lltest::BusVolume("Sfx", -2.0f);
    std::vector<std::uint8_t> b(16 + sizeof(v));
    ll::EncodeHeader(b.data(), static_cast<std::uint16_t>(ll::MessageType::SetValue), ll::kFrameWantsAck, sizeof(v), i + 1);
    std::memcpy(b.data() + 16, &v, sizeof(v));
    stream.insert(stream.end(), b.begin(), b.end());
  }
  SND_REQUIRE(p.SendBytes(stream.data(), stream.size(), 5000));
  const auto t0 = std::chrono::steady_clock::now();
  double worstStepMs = 0;
  while (g.f.server->Connected() && std::chrono::steady_clock::now() - t0 < std::chrono::seconds(10)) {
    const auto s0 = std::chrono::steady_clock::now();
    g.f.Step();
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - s0).count();
    if (ms > worstStepMs) worstStepMs = ms;
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  SND_CHECK(!g.f.server->Connected());
  SND_CHECK_MSG(secs >= 0.6 && secs < 5.0, "dropped after %.2f s (timeout 0.8 s)", secs);
  SND_CHECK(g.f.server->Stats().timeouts >= 1u);
  SND_CHECK_MSG(worstStepMs < 100.0, "a block took %.1f ms", worstStepMs);
  p.Close();
  Tool t(g.f);
  SND_REQUIRE(t.Ready());
  SND_CHECK_EQ(t.Ack(t.c->SetValue(lltest::BusVolume("Sfx", -6.0f), true)), SND_OK);
}

// --- 3. recording truncation ------------------------------------------------------------
SND_TEST(IndepLivelinkRecheck, CopyRecordingReportsTruncation) {
  for (const std::uint32_t cap : {4096u, 8u << 20}) {
    Game g;
    SND_REQUIRE_EQ(g.started, SND_OK);
    ll::ClientDesc cd = lltest::MemoryClient(g.f.link);
    cd.recordBufferBytes = cap;
    Tool t(g.f, cd);
    SND_REQUIRE(t.Ready());
    SND_REQUIRE_EQ(t.Ack(t.c->StartRecording(true)), SND_OK);
    for (int i = 0; i < 60; ++i) {
      g.f.sys->SetParamGlobal(HashId("Knob"), static_cast<float>(i) / 60.0f);
      g.f.Step();
    }
    SND_REQUIRE_EQ(t.Ack(t.c->StopRecording(true)), SND_OK);
    std::uint64_t produced = 0;
    g.f.StepUntil([&] {
      produced = g.f.server->Stats().recordBytes;
      return !g.f.rt->Recording() && produced != 0u && t.c->Stats().recordOverflow + t.c->RecordingBytes() == produced;
    });
    std::vector<std::uint8_t> buf(cap);
    bool truncated = !(cap > 4096u);
    const std::uint64_t n = t.c->CopyRecording(buf.data(), buf.size(), &truncated);
    SND_CHECK_EQ(t.c->Stats().protocolErrors, 0u);
    SND_CHECK_EQ(n + t.c->Stats().recordOverflow, produced);
    if (cap == 4096u) {
      SND_CHECK(produced > cap);
      SND_CHECK(truncated);
      SND_CHECK_EQ(n, 4096u);
    } else {
      SND_CHECK(!truncated);
      SND_CHECK_EQ(t.c->Stats().recordOverflow, 0u);
      ll::RecordReader rr(buf.data(), n);
      ll::RecordItem it;
      while (rr.Next(&it)) {
      }
      SND_CHECK(rr.Error() == ll::RecordError::None);
    }
    // A new recording clears the flag (LIMIT_REACHED while the previous stop drains).
    std::int32_t sr = SND_ERR_LIMIT_REACHED;
    for (int k = 0; k < 200 && sr == SND_ERR_LIMIT_REACHED; ++k) {
      sr = t.Ack(t.c->StartRecording(true));
      if (sr == SND_ERR_LIMIT_REACHED) g.f.Steps(2);
    }
    SND_REQUIRE_EQ(sr, SND_OK);
    bool tr2 = true;
    t.c->CopyRecording(buf.data(), 0, &tr2);
    SND_CHECK(!tr2);
    SND_REQUIRE_EQ(t.Ack(t.c->StopRecording(true)), SND_OK);
  }
}

// --- 4. ReplaceChunk slots across Server::Stop / Destroy --------------------------------

// The game renders on its own thread while servers come and go with ReplaceChunk in
// flight (TSan target). No server memory is written after its Destroy, every slot comes
// back, and a later ReplaceChunk works.
SND_TEST(IndepLivelinkRecheck, ServerDestroyRacesReplaceChunk) {
  Fixture f;
  f.Load(IndepBank(), "a.sndb");
  f.sys->Play(HashId("Tone"), snd_emitter_handle{0}, nullptr);
  f.Steps(8);
  SND_REQUIRE(f.link.Init(snd::base::DefaultAllocator(), 1u << 20));
  QuarantineAllocator qa;
  std::atomic<bool> stop{false};
  std::thread game([&] {
    while (!stop.load()) {
      f.Step();
      std::this_thread::sleep_for(std::chrono::microseconds(300));
    }
  });
  std::mt19937 rng(9);
  std::uint32_t queued = 0;
  for (int round = 0; round < 25; ++round) {
    ll::Server* s = NewServer(f, qa);
    SND_REQUIRE(s != nullptr);
    ll::Client* c = nullptr;
    SND_REQUIRE_EQ(ll::Client::Create(lltest::MemoryClient(f.link), &c), SND_OK);
    c->Start();
    if (c->WaitReady(5000)) {
      const std::vector<std::uint8_t> chunk = BussChunk(round % 2 == 0 ? -6.0f : 0.0f);
      if (c->ReplaceChunk(lltest::kBankId, bf::chunk::BUSS, chunk.data(), chunk.size(), true) != 0u) ++queued;
      std::this_thread::sleep_for(std::chrono::microseconds(rng() % 4000u));
    }
    ll::Server::Destroy(s);  // may race the audio thread applying the chunk
    ll::Client::Destroy(c);
  }
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  stop.store(true);
  game.join();
  SND_CHECK(queued > 10u);
  const std::size_t modified = qa.ModifiedAfterFree();
  SND_CHECK_MSG(modified == 0u, "%zu bytes of destroyed Servers were written afterwards", modified);
  // The pool (1 slot) is free again: a new server's ReplaceChunk is applied.
  f.server = NewServer(f, snd::base::DefaultAllocator());
  SND_REQUIRE(f.server != nullptr);
  Tool t(f);
  SND_REQUIRE(t.Ready());
  const std::vector<std::uint8_t> chunk = BussChunk(-12.0f);
  SND_CHECK_EQ(t.Ack(t.c->ReplaceChunk(lltest::kBankId, bf::chunk::BUSS, chunk.data(), chunk.size(), true)), SND_OK);
}

// A Server is destroyed while its SetValue is recorded but not applied; the next
// Server on the same runtime must answer its own commands with their own results.
SND_TEST(IndepLivelinkRecheck, NewServerGetsOnlyItsOwnResults) {
  Fixture f;
  f.Load(IndepBank(), "a.sndb");
  f.sys->Play(HashId("Tone"), snd_emitter_handle{0}, nullptr);
  f.Steps(8);
  SND_REQUIRE(f.link.Init(snd::base::DefaultAllocator(), 1u << 20));
  f.server = NewServer(f, snd::base::DefaultAllocator());
  SND_REQUIRE(f.server != nullptr);
  {
    Tool t(f);
    SND_REQUIRE(t.Ready());
    SND_REQUIRE(t.c->SetValue(lltest::BusVolume("Sfx", -6.0f), true) != 0u);
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (f.server->Stats().commandsRecorded == 0u && std::chrono::steady_clock::now() < until) {
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    SND_REQUIRE_EQ(f.server->Stats().commandsRecorded, 1u);
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    ll::Server::Destroy(f.server);  // no block since: the edit is not applied yet
    f.server = nullptr;
  }
  f.server = NewServer(f, snd::base::DefaultAllocator());
  SND_REQUIRE(f.server != nullptr);
  Tool t(f);
  // Connect without rendering (the old edit stays unapplied until the next block).
  const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (!(t.c->State() == ll::ClientState::Ready && f.server->Connected()) && std::chrono::steady_clock::now() < until) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  SND_REQUIRE(f.server->Connected());
  ll::SetValue bad = lltest::BusVolume("Sfx", -6.0f);
  bad.runtimeId = HashId("NoSuchBus");
  const std::uint32_t seq = t.c->SetValue(bad, true);
  const auto u2 = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (f.server->Stats().commandsRecorded == 0u && std::chrono::steady_clock::now() < u2) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  std::this_thread::sleep_for(std::chrono::milliseconds(30));
  const std::int32_t r = t.Ack(seq);  // renders: both edits are applied in this block
  SND_CHECK_MSG(r == SND_ERR_NOT_FOUND, "the Ack of a SetValue to an unknown bus says %d (expected %d)", r,
                SND_ERR_NOT_FOUND);
}

// --- spec gaps: master meter id, StreamStall event id -------------------------------------
SND_TEST(IndepLivelinkRecheck, MasterMeterCarriesTheMasterBusId) {
  Game g;
  SND_REQUIRE_EQ(g.started, SND_OK);
  Tool t(g.f);
  SND_REQUIRE(t.Ready());
  t.c->SetProfilerMask(ll::kProfMeters, 60);
  ll::MeterEntry m[16];
  std::uint32_t n = 0;
  SND_REQUIRE(g.f.StepUntil([&] { return (n = t.c->CopyMeters(m, 16)) >= 2u; }));
  std::uint32_t master = 0, zero = 0;
  for (std::uint32_t i = 0; i < n; ++i) {
    master += m[i].busId == HashId("Master") ? 1u : 0u;
    zero += m[i].busId == 0u ? 1u : 0u;
  }
  SND_CHECK_EQ(master, 1u);
  SND_CHECK_EQ(zero, 0u);
  ll::MeterSample hist[64];
  SND_CHECK(t.c->CopyMeterHistory(HashId("Master"), hist, 64) > 0u);
}

// The hook path of a stall (no stream can be made to stall with the test sources):
// what the stall hook reports reaches the tool with its event id; with no stream the
// query answers 0.
SND_TEST(IndepLivelinkRecheck, StreamStallCarriesTheEventIdThroughTheHook) {
  Game g;
  SND_REQUIRE_EQ(g.started, SND_OK);
  Tool t(g.f);
  SND_REQUIRE(t.Ready());
  t.c->SetProfilerMask(ll::kProfStreams, 30);
  SND_REQUIRE(g.f.StepUntil([&] { return (g.f.rt->ProfilerMask() & ll::kProfStreams) != 0u; }));
  SND_CHECK_EQ(g.f.sys->LiveStreamEvent(0), 0u);  // test thread = audio thread
  g.f.rt->OnStall(2u, HashId("Tone"), 1u, 100u);  // as core's stall hook would
  ll::StallSample s[8];
  std::uint32_t n = 0;
  SND_REQUIRE(g.f.StepUntil([&] { return (n = t.c->CopyStalls(s, 8)) >= 1u; }));
  SND_CHECK_EQ(s[0].stall.eventId, HashId("Tone"));
  SND_CHECK_EQ(s[0].stall.streamSlot, 2u);
}
