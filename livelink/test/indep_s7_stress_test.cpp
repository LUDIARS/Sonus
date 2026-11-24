// Independent tests, protocol 1.1 (组 L, indep_s7): concurrency (H4, F9, L1 under threads).
// The game renders on the main thread, a tool thread sends every 1.1 request and waits for
// its answers, a UI thread reads the client's update numbers and Copy*Since windows.
// Repeat under TSan and Release: ctest --repeat until-fail:20 -R livelink_indep_s7_stress
#include <atomic>
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
using lltest::Fixture;
using lltest::Hooks;
constexpr snd::Id kLive = lltest::kBankId;

}  // namespace

SND_TEST(IndepS7Stress, ToolAndUiThreadsAgainstARenderingGame) {
  FakeEditor ed;
  ll::RuntimeDesc rd;
  rd.structEditor = &ed;
  rd.maxAuditions = 4;
  Fixture f(Hooks::Live, ReserveConfig(2, 2, 1u << 20), nullptr, rd);
  f.Load(IndepBank(), "a.sndb");
  ll::ServerDesc sd;
  sd.timingHz = 1000;
  SND_REQUIRE_EQ(f.StartServer(sd), SND_OK);
  ll::ClientDesc d = lltest::MemoryClient(f.link);
  d.autoReconnect = false;
  d.historySeconds = 1;
  ll::Client* c = nullptr;
  SND_REQUIRE_EQ(ll::Client::Create(d, &c), SND_OK);
  SND_REQUIRE_EQ(c->Start(), SND_OK);
  SND_REQUIRE(f.StepUntil([&] { return c->State() == ll::ClientState::Ready && f.server->Connected(); }));
  c->SetProfilerMask(ll::ProfilerMaskOf(1), 120);
  f.sys->Play(HashId("Tone"), snd_emitter_handle{0}, nullptr);

  std::atomic<bool> toolDone{false}, stop{false};
  std::atomic<std::uint32_t> lostAcks{0}, badResults{0}, cellMismatch{0}, uiViolations{0}, uiReads{0};
  std::atomic<std::uint32_t> addsOk{0}, removesOk{0}, refused{0};
  const std::uint32_t rounds = 150;

  std::thread tool([&] {
    auto ack = [&](std::uint32_t seq, std::int32_t* r) {
      if (seq == 0u) return false;
      if (!c->WaitAck(seq, 20000, r)) {
        lostAcks.fetch_add(1);
        return false;
      }
      return true;
    };
    const std::uint64_t args[2] = {HashId("Master"), 2};
    for (std::uint32_t i = 0; i < rounds; ++i) {
      std::int32_t r = 0;
      const float db = -static_cast<float>(i % 24u);
      if (ack(c->SetValue(lltest::BusVolume("Sfx", db), true), &r) && r != SND_OK) badResults.fetch_add(1);
      const std::uint32_t rs = c->ReadCell(SfxVolumeCell(), true);
      ll::CellValue cv{};
      if (ack(rs, &r)) {
        if (!c->WaitCell(rs, 20000, &cv) || cv.result != SND_OK || cv.current != F32Bits(db)) cellMismatch.fetch_add(1);
      }
      const std::uint32_t bus = 0x10000u + i;  // each id added once, removed once
      if (ack(c->StructEdit(ll::StructOp::AddBus, bus, args, 2), &r)) {
        if (r == SND_OK) {
          addsOk.fetch_add(1);
          if (ack(c->StructEdit(ll::StructOp::RemoveBus, bus, nullptr, 0), &r) && r == SND_OK) removesOk.fetch_add(1);
        } else if (r == SND_ERR_OUT_OF_BUDGET) {
          refused.fetch_add(1);
        } else {
          badResults.fetch_add(1);
        }
      }
      if (ack(c->Audition(HashId("Blip"), nullptr, false, true), &r) && r != SND_OK && r != SND_ERR_LIMIT_REACHED)
        badResults.fetch_add(1);
      if ((i % 3u) == 0u && ack(c->StopAuditions(true), &r) && r != SND_OK) badResults.fetch_add(1);
      if (ack(c->RevertValue(SfxVolumeCell(), true), &r) && r != SND_OK) badResults.fetch_add(1);
      if (ack(c->QueryBanks(true), &r) && r != SND_OK) badResults.fetch_add(1);
    }
    std::int32_t r = 0;
    ack(c->StopAuditions(true), &r);
    toolDone.store(true);
  });

  std::thread ui([&] {
    ll::ClientSequences prev{};
    std::uint64_t seenTiming = 0, seenDetail = 0, seenLog = 0;
    std::vector<ll::TimingSample> ts(512);
    std::vector<ll::BlockDetailSample> bd(512);
    std::vector<ll::LogSample> lg(512);
    ll::BankEntry banks[16];
    ll::StreamInfo streams[16];
    ll::ReloadStatus rs[64];
    while (!stop.load()) {
      const ll::ClientSequences s = c->Sequences();
      if (s.timings < prev.timings || s.details < prev.details || s.acks < prev.acks || s.cells < prev.cells ||
          s.bankLists < prev.bankLists || s.eventLog < prev.eventLog || s.streamLists < prev.streamLists)
        uiViolations.fetch_add(1);
      prev = s;
      std::uint64_t first = 0;
      std::uint32_t n = c->CopyTimingsSince(seenTiming, ts.data(), static_cast<std::uint32_t>(ts.size()), &first);
      if (n != 0u) {
        if (first < seenTiming + 1u) uiViolations.fetch_add(1);
        for (std::uint32_t k = 1; k < n; ++k)
          if (ts[k].timing.blockIndex < ts[k - 1u].timing.blockIndex) uiViolations.fetch_add(1);
        seenTiming = first + n - 1u;
      }
      n = c->CopyBlockDetailsSince(seenDetail, bd.data(), static_cast<std::uint32_t>(bd.size()), &first);
      if (n != 0u) {
        if (first < seenDetail + 1u) uiViolations.fetch_add(1);
        seenDetail = first + n - 1u;
      }
      n = c->CopyEventLogSince(seenLog, lg.data(), static_cast<std::uint32_t>(lg.size()), &first);
      if (n != 0u) {
        if (first < seenLog + 1u) uiViolations.fetch_add(1);
        seenLog = first + n - 1u;
      }
      c->CopyBanks(banks, 16);
      c->CopyStreams(streams, 16);
      c->CopyReloadStatus(rs, 64);
      uiReads.fetch_add(1);
      std::this_thread::sleep_for(std::chrono::microseconds(200));
    }
  });

  const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(120);
  while (!toolDone.load() && std::chrono::steady_clock::now() < end) {
    f.Step();
    std::this_thread::sleep_for(std::chrono::microseconds(100));
  }
  for (int i = 0; i < 50; ++i) f.Step();
  stop.store(true);
  tool.join();
  ui.join();
  SND_CHECK(toolDone.load());
  SND_CHECK_EQ(lostAcks.load(), 0u);
  SND_CHECK_EQ(badResults.load(), 0u);
  SND_CHECK_EQ(cellMismatch.load(), 0u);
  SND_CHECK_EQ(uiViolations.load(), 0u);
  SND_CHECK(uiReads.load() > 10u);
  SND_CHECK_EQ(addsOk.load(), removesOk.load());
  SND_CHECK_EQ(addsOk.load() + refused.load(), rounds);
  const ll::DevReserveUsage u = f.rt->ReserveUsage();
  SND_CHECK_EQ(u.buses, 0u);
  SND_CHECK_EQ(u.bytes, 0u);
  SND_CHECK(f.StepUntil([&] { return f.rt->ActiveAuditions() == 0u; }));
  SND_CHECK(f.server->Connected());
  SND_CHECK_EQ(c->Stats().disconnects, 0u);
  std::printf("[indep_s7] stress: adds %u refused %u, ui reads %u\n", addsOk.load(), refused.load(), uiReads.load());
  c->Stop();
  ll::Client::Destroy(c);
}

// Reconnect storm: a client disconnects and reconnects while a ReloadBank and edits are in
// flight; the runtime's counters stay consistent (no reserve, audition or chunk slot leaks).
SND_TEST(IndepS7Stress, ReconnectsDuringRequests) {
  FakeEditor ed;
  ll::RuntimeDesc rd;
  rd.structEditor = &ed;
  Fixture f(Hooks::Live, ReserveConfig(8, 8, 1u << 20), nullptr, rd);
  f.Load(IndepBank(), "a.sndb");
  ll::ServerDesc sd;
  sd.revertOnDisconnect = true;
  SND_REQUIRE_EQ(f.StartServer(sd), SND_OK);
  ll::ClientDesc d = lltest::MemoryClient(f.link);
  d.reconnectDelayMs = 1;
  ll::Client* c = nullptr;
  SND_REQUIRE_EQ(ll::Client::Create(d, &c), SND_OK);
  SND_REQUIRE_EQ(c->Start(), SND_OK);
  std::atomic<bool> done{false};
  std::thread tool([&] {
    const std::uint64_t args[2] = {HashId("Master"), 2};
    const std::vector<std::uint8_t> ch = BussChunk(-3.0f);
    for (std::uint32_t i = 0; i < 60; ++i) {
      for (int k = 0; k < 2000 && c->State() != ll::ClientState::Ready; ++k)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      c->StructEdit(ll::StructOp::AddBus, 0x20000u + i, args, 2, true);
      c->ReplaceChunk(kLive, bf::chunk::BUSS, ch.data(), ch.size(), true);
      c->Audition(HashId("Tone"), nullptr, false, true);
      if ((i % 10u) == 0u) c->ReloadBank(kLive, 0, "a.sndb");
      c->ReadCell(SfxVolumeCell(), true);
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
      if ((i % 3u) == 0u) c->Disconnect();
    }
    done.store(true);
  });
  const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(120);
  while (!done.load() && std::chrono::steady_clock::now() < end) {
    f.Step();
    std::this_thread::sleep_for(std::chrono::microseconds(200));
  }
  tool.join();
  // settle, then a final disconnect revert puts every structural edit back
  SND_CHECK(f.StepUntil([&] { return c->State() == ll::ClientState::Ready && f.server->Connected(); }, 10000));
  f.Steps(50);
  // The sender returns 0 while the client is not Ready (client.hpp), and the tool thread's last
  // Disconnect() may take effect after the client looked Ready (seen once in 80 loaded Release
  // runs: the connection dropped between queueing and the Ack). An Ack belongs to its connection,
  // so the stop is sent again when the connection changed; on one connection it must be answered.
  std::int32_t res = 0;
  bool acked = false;
  for (int attempt = 0; attempt < 5 && !acked; ++attempt) {
    std::uint32_t seq = 0;
    SND_CHECK(f.StepUntil([&] {
      seq = c->StopAuditions(true);
      return seq != 0u;
    }, 10000));
    const ll::ClientStats cs0 = c->Stats();
    const ll::ServerStats ss0 = f.server->Stats();
    acked = f.StepUntil([&] { return c->TakeAck(seq, &res); }, 10000);
    if (!acked) {
      const ll::ClientStats cs1 = c->Stats();
      const ll::ServerStats ss1 = f.server->Stats();
      std::printf("[indep_s7] no Ack for seq %u: client state %u, disconnects %u->%u, connects %u->%u, timeouts %u, "
                  "acksLost %u; server connections %u->%u, connected %d, protocolErrors %u\n",
                  seq, static_cast<unsigned>(c->State()), cs0.disconnects, cs1.disconnects, cs0.connects, cs1.connects,
                  cs1.timeouts, cs1.acksLost, ss0.connections, ss1.connections, f.server->Connected() ? 1 : 0,
                  ss1.protocolErrors);
      SND_CHECK_MSG(cs1.connects != cs0.connects, "no Ack on an unbroken connection (D-86)");
      if (cs1.connects == cs0.connects) break;
      SND_REQUIRE(f.StepUntil([&] { return c->State() == ll::ClientState::Ready && f.server->Connected(); }, 10000));
    }
  }
  SND_CHECK(acked);
  SND_CHECK_EQ(res, SND_OK);
  f.Steps(50);
  c->Stop();
  SND_CHECK(f.StepUntil([&] { return !f.server->Connected(); }));
  f.Steps(50);
  const ll::DevReserveUsage u = f.rt->ReserveUsage();
  SND_CHECK_EQ(u.buses, 0u);
  SND_CHECK_EQ(u.bytes, 0u);
  SND_CHECK_EQ(f.rt->Drops().structUndo, 0u);
  // every chunk slot is free again: a new tool can replace (chunkSlots = 1)
  ll::Client::Destroy(c);
  c = nullptr;
  ll::ClientDesc d2 = lltest::MemoryClient(f.link);
  indep::Tool t(f, d2);
  SND_REQUIRE(t.Ready());
  const std::vector<std::uint8_t> ch = BussChunk(-3.0f);
  SND_CHECK_EQ(t.Ack(t.c->ReplaceChunk(kLive, bf::chunk::BUSS, ch.data(), ch.size())), SND_OK);
  std::printf("[indep_s7] reconnects: server connections %u, active auditions %u, staleEdits %u\n",
              f.server->Stats().connections, f.rt->ActiveAuditions(), f.rt->Drops().staleEdits);
}
