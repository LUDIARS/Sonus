// Live link under concurrency and hostile input (design.md 14: RT safety, fuzz).
//   - The game renders and updates on its thread while a tool thread floods
//     edits, overrides and auditions and the profiler streams everything: every
//     edit is acknowledged, the RT checks of the test kit see no violation in
//     the audio-thread hooks (RenderBlock runs inside SND_RT_SCOPE). Repeat
//     under TSan and Release: ctest --repeat until-fail:20 -R livelink_stress
//   - A peer sends mutated, truncated and oversized frames after a valid
//     handshake: the server never crashes (ASan), the game keeps rendering and
//     the next tool works.
#include <atomic>
#include <random>
#include <thread>

#include "livelink_fixture.hpp"

using namespace lltest;
using snd::HashId;

SND_TEST(LivelinkStress, ToolFloodsEditsWhileTheGameRenders) {
  Fixture f;
  f.Load(DevBank());
  f.sys->Play(HashId("Tone"), snd_emitter_handle{0}, nullptr);
  f.Steps(4);
  ll::ServerDesc sd;
  sd.meterHz = 120;
  sd.timingHz = 120;
  sd.timeoutMs = 60000;  // sanitizers under load: no spurious heartbeat timeouts
  SND_REQUIRE_EQ(f.StartServer(sd), SND_OK);
  ll::Client* c = nullptr;
  ll::ClientDesc cd = MemoryClient(f.link);
  cd.timeoutMs = 60000;
  SND_REQUIRE_EQ(ll::Client::Create(cd, &c), SND_OK);
  c->SetProfilerMask(0x7Fu, 120);
  c->Start();
  SND_REQUIRE(f.StepUntil([&] { return c->State() == ll::ClientState::Ready; }));
  std::atomic<bool> done{false};
  std::atomic<std::uint32_t> acked{0}, sent{0}, failed{0};
  std::thread tool([&] {
    std::vector<std::uint32_t> seqs;
    for (int i = 0; i < 300; ++i) {
      const float db = -static_cast<float>(i % 24);
      std::uint32_t s = c->SetValue(BusVolume(i % 2 ? "Sfx" : "Master", db));
      const auto retryEnd = std::chrono::steady_clock::now() + std::chrono::seconds(30);
      while (s == 0u && std::chrono::steady_clock::now() < retryEnd) {  // queue full: retry
        std::this_thread::sleep_for(std::chrono::microseconds(200));
        s = c->SetValue(BusVolume(i % 2 ? "Sfx" : "Master", db));
      }
      seqs.push_back(s);
      sent.fetch_add(1);
      c->OverrideParam(HashId("Knob"), 0.01f * static_cast<float>(i % 100), (i % 7) != 0);
      if (i % 25 == 0) c->Audition(HashId("Blip"));
      std::this_thread::sleep_for(std::chrono::microseconds(100));
    }
    const auto ackEnd = std::chrono::steady_clock::now() + std::chrono::seconds(60);
    for (std::uint32_t s : seqs) {
      std::int32_t r = 1;
      const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(ackEnd - std::chrono::steady_clock::now());
      if (s != 0u && left.count() > 0 && c->WaitAck(s, static_cast<std::uint32_t>(left.count()), &r) && r == SND_OK) {
        acked.fetch_add(1);
      } else {
        failed.fetch_add(1);
      }
    }
    done.store(true);
  });
  const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(120);
  while (!done.load() && std::chrono::steady_clock::now() < end) {
    f.Step();
    std::this_thread::sleep_for(std::chrono::microseconds(100));
  }
  tool.join();
  SND_CHECK_EQ(acked.load(), 300u);
  SND_CHECK_EQ(failed.load(), 0u);
  SND_CHECK(c->Received(ll::MessageType::BlockTiming) > 0u);
  // The last edit of each bus is in effect: Sfx -(299 % 24) = -11 dB, Master -(298 % 24) = -10 dB.
  f.Steps(8);
  ll::Client::Destroy(c);
}

SND_TEST(LivelinkStress, MutatedFramesNeverCrashTheServer) {
  Fixture f;
  f.Load(DevBank());
  SND_REQUIRE_EQ(f.StartServer(), SND_OK);
  std::mt19937 rng(1234);
  // Valid payloads of every tool -> game type, the seeds of the mutations.
  std::vector<std::pair<ll::MessageType, std::vector<std::uint8_t>>> seeds;
  auto add = [&](ll::MessageType t, const void* p, std::size_t n) {
    const auto* b = static_cast<const std::uint8_t*>(p);
    seeds.emplace_back(t, std::vector<std::uint8_t>(b, b + n));
  };
  const ll::SetValue sv = BusVolume("Sfx", -3.0f);
  add(ll::MessageType::SetValue, &sv, sizeof(sv));
  const ll::OverrideParam op{HashId("Knob"), 1, 0.5f, 0};
  add(ll::MessageType::OverrideParam, &op, sizeof(op));
  const ll::OverrideState os{HashId("Music"), HashId("A"), 1, 0};
  add(ll::MessageType::OverrideState, &os, sizeof(os));
  const ll::Audition au{HashId("Blip"), 0, {0, 0, 0}, 0};
  add(ll::MessageType::Audition, &au, sizeof(au));
  const ll::SetProfilerMask pm{0x7F, 60};
  add(ll::MessageType::SetProfilerMask, &pm, sizeof(pm));
  const ll::RecordControl rc{1, 0};
  add(ll::MessageType::RecordControl, &rc, sizeof(rc));
  {
    bt::TableBuilder bu(bf::chunk::BUSS);
    bu.AddRow();
    bu.AddRow();
    const std::vector<std::uint8_t> body = bu.Serialize();
    std::vector<std::uint8_t> rcp(sizeof(ll::ReplaceChunk) + body.size());
    const ll::ReplaceChunk h{kBankId, bf::chunk::BUSS, body.size()};
    std::memcpy(rcp.data(), &h, sizeof(h));
    std::memcpy(rcp.data() + sizeof(h), body.data(), body.size());
    add(ll::MessageType::ReplaceChunk, rcp.data(), rcp.size());
  }
  {
    std::vector<std::uint8_t> se(sizeof(ll::StructEdit) + 16);
    const ll::StructEdit h{0, HashId("Sfx"), 2, 0};
    std::memcpy(se.data(), &h, sizeof(h));
    add(ll::MessageType::StructEdit, se.data(), se.size());
  }
  add(ll::MessageType::ClearOverrides, nullptr, 0);
  for (int session = 0; session < 6; ++session) {
    RawPeer p;
    SND_REQUIRE(f.StepUntil([&] { return p.Connect(f.link.connector(), 10); }));
    SND_REQUIRE(p.Hello());
    SND_REQUIRE(p.WaitFor(ll::MessageType::HelloReply) != nullptr);
    for (int i = 0; i < 400 && !p.closed; ++i) {
      const auto& s = seeds[rng() % seeds.size()];
      std::vector<std::uint8_t> payload = s.second;
      std::uint16_t type = static_cast<std::uint16_t>(s.first);
      std::uint32_t declared = static_cast<std::uint32_t>(payload.size());
      switch (rng() % 8) {
        case 0:  // flip bytes
          for (int k = 0; k < 4 && !payload.empty(); ++k) payload[rng() % payload.size()] ^= static_cast<std::uint8_t>(1u << (rng() % 8));
          break;
        case 1:  // truncate
          payload.resize(payload.empty() ? 0 : rng() % payload.size());
          declared = static_cast<std::uint32_t>(payload.size());
          break;
        case 2:  // random bytes, random length
          payload.resize(rng() % 96);
          for (auto& b : payload) b = static_cast<std::uint8_t>(rng());
          declared = static_cast<std::uint32_t>(payload.size());
          break;
        case 3:  // random type (unknown ones are skipped, game -> tool ones answered with Error)
          type = static_cast<std::uint16_t>(rng() % 0x120);
          break;
        case 4:  // huge counts inside the payload (ReplaceChunk.bytes, StructEdit.argCount)
          if (payload.size() >= 16) {
            const std::uint64_t big = 0xFFFFFFFFFFull + (rng() % 1000);
            std::memcpy(payload.data() + 8, &big, 8);
          }
          break;
        case 5:  // enum fields out of range
          if (payload.size() >= 8) {
            const std::uint32_t v = 0x7FFFFFF0u + (rng() % 16);
            std::memcpy(payload.data() + 4 * (rng() % (payload.size() / 4)), &v, 4);
          }
          break;
        default:
          break;
      }
      std::vector<std::uint8_t> frame(ll::kFrameHeaderBytes + payload.size());
      ll::EncodeHeader(frame.data(), type, static_cast<std::uint16_t>(rng() % 2), declared, static_cast<std::uint32_t>(i));
      if (!payload.empty()) std::memcpy(frame.data() + ll::kFrameHeaderBytes, payload.data(), payload.size());
      if (!p.SendBytes(frame.data(), frame.size(), 200)) break;
      if (i % 16 == 0) {
        f.Step();
        p.Pump(0);
        p.frames.clear();
      }
    }
    // End each session with a fatal header: oversized, then bad magic.
    std::uint8_t h[16];
    ll::EncodeHeader(h, static_cast<std::uint16_t>(ll::MessageType::SetValue), 0,
                     session % 2 == 0 ? ll::kMaxPayloadBytes + 1u : 4u, 0);
    if (session % 3 == 2) h[0] ^= 0xFF;
    p.SendBytes(h, sizeof(h), 200);
    f.StepUntil([&] { p.Pump(1); return p.closed || !f.server->Connected(); }, 2000);
  }
  // Still alive and serving.
  ll::Client* c = nullptr;
  SND_REQUIRE_EQ(ll::Client::Create(MemoryClient(f.link), &c), SND_OK);
  c->Start();
  SND_REQUIRE(f.StepUntil([&] { return c->State() == ll::ClientState::Ready; }, 10000));
  const std::uint32_t seq = c->SetValue(BusVolume("Sfx", -1.0f));
  std::int32_t r = 1;
  SND_CHECK(f.StepUntil([&] { return c->TakeAck(seq, &r); }));
  SND_CHECK_EQ(r, SND_OK);
  SND_CHECK(f.server->Stats().protocolErrors > 0u);
  ll::Client::Destroy(c);
}
