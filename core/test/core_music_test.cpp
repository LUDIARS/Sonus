// core <-> music wiring (music_if.hpp) with a fake IMusicSystem, and the
// minimal transport core runs without a music system (8.1, 8.2).
#include <cmath>
#include <vector>

#include "harness.hpp"

using namespace coretest;
using snd::HashId;

namespace {
constexpr float kCenter = 0.70710678f;

class FakeMusic final : public snd::music::IMusicSystem {
 public:
  std::size_t PoolBytes(const snd::music::MusicLimits&) const noexcept override { return 64; }
  bool Init(void*, std::size_t, const snd::music::MusicLimits& l, const snd::music::MusicServices& s) noexcept override {
    limits = l;
    services = s;
    return true;
  }
  void Shutdown() noexcept override { shutdown = true; }
  void UpdateTransport(const snd::music::MusicFrame& f, snd::schema::Tables&, snd::music::FireList& fire) noexcept override {
    ++updates;
    lastBlock = f.block.index;
    fire.count = 0;
  }
  void RunMusicLogic(const snd::music::MusicFrame&, const snd::schema::Tables&, const snd::music::MusicInput* in,
                     std::uint32_t n, snd::music::MusicVoiceRequestList& out) noexcept override {
    for (std::uint32_t i = 0; i < n; ++i) {
      inputs.push_back(in[i].kind);
      if (in[i].kind == snd::music::MusicInputKind::PlaybackStarted && out.count < out.capacity) {
        // The bank data is reachable through the services (MSEG etc.); here: clip 0 of the bank.
        std::uint64_t bytes = 0;
        chunkFound = services.bankChunk(services.bankCtx, in[i].event.bankSlot, snd::bank_format::chunk::CLIP, &bytes) != nullptr;
        snd::music::MusicVoiceRequest& r = out.requests[out.count++];
        r = snd::music::MusicVoiceRequest{};
        r.op = snd::music::MusicVoiceOp::StartClip;
        r.playbackSlot = in[i].playbackSlot;
        r.tag = 7;
        r.clip = snd::ClipRef{in[i].event.bankSlot, 0, 0};
        r.startClock = in[i].clock;
        r.gain = 0.5f;
      }
      if (in[i].kind == snd::music::MusicInputKind::SetState) states.push_back(in[i].stateId);
    }
  }
  void OnVoiceResults(const snd::music::MusicVoiceResult* r, std::uint32_t n) noexcept override {
    for (std::uint32_t i = 0; i < n; ++i) results.push_back(r[i]);
  }
  std::uint32_t CollectNotifications(snd::notify::EventRecord* out, std::uint32_t cap) noexcept override {
    if (markerSent || results.empty() || cap == 0u) return 0;
    markerSent = true;
    out[0] = snd::notify::EventRecord{};
    out[0].type = static_cast<std::uint16_t>(snd::notify::EventType::Marker);
    out[0].a = 1234;
    out[0].object = 99;
    return 1;
  }
  FakeMusic() {
    inputs.reserve(4096);
    states.reserve(64);
    results.reserve(64);
  }
  bool TicksToClock(snd::SlotIndex, snd::MusicalTicks, snd::ClockSample*) const noexcept override { return false; }
  bool ClockToTicks(snd::SlotIndex, snd::ClockSample, snd::MusicalTicks*) const noexcept override { return false; }

  snd::music::MusicLimits limits{};
  snd::music::MusicServices services{};
  std::uint32_t updates = 0;
  std::uint64_t lastBlock = 0;
  bool shutdown = false, markerSent = false, chunkFound = false;
  std::vector<snd::music::MusicInputKind> inputs;
  std::vector<snd::Id> states;
  std::vector<snd::music::MusicVoiceResult> results;
};

bool HasInput(const FakeMusic& m, snd::music::MusicInputKind k) {
  for (auto i : m.inputs) {
    if (i == k) return true;
  }
  return false;
}
}  // namespace

SND_TEST(CoreMusic, MusicPlaybackDrivesVoicesThroughTheMusicSystem) {
  FakeMusic music;
  std::uint32_t markers = 0;
  {
    Harness h(SmallConfig(), {}, &music);
    struct Both {
      Ends* ends;
      std::uint32_t* markers;
    } both{&h.ends, &markers};
    snd_callbacks cb{};
    cb.user = &both;
    cb.on_marker = [](void* u, snd_playback_handle, snd::Id id, const snd_timing*) {
      if (id == 1234u) ++*static_cast<Both*>(u)->markers;
    };
    cb.on_end = [](void* u, snd_playback_handle hp, snd_end_reason r, snd_end_detail d) {
      static_cast<Both*>(u)->ends->list.push_back(Ends::E{hp.value, r, d});
    };
    h.sys->SetCallbacks(&cb);
    TestBank b = BasicBank();
    b.assets.push_back(TAsset{HashId("seg"), 1, 48000, Constant(480000, 0.4f), 0, 0});
    TEvent e = SimpleEvent("Music", 0);
    e.flags = bf::kEventMusic;
    b.events.push_back(e);
    h.Load("b", b.Build(HashId("B")));
    const snd_playback_handle p = h.sys->Play(HashId("Music"), snd_emitter_handle{0}, nullptr);
    h.Clear();
    h.Steps(4);
    SND_CHECK(music.updates > 0u);
    SND_CHECK(HasInput(music, snd::music::MusicInputKind::PlaybackStarted));
    SND_REQUIRE_EQ(music.results.size(), std::size_t{1});
    SND_CHECK_EQ(music.results[0].tag, 7u);
    SND_CHECK(music.results[0].voiceSlot != snd::kInvalidSlot);
    SND_CHECK(music.chunkFound);
    // Only the music voice plays, at the requested gain.
    SND_CHECK_NEAR(h.At(3 * 256 + 5, 0), 0.4f * 0.5f * kCenter, 1e-4f);
    h.sys->SetMusicState(p, HashId("Battle"), nullptr);
    h.Steps(2);
    SND_REQUIRE_EQ(music.states.size(), std::size_t{1});
    SND_CHECK_EQ(music.states[0], HashId("Battle"));
    SND_CHECK_EQ(markers, 1u);
    h.sys->Stop(p, 0.0f, nullptr);
    h.Steps(6);
    h.sys->Update();
    SND_CHECK(HasInput(music, snd::music::MusicInputKind::PlaybackStopping));
    SND_CHECK(HasInput(music, snd::music::MusicInputKind::PlaybackEnded));
    SND_CHECK(h.ends.Has(p, SND_END_STOPPED));
  }
  SND_CHECK(music.shutdown);
}

SND_TEST(CoreMusic, MinimalTransportQuantisesToTheNextBar) {
  Harness h;
  TestBank b = BasicBank();
  b.assets.push_back(TAsset{HashId("imp"), 1, 48000, Impulse(4000, 0, 1.0f), 0, 0});
  b.events.push_back(SimpleEvent("I", 0));
  h.Load("b", b.Build(HashId("B")));
  // 120 bpm, 4/4: one bar = 2 s = 96000 frames from the transport start.
  snd_transport_desc td{120.0f, 4, 4, 1, 0};
  const snd_transport_handle tr = h.sys->CreateTransport(&td);
  SND_REQUIRE(tr.value != 0u);
  h.Clear();
  h.Steps(3);  // the transport starts in block 0 of this run
  snd_when w{};
  w.kind = SND_WHEN_NEXT_BAR;
  w.transport = tr;
  const snd_playback_handle p = h.sys->Play(HashId("I"), snd_emitter_handle{0}, &w);
  h.Steps(380);  // ~2.03 s
  SND_CHECK_EQ(h.sys->GetPlaybackState(p) == SND_PLAYBACK_STATE_PENDING, false);
  std::size_t at = SIZE_MAX;
  for (std::size_t f = 0; f < h.TotalFrames(); ++f) {
    if (std::fabs(h.At(f, 0)) > 0.1f) {
      at = f;
      break;
    }
  }
  SND_CHECK_EQ(at, std::size_t{96000});
  snd_position pos{};
  h.sys->Update();
  SND_CHECK_EQ(h.sys->GetTransportPosition(tr, &pos), SND_OK);
  SND_CHECK_EQ(pos.musical.bar, 1);
  SND_CHECK_EQ(h.sys->DestroyTransport(tr), SND_OK);
  h.Steps(3);
  h.sys->Update();
  SND_CHECK_EQ(h.sys->GetTransportPosition(tr, &pos), SND_ERR_INVALID_HANDLE);
}
