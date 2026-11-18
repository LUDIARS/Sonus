// The music system as a whole (8.3, 8.4, 2.5, 14): sync-group stalls, pause and
// stop of the music playback, seeks of its transport, stingers (replace /
// stack), determinism (bit-identical output from the same commands), no
// allocation after initialisation, RT safety (the runner fails a test on any
// RT violation inside RenderBlock), and parallel rendering.
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "music_fixture.hpp"

using namespace mt;

namespace {

std::vector<std::uint8_t> BuildOne(MusicProject& p) {
  std::string diag;
  std::vector<std::uint8_t> bank = p.Build(&diag);
  if (bank.empty()) std::fprintf(stderr, "%s\n", diag.c_str());
  return bank;
}

const char* kOneState = "\"initialState\": \"S\", \"states\": [{\"name\": \"S\", \"playlist\": \"PL\"}]";

std::vector<std::uint8_t> StallBank(bool soundPriority) {
  MusicProject p(soundPriority ? "stall-sound" : "stall-time");
  p.Asset("s", Material(kBar, 0.3f), soundPriority ? "\"stallPolicy\": \"sound\"" : "");
  p.Segment("S1", "s", 1);
  p.Playlist("PL", "{\"segment\": \"S1\"}", "\"loopCount\": 2");
  p.MusicEvent("Music", kOneState);
  return BuildOne(p);
}

std::uint64_t NthStart(const Rig& r, std::size_t n) {
  const std::vector<StartLog> st = r.kind.Starts();
  return n < st.size() ? st[n].clock : UINT64_MAX;
}

}  // namespace

SND_TEST(MusicSystem, SoundPriorityStallHoldsTheTransport) {
  Rig r;
  SND_REQUIRE(r.Load(StallBank(true)));
  const std::uint64_t s0 = r.Clock();
  r.sys->Play(HashId("Music"), snd_emitter_handle{0}, nullptr);
  r.kind.stallAsset.store(HashId("s"));
  r.kind.stallFrom.store(s0 + 100 * 256);
  r.kind.stallTo.store(s0 + 104 * 256);  // 4 blocks starved: 1024 samples late
  r.RunTo(s0 + 2 * kBar + 4096);
  SND_CHECK_EQ(NthStart(r, 1), s0 + kBar + 1024);
  // No gap and no overlap: the held material ends where the next segment begins.
  SND_CHECK_NEAR(r.At(s0 + kBar + 1024 - 300), 0.3f, 1e-4f);
  SND_CHECK_NEAR(r.At(s0 + kBar + 1024 + 300), 0.3f, 1e-4f);
  r.sys->Update();
  bool shifted = false;
  for (const Beat& b : r.ev.beats) {
    if (b.bar == 0 && b.beat == 2) shifted = b.clock == s0 + 48000 + 1024;
  }
  SND_CHECK(shifted);
}

SND_TEST(MusicSystem, TimePriorityStallKeepsTheTransportGoing) {
  Rig r;
  SND_REQUIRE(r.Load(StallBank(false)));
  const std::uint64_t s0 = r.Clock();
  r.sys->Play(HashId("Music"), snd_emitter_handle{0}, nullptr);
  r.kind.stallFrom.store(s0 + 100 * 256);
  r.kind.stallTo.store(s0 + 104 * 256);
  r.RunTo(s0 + 2 * kBar + 4096);
  SND_CHECK_EQ(NthStart(r, 1), s0 + kBar);
  SND_CHECK_NEAR(r.At(s0 + 100 * 256 + 512), 0.0f, 1e-3f);  // silent while starved
  SND_CHECK_NEAR(r.At(s0 + 120 * 256), 0.3f, 1e-4f);         // then from the current position
}

SND_TEST(MusicSystem, PausedPlaybackFreezesItsTransport) {
  Rig r;
  SND_REQUIRE(r.Load(StallBank(false)));
  const std::uint64_t s0 = r.Clock();
  const snd_playback_handle p = r.sys->Play(HashId("Music"), snd_emitter_handle{0}, nullptr);
  r.RunTo(s0 + 30000);
  const std::uint64_t cp = r.Clock();
  r.sys->Pause(p, 0.0f);
  r.RunTo(s0 + 80000);
  snd_position a{}, b{};
  r.sys->Update();
  r.sys->GetTransportPosition(OwnedTransport(p), &a);
  r.Steps(20);
  r.sys->Update();
  r.sys->GetTransportPosition(OwnedTransport(p), &b);
  SND_CHECK(a.musical.bar == b.musical.bar && a.musical.beat == b.musical.beat && a.musical.tick == b.musical.tick);
  const std::uint64_t cr = r.Clock();
  r.sys->ResumePlayback(p, 0.0f);
  r.RunTo(s0 + 3 * kBar);
  // The next segment comes later by the time the playback was paused (block granularity).
  const std::uint64_t gap = cr - cp;
  const std::uint64_t next = NthStart(r, 1);
  SND_CHECK(next >= s0 + kBar + gap - 1024 && next <= s0 + kBar + gap + 1024);
}

SND_TEST(MusicSystem, StopEndsTheMusicAndItsTransport) {
  Rig r;
  SND_REQUIRE(r.Load(StallBank(false)));
  const std::uint64_t s0 = r.Clock();
  const snd_playback_handle p = r.sys->Play(HashId("Music"), snd_emitter_handle{0}, nullptr);
  r.RunTo(s0 + 50000);
  r.sys->Stop(p, 0.1f, nullptr);
  r.RunTo(s0 + 3 * kBar);
  r.sys->Update();
  bool stopped = false;
  for (const EndRec& e : r.ev.ends) stopped = stopped || (e.handle == p.value && e.reason == SND_END_STOPPED);
  SND_CHECK(stopped);
  SND_CHECK_EQ(r.kind.Starts().size(), std::size_t{1});  // no further segment
  snd_position pos{};
  SND_CHECK_EQ(r.sys->GetTransportPosition(OwnedTransport(p), &pos), SND_ERR_INVALID_HANDLE);
  snd::music::MusicPlaybackInfo info;
  SND_CHECK(!r.music.GetPlaybackInfo(SlotOf(p), &info));
}

SND_TEST(MusicSystem, SeekOfTheMusicTransportRelocatesTheSegment) {
  MusicProject p("seek");
  bt::PcmData m = Material(2 * kBar, 0.2f);
  for (std::size_t i = kBar; i < 2 * kBar; ++i) m.samples[i] = 0.4f;
  p.Asset("e", m);
  p.Segment("E", "e", 2);
  p.Playlist("PL", "{\"segment\": \"E\"}");
  p.MusicEvent("Music", kOneState);
  Rig r;
  SND_REQUIRE(r.Load(BuildOne(p)));
  const std::uint64_t s0 = r.Clock();
  const snd_playback_handle h = r.sys->Play(HashId("Music"), snd_emitter_handle{0}, nullptr);
  r.RunTo(s0 + 10000);
  const std::uint64_t c = r.Clock();
  snd_musical_time to{1, 0, 0};
  r.sys->SeekTransport(OwnedTransport(h), &to, nullptr);
  r.RunTo(s0 + 3 * kBar);
  const std::vector<StartLog> st = r.kind.Starts();
  SND_REQUIRE(st.size() >= 2u);
  SND_CHECK_EQ(st[1].clock, c);
  SND_CHECK_EQ(st[1].frame, static_cast<std::uint64_t>(kBar));
  SND_CHECK_NEAR(r.At(c + 2000), 0.4f, 1e-4f);
  SND_REQUIRE(st.size() >= 3u);
  SND_CHECK_EQ(st[2].clock, c + kBar);  // the playlist goes on from the relocated segment's end
}

SND_TEST(MusicSystem, StingersReplaceOrStack) {
  MusicProject p("stingers");
  p.Asset("m", Material(4 * kBar, 0.1f));
  p.Asset("st", Material(4800, 0.05f));
  p.Asset("st2", Material(4800, 0.05f));
  p.Event("St", "\"stinger\": true, \"tracks\": [{\"clips\": [{\"asset\": \"st\"}]}]");
  p.Event("St2", "\"stinger\": true, \"tracks\": [{\"clips\": [{\"asset\": \"st2\"}]}]");
  p.Segment("M", "m", 4);
  p.Playlist("PL", "{\"segment\": \"M\"}");
  p.MusicEvent("Music", std::string(kOneState) +
                            ", \"stingers\": [{\"name\": \"Hit\", \"event\": \"St\", \"timing\": \"nextBeat\", \"overlap\": \"replace\"},"
                            " {\"name\": \"Hit2\", \"event\": \"St2\", \"timing\": \"nextBeat\", \"overlap\": \"stack\"}]");
  Rig r;
  SND_REQUIRE(r.Load(BuildOne(p)));
  const std::uint64_t s0 = r.Clock();
  const snd_playback_handle m = r.sys->Play(HashId("Music"), snd_emitter_handle{0}, nullptr);
  r.RunTo(s0 + 10000);
  const snd_when w = NextBar(OwnedTransport(m));
  const snd_playback_handle a = r.sys->Play(HashId("St"), snd_emitter_handle{0}, &w);
  const snd_playback_handle b = r.sys->Play(HashId("St2"), snd_emitter_handle{0}, &w);
  r.Step();
  const snd_playback_handle a2 = r.sys->Play(HashId("St"), snd_emitter_handle{0}, &w);
  const snd_playback_handle b2 = r.sys->Play(HashId("St2"), snd_emitter_handle{0}, &w);
  r.RunTo(s0 + 2 * kBar);
  r.sys->Update();
  bool replaced = false;
  for (const EndRec& e : r.ev.ends) {
    replaced = replaced || (e.handle == a.value && e.reason == SND_END_REJECTED && e.detail == SND_END_DETAIL_RESERVATION_MISSED);
    SND_CHECK(e.handle != a2.value || e.reason == SND_END_COMPLETED);
    SND_CHECK(e.handle != b.value || e.reason == SND_END_COMPLETED);
    SND_CHECK(e.handle != b2.value || e.reason == SND_END_COMPLETED);
  }
  SND_CHECK(replaced);
  std::uint32_t st = 0, st2 = 0;
  for (const StartLog& s : r.kind.Starts()) {
    if (s.asset == HashId("st")) {
      ++st;
      SND_CHECK_EQ(s.clock, s0 + kBar);
    }
    if (s.asset == HashId("st2")) {
      ++st2;
      SND_CHECK_EQ(s.clock, s0 + kBar);
    }
  }
  SND_CHECK_EQ(st, 1u);
  SND_CHECK_EQ(st2, 2u);
}

namespace {

// A session with random playlists, transitions, stems and parameter changes.
std::vector<std::uint8_t> BusyBank() {
  MusicProject p("busy");
  p.Parameter("Intensity", "\"min\": 0, \"max\": 1, \"default\": 0.5, \"scope\": [\"playback\"]");
  const char* names[] = {"a", "b", "c", "d"};
  for (int i = 0; i < 4; ++i) {
    bt::PcmData m = Material(kBar + 6000, 0.05f + 0.05f * i);
    for (std::size_t k = 0; k < m.samples.size(); ++k) m.samples[k] *= static_cast<float>(1.0 + 0.3 * std::sin(0.001 * k * (i + 1)));
    p.Asset(names[i], m);
    p.Segment(std::string("S") + names[i], names[i], 1, "\"entryFrame\": 3000, \"markers\": [{\"name\": \"Mk\", \"frame\": 40000}]");
  }
  p.Asset("drum", Material(kBar + 6000, 0.07f));
  p.Segment("Sd", "drum", 1,
            "\"entryFrame\": 3000, \"stems\": [{\"asset\": \"a\", \"channelFirst\": 0, \"channelCount\": 1, \"volumeCurve\": "
            "{\"parameter\": \"Intensity\", \"target\": \"volume\", \"points\": [[0, -30], [1, 0]]}}]");
  p.Playlist("P1", "{\"segment\": \"Sa\"}, {\"segment\": \"Sb\", \"weight\": 2}, {\"segment\": \"Sd\"}", "\"mode\": \"random\"");
  p.Playlist("P2", "{\"segment\": \"Sc\"}, {\"segment\": \"Sd\"}", "\"mode\": \"shuffle\"");
  p.MusicEvent("Music",
               "\"initialState\": \"One\", \"states\": [{\"name\": \"One\", \"playlist\": \"P1\"}, {\"name\": \"Two\", \"playlist\": \"P2\"}],"
               " \"transitions\": [{\"from\": \"One\", \"to\": \"Two\", \"timing\": \"nextBeat\", \"fadeOutBeats\": 1, \"fadeInBeats\": 0.5, "
               "\"bridge\": \"Sc\"}, {\"from\": \"*\", \"to\": \"*\", \"timing\": \"nextBar\", \"start\": \"lastPosition\", "
               "\"fadeOutBeats\": 0.5, \"fadeInBeats\": 1}]");
  return BuildOne(p);
}

struct Script {
  std::vector<float> audio;
  std::vector<StartLog> starts;
  std::vector<Beat> beats;
  std::vector<Marker> markers;
};

void RunScript(Rig& r, Script* out, std::uint32_t blocks = 3000) {
  const std::uint64_t s0 = r.Clock();
  const snd_playback_handle p = r.sys->Play(HashId("Music"), snd_emitter_handle{0}, nullptr);
  for (std::uint32_t i = 0; i < blocks; ++i) {
    if (i == 300) r.sys->SetMusicState(p, HashId("Two"), nullptr);
    if (i == 700) r.sys->SetParamPlayback(p, HashId("Intensity"), 1.0f);
    if (i == 900) r.sys->SetMusicState(p, HashId("One"), nullptr);
    if (i == 910) r.sys->SetMusicState(p, HashId("Two"), nullptr);
    if (i == 1500) {
      const snd_when w = Kind(OwnedTransport(p), SND_WHEN_GRID, 8);
      r.sys->SetMusicState(p, HashId("One"), &w);
    }
    if (i == 2200) r.sys->SetParamPlayback(p, HashId("Intensity"), 0.2f);
    r.Step();
  }
  r.sys->Update();
  (void)s0;
  out->audio = r.all;
  out->starts = r.kind.Starts();
  out->beats = r.ev.beats;
  out->markers = r.ev.markers;
}

bool SameStarts(const std::vector<StartLog>& a, const std::vector<StartLog>& b) {
  if (a.size() != b.size()) return false;
  for (std::size_t i = 0; i < a.size(); ++i) {
    if (a[i].clock != b[i].clock || a[i].frame != b[i].frame || a[i].asset != b[i].asset) return false;
  }
  return true;
}
bool SameBeats(const std::vector<Beat>& a, const std::vector<Beat>& b) {
  if (a.size() != b.size()) return false;
  for (std::size_t i = 0; i < a.size(); ++i) {
    if (a[i].clock != b[i].clock || a[i].bar != b[i].bar || a[i].beat != b[i].beat) return false;
  }
  return true;
}

}  // namespace

SND_TEST(MusicSystem, SameCommandsGiveBitIdenticalOutput) {
  const std::vector<std::uint8_t> bank = BusyBank();
  SND_REQUIRE(!bank.empty());
  // 2.5: bit identity holds with the load LOD disabled (a slow run, e.g. under TSan,
  // would otherwise switch new voices to the light resampler at different blocks).
  snd_config cfg = Config();
  cfg.tuning.lod_down_blocks = 0;
  Script a, b;
  {
    Rig r(cfg);
    SND_REQUIRE(r.Load(bank));
    RunScript(r, &a);
    snd::music::MusicTransitionRecord log[16];
    const std::uint32_t n = r.music.TransitionLog(log, 16);
    // One -> Two (rule with bridge); One then Two again while pending = cancelled; -> One (When grid).
    SND_CHECK_EQ(n, 2u);
  }
  {
    Rig r(cfg);
    SND_REQUIRE(r.Load(bank));
    RunScript(r, &b);
    snd_stats st{};
    r.sys->GetStats(&st);
    SND_CHECK_EQ(st.lod_level, 0u);
  }
  SND_CHECK(a.starts.size() > 8u);
  SND_CHECK(SameStarts(a.starts, b.starts));
  SND_CHECK(SameBeats(a.beats, b.beats));
  SND_CHECK_EQ(a.markers.size(), b.markers.size());
  SND_REQUIRE_EQ(a.audio.size(), b.audio.size());
  bool same = true;
  for (std::size_t i = 0; i < a.audio.size() && same; ++i) same = std::memcmp(&a.audio[i], &b.audio[i], sizeof(float)) == 0;
  SND_CHECK(same);
  float peak = 0.0f;
  for (float v : a.audio) peak = std::max(peak, std::fabs(v));
  SND_CHECK(peak > 0.05f);
}

namespace {
class CountingAllocator final : public snd::IAllocator {
 public:
  void* Allocate(std::size_t bytes, std::size_t align, snd::MemTag tag) override {
    allocations.fetch_add(1, std::memory_order_relaxed);
    return snd::base::DefaultAllocator().Allocate(bytes, align, tag);
  }
  void Free(void* p, snd::MemTag tag) override { snd::base::DefaultAllocator().Free(p, tag); }
  std::atomic<std::uint64_t> allocations{0};
};
}  // namespace

SND_TEST(MusicSystem, NoAllocationAfterInitialisation) {
  // The music system's state lives in the pool core gives it at Create; the
  // bank is loaded before counting. RT violations (allocation, locks, IO inside
  // RenderBlock) fail the test through the runner's RT guard.
  const std::vector<std::uint8_t> bank = BusyBank();
  SND_REQUIRE(!bank.empty());
  CountingAllocator alloc;
  Rig r(Config(), &alloc);
  SND_REQUIRE(r.Load(bank));
  r.Steps(4);
  r.all.reserve(r.all.size() + 2 * 256 * 3100);
  const std::uint64_t before = alloc.allocations.load();
  Script s;
  RunScript(r, &s);
  SND_CHECK_EQ(alloc.allocations.load(), before);
  SND_CHECK(r.music.DroppedNotifications() == 0u);
}

SND_TEST(MusicSystem, ParallelRenderingKeepsTheMusicDecisions) {
  // Run: ctest --repeat until-fail:20 -R music_system_test (TSan / Release).
  const std::vector<std::uint8_t> bank = BusyBank();
  SND_REQUIRE(!bank.empty());
  Script serial, parallel;
  {
    Rig r;
    SND_REQUIRE(r.Load(bank));
    RunScript(r, &serial, 2000);
  }
  {
    Rig r(Config(2));
    SND_REQUIRE(r.Load(bank));
    RunScript(r, &parallel, 2000);
  }
  SND_CHECK(SameStarts(serial.starts, parallel.starts));
  SND_CHECK(SameBeats(serial.beats, parallel.beats));
}

namespace {
// Forwards to MusicSystem and, once armed, adds a fired stinger input (the input core
// produces for ReservationAction::Stinger, which only the api stage will create).
class StingerInjector final : public snd::music::IMusicSystem {
 public:
  explicit StingerInjector(snd::music::MusicSystem& m) : m_(m) {}
  std::size_t PoolBytes(const snd::music::MusicLimits& l) const noexcept override { return m_.PoolBytes(l); }
  bool Init(void* p, std::size_t b, const snd::music::MusicLimits& l, const snd::music::MusicServices& s) noexcept override {
    return m_.Init(p, b, l, s);
  }
  void Shutdown() noexcept override { m_.Shutdown(); }
  void UpdateTransport(const snd::music::MusicFrame& f, snd::schema::Tables& t, snd::music::FireList& fire) noexcept override {
    m_.UpdateTransport(f, t, fire);
  }
  void RunMusicLogic(const snd::music::MusicFrame& f, const snd::schema::Tables& t, const snd::music::MusicInput* in,
                     std::uint32_t n, snd::music::MusicVoiceRequestList& out) noexcept override {
    std::uint32_t k = 0;
    for (; k < n && k + 1u < kCap; ++k) buf_[k] = in[k];
    if (armed.load() != 0u) {
      armed.store(0u);
      snd::music::MusicInput& x = buf_[k++];
      x = snd::music::MusicInput{};
      x.kind = snd::music::MusicInputKind::Stinger;
      x.playbackSlot = slot;
      x.stateId = stinger;
      x.voiceSlot = snd::kInvalidSlot;
      x.clock = f.block.clock + f.block.frames + 100;  // inside the next block (fired one block early)
      fired = x.clock;
    }
    m_.RunMusicLogic(f, t, buf_, k, out);
  }
  void OnVoiceResults(const snd::music::MusicVoiceResult* r, std::uint32_t n) noexcept override { m_.OnVoiceResults(r, n); }
  std::uint32_t CollectNotifications(snd::notify::EventRecord* o, std::uint32_t c) noexcept override {
    return m_.CollectNotifications(o, c);
  }
  bool TicksToClock(snd::SlotIndex t, snd::MusicalTicks k, snd::ClockSample* o) const noexcept override {
    return m_.TicksToClock(t, k, o);
  }
  bool ClockToTicks(snd::SlotIndex t, snd::ClockSample c, snd::MusicalTicks* o) const noexcept override {
    return m_.ClockToTicks(t, c, o);
  }
  std::atomic<std::uint32_t> armed{0};
  snd::SlotIndex slot = 0;
  snd::Id stinger = 0;
  std::uint64_t fired = 0;

 private:
  static constexpr std::uint32_t kCap = 512;
  snd::music::MusicSystem& m_;
  snd::music::MusicInput buf_[kCap] = {};
};
}  // namespace

SND_TEST(MusicSystem, StingerInputStartsOnItsFireClock) {
  MusicProject p("stinger-input");
  p.Asset("m", Material(4 * kBar, 0.1f));
  p.Asset("st", Material(4800, 0.05f));
  p.Event("St", "\"stinger\": true, \"tracks\": [{\"clips\": [{\"asset\": \"st\"}]}]");
  p.Segment("M", "m", 4);
  p.Playlist("PL", "{\"segment\": \"M\"}");
  p.MusicEvent("Music", std::string(kOneState) +
                            ", \"stingers\": [{\"name\": \"Hit\", \"event\": \"St\", \"timing\": \"nextBeat\"}]");
  const std::vector<std::uint8_t> bank = BuildOne(p);
  SND_REQUIRE(!bank.empty());
  std::unique_ptr<StingerInjector> inj;
  Rig* rp = nullptr;
  // The injector wraps the rig's own MusicSystem.
  struct Holder {
    snd::music::MusicSystem inner;
  };
  Holder h;
  inj = std::make_unique<StingerInjector>(h.inner);
  Rig r(Config(), nullptr, inj.get());
  rp = &r;
  SND_REQUIRE(rp->Load(bank));
  const std::uint64_t s0 = r.Clock();
  const snd_playback_handle m = r.sys->Play(HashId("Music"), snd_emitter_handle{0}, nullptr);
  r.RunTo(s0 + 20000);
  inj->slot = SlotOf(m);
  inj->stinger = HashId("Hit");
  inj->armed.store(1u);
  r.Steps(8);
  std::uint64_t at = UINT64_MAX;
  for (const StartLog& s : r.kind.Starts()) {
    if (s.asset == HashId("st")) at = s.clock;
  }
  SND_REQUIRE(inj->fired != 0u);
  SND_CHECK_EQ(at, inj->fired);
}

SND_TEST(MusicSystem, SeekBackAfterMoreTempoPointsThanTheMapHolds) {
  // D-71: a 40-bar segment with a tempo point on every bar (480 / 960 BPM alternately:
  // bars of 24000 / 12000 samples), played three times: about 90 points have passed
  // (the map holds 64) when the transport is seeked back to bar 2 beat 1. The segment is
  // re-placed from its own data: frame 24000 + 12000 + 6000, announced as bar 2 beat 1,
  // and the beats go on at bar 2's tempo.
  MusicProject p("many-points");
  std::string tempo;
  for (int k = 0; k < 40; ++k) {
    tempo += (k == 0 ? "" : ", ") + std::string("{\"tick\": ") + std::to_string(3840 * k) +
             ", \"bpm\": " + ((k % 2) == 0 ? "480" : "960") + "}";
  }
  p.Asset("many", Material(20 * 24000 + 20 * 12000, 0.2f));
  p.Segment("Many", "many", 40, "\"tempo\": [" + tempo + "]");
  p.Playlist("PL", "{\"segment\": \"Many\"}", "\"loopCount\": 3");
  p.MusicEvent("Music", kOneState);
  const std::vector<std::uint8_t> bank = BuildOne(p);
  SND_REQUIRE(!bank.empty());
  Rig r;
  SND_REQUIRE(r.Load(bank));
  const std::uint64_t s0 = r.Clock();
  const snd_playback_handle m = r.sys->Play(HashId("Music"), snd_emitter_handle{0}, nullptr);
  constexpr std::uint64_t kLoop = 20 * 24000 + 20 * 12000;
  r.RunTo(s0 + 2 * kLoop + 50000);
  {
    const std::vector<StartLog> st = r.kind.Starts();
    SND_REQUIRE_EQ(st.size(), std::size_t{3});
    SND_CHECK_EQ(st[1].clock, s0 + kLoop);
    SND_CHECK_EQ(st[2].clock, s0 + 2 * kLoop);
  }
  const std::uint64_t c = r.Clock();
  const snd_musical_time to{2, 1, 0};
  r.sys->SeekTransport(OwnedTransport(m), &to, nullptr);
  r.RunTo(c + 20000);
  r.sys->Update();
  const std::vector<StartLog> st = r.kind.Starts();
  SND_REQUIRE_EQ(st.size(), std::size_t{4});
  SND_CHECK_EQ(st[3].clock, c);
  SND_CHECK_EQ(st[3].frame, 24000u + 12000u + 6000u);
  bool atSeek = false, next = false;
  for (const Beat& b : r.ev.beats) {
    if (b.transport != OwnedTransport(m).value) continue;
    if (b.clock == c) atSeek = b.bar == 2 && b.beat == 1;
    if (b.clock == c + 6000) next = b.bar == 2 && b.beat == 2;
  }
  SND_CHECK(atSeek);
  SND_CHECK(next);
}
