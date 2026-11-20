// Concurrency of the 1.1 paths (run under TSan, and 20 times in Release): a UI thread
// reads the client (update numbers, Copy*Since, banks, streams) while the network thread
// receives; a tool thread mixes SetValue / ReadCell / RevertValue / stop-all / QueryBanks
// while the test thread renders. Every request is answered exactly once, the answers of
// the applied edits come in order, and the last read sees the last write.
#include <atomic>
#include <thread>

#include "livelink_v11_common.hpp"

using namespace v11;

SND_TEST(LivelinkV11Stress, MixedRequestsWhileReadingTheClient) {
  ll::ServerDesc sd;
  sd.timingHz = 200;
  Session s(Config(), {}, sd);
  SND_REQUIRE(s.Ready());
  s.client->SetProfilerMask(ll::kProfTiming | ll::kProfStreamList | ll::kProfBlockDetail | ll::kProfMeters, 120);
  std::atomic<bool> done{false};
  std::atomic<std::uint32_t> uiRounds{0};
  std::thread ui([&] {
    std::vector<ll::TimingSample> t(64);
    std::vector<ll::MeterSample> m(64);
    ll::BankEntry b[4];
    ll::StreamInfo si[8];
    std::uint64_t seen = 0, first = 0;
    while (!done.load(std::memory_order_acquire)) {
      const ll::ClientSequences q = s.client->Sequences();
      const std::uint32_t n = s.client->CopyTimingsSince(seen, t.data(), 64, &first);
      if (n != 0u) seen = first + n - 1u;
      SND_CHECK(seen <= q.timings + 64u);
      s.client->CopyMeterHistorySince(0, ~0u, m.data(), 64);
      s.client->CopyBanks(b, 4);
      s.client->CopyStreams(si, 8);
      uiRounds.fetch_add(1u, std::memory_order_relaxed);
      std::this_thread::yield();
    }
  });
  struct Req {
    std::uint32_t seq;
    int kind;  // 0 set, 1 read, 2 revert, 3 stop, 4 banks
    float db;
  };
  std::vector<Req> reqs;
  std::atomic<bool> sending{true};
  std::thread tool([&] {
    float db = 0.0f;
    for (std::uint32_t i = 0; i < 400; ++i) {
      Req r{0, static_cast<int>(i % 5u), 0.0f};
      switch (r.kind) {
        case 0:
          db = -static_cast<float>(i % 24u);
          r.db = db;
          r.seq = s.client->SetValue(BusVolume("Sfx", db));
          break;
        case 1: r.seq = s.client->ReadCell(BusVolumeCell("Sfx"), true); break;
        case 2: r.seq = s.client->RevertValue(BusVolumeCell("Master")); break;
        case 3: r.seq = s.client->StopAuditions(true); break;
        default: r.seq = s.client->QueryBanks(true); break;
      }
      if (r.seq != 0u) reqs.push_back(r);
      if (i % 4u == 0u) std::this_thread::sleep_for(std::chrono::microseconds(200));
    }
    sending.store(false, std::memory_order_release);
  });
  // Render until every request was answered.
  SND_CHECK(s.f.StepUntil([&] { return !sending.load(std::memory_order_acquire); }, 20000, 50));
  tool.join();
  std::uint32_t answered = 0;
  float lastSet = 0.0f;
  for (const Req& r : reqs) {
    std::int32_t res = 1;
    const bool got = s.f.StepUntil([&] { return s.client->TakeAck(r.seq, &res); }, 5000, 50);
    SND_CHECK_MSG(got, "seq %u (kind %d) not answered", r.seq, r.kind);
    if (!got) continue;
    ++answered;
    // A full command buffer is the tool's to resend (D-86); everything else succeeds.
    SND_CHECK_MSG(res == SND_OK || res == SND_ERR_COMMAND_BUFFER_FULL, "seq %u kind %d result %d", r.seq, r.kind, res);
    std::int32_t again = 1;
    SND_CHECK(!s.client->TakeAck(r.seq, &again));  // exactly once
    if (r.kind == 0 && res == SND_OK) lastSet = r.db;  // applied edits are answered in seq order
  }
  SND_CHECK_EQ(answered, static_cast<std::uint32_t>(reqs.size()));
  SND_CHECK(reqs.size() >= 300u);
  ll::CellValue v{};
  SND_REQUIRE(s.WaitCellValue(s.client->ReadCell(BusVolumeCell("Sfx")), &v));
  SND_CHECK_EQ(v.current, Bits(lastSet));
  done.store(true, std::memory_order_release);
  ui.join();
  SND_CHECK(uiRounds.load() > 0u);
  SND_CHECK_EQ(s.f.server->Stats().protocolErrors, 0u);
}

// A tool that disconnects and reconnects while hot reloads, struct edits and auditions are
// in flight: nothing leaks (the dev reserve and the audition places come back), the game
// keeps rendering, the last session works.
SND_TEST(LivelinkV11Stress, ReconnectsWhileRequestsAreInFlight) {
  FakeEditor ed;
  ll::RuntimeDesc rd;
  rd.structEditor = &ed;
  ll::ServerDesc sd;
  sd.revertOnDisconnect = true;
  Session s(Config(), rd, sd);
  indep::BankOpts rebuilt;
  s.f.fs.AddFile("Live2.sndb", indep::IndepBank(rebuilt));
  std::uint32_t reloads = 0;
  for (std::uint32_t round = 0; round < 6; ++round) {
    SND_REQUIRE(s.Ready());
    const std::uint64_t in0 = s.f.server->Stats().framesIn;
    const std::uint64_t addBus[2] = {HashId("Master"), 2u};
    std::uint32_t sent = 0;
    sent += s.client->StructEdit(ll::StructOp::AddBus, HashId("New") + round, addBus, 2) != 0u ? 1u : 0u;
    sent += s.client->Audition(HashId("Tone")) != 0u ? 1u : 0u;
    sent += s.client->SetValue(BusVolume("Sfx", -3.0f)) != 0u ? 1u : 0u;
    if (round % 2u == 1u) {
      const std::uint32_t q = s.client->ReloadBank(kBankId, 0, round % 4u == 1u ? "Live2.sndb" : "Live.sndb");
      sent += q != 0u ? 1u : 0u;
      reloads += q != 0u ? 1u : 0u;
    }
    // The game took them (some still in flight on the audio side), then the tool goes away.
    SND_REQUIRE(s.f.StepUntil([&] { return s.f.server->Stats().framesIn >= in0 + sent; }));
    s.f.Steps(round);
    s.client->Disconnect();
    SND_REQUIRE(s.f.StepUntil([&] { return !s.f.server->Connected(); }));
  }
  SND_REQUIRE(s.Ready());
  SND_CHECK_EQ(reloads, 3u);
  // Each reload finished or was refused while another one ran (LIMIT_REACHED, not counted).
  SND_CHECK(s.f.StepUntil([&] { return s.f.server->Stats().reloadsDone + s.f.server->Stats().reloadsFailed >= 1u; }));
  SND_CHECK(s.f.server->Stats().connections >= 7u);
  SND_CHECK_EQ(s.Ack(s.client->StopAuditions(true)), SND_OK);
  SND_CHECK(s.f.StepUntil([&] { return s.f.rt->ActiveAuditions() == 0u; }));
  s.client->Disconnect();  // the revert of the last session undoes its edits
  SND_REQUIRE(s.f.StepUntil([&] { return !s.f.server->Connected(); }));
  SND_CHECK(s.f.StepUntil([&] { return s.f.rt->ReserveUsage().buses == 0u && s.f.rt->ReserveUsage().bytes == 0u; }));
  SND_REQUIRE(s.Ready());
  SND_CHECK_EQ(s.Ack(s.client->SetValue(BusVolume("Sfx", -6.0f))), SND_OK);
}
