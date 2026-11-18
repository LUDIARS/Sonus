// Transports and reservations through core with the music system (8.1, 8.2):
// quantised Play on every When kind, tempo changes and seeks that move pending
// targets, pause / resume / stop, beat notices, lead time and the late rule,
// destroyed transports, clock kinds, PICKUP_ALIGN, and the clock <-> tick
// conversions of IMusicSystem.
#include <cmath>
#include <string>
#include <vector>

#include "music_fixture.hpp"

using namespace mt;

namespace {

// Events H0..H4 play assets h0..h4 (mono DC 0.5, 2400 frames). "Up" plays asset "up",
// which is also segment SUp's material (pickup 6000 frames before bar 0).
std::vector<std::uint8_t> TransportBank() {
  MusicProject p("transport");
  for (int i = 0; i < 5; ++i) {
    const std::string n = std::to_string(i);
    p.Asset("h" + n, Material(2400, 0.5f));
    p.Event("H" + n, "\"tracks\": [{\"clips\": [{\"asset\": \"h" + n + "\"}]}]");
  }
  p.Asset("up", Material(30000, 0.5f));
  p.Event("Up", "\"tracks\": [{\"clips\": [{\"asset\": \"up\"}]}]");
  p.Segment("SUp", "up", 1, "\"entryFrame\": 6000");
  p.Playlist("PL", "{\"segment\": \"SUp\"}");
  p.MusicEvent("Music", "\"initialState\": \"S\", \"states\": [{\"name\": \"S\", \"playlist\": \"PL\"}]");
  // "Meter": A (1 bar 4/4) then B (2 bars 3/4, 120 BPM: 72000 samples a bar).
  p.Asset("ma", Material(kBar, 0.1f));
  p.Asset("mb", Material(2 * 72000, 0.2f));
  p.Segment("MA", "ma", 1);
  p.Segment("MB", "mb", 2, "\"tempo\": [{\"tick\": 0, \"bpm\": 120, \"beatsPerBar\": 3}]");
  p.Playlist("PM", "{\"segment\": \"MA\"}, {\"segment\": \"MB\"}", "\"loopCount\": 1");
  p.MusicEvent("Meter", "\"initialState\": \"M\", \"states\": [{\"name\": \"M\", \"playlist\": \"PM\"}]");
  std::string diag;
  std::vector<std::uint8_t> bank = p.Build(&diag);
  if (bank.empty()) std::fprintf(stderr, "%s\n", diag.c_str());
  return bank;
}

struct T {
  Rig r;
  snd_transport_handle t{};
  std::uint64_t t0 = 0;  // clock of the transport's tick 0
  explicit T(float bpm = 120.0f) {
    if (r.sys == nullptr || !r.Load(TransportBank())) return;
    snd_transport_desc d{bpm, 4, 4, 1, 0};
    t = r.sys->CreateTransport(&d);
    t0 = r.Clock();
    r.Step();
  }
  bool ok() const { return t.value != 0u; }
  snd_playback_handle Play(const char* ev, const snd_when& w) {
    return r.sys->Play(HashId(ev), snd_emitter_handle{0}, &w);
  }
  // Clock of the first start of asset `a` (UINT64_MAX if none).
  std::uint64_t StartOf(const char* a) const {
    for (const StartLog& s : r.kind.Starts()) {
      if (s.asset == HashId(a)) return s.clock;
    }
    return UINT64_MAX;
  }
  snd::SlotIndex Slot() const { return static_cast<snd::SlotIndex>(t.value & 0xFFFFFFFFu); }
};

}  // namespace

SND_TEST(MusicTransport, QuantisedPlaysStartOnTheirGrid) {
  T x;
  SND_REQUIRE(x.ok());
  x.r.RunTo(x.t0 + 30000);  // requests applied at t0 + 30208
  x.Play("H0", Kind(x.t, SND_WHEN_NEXT_BEAT));
  x.Play("H1", Kind(x.t, SND_WHEN_NEXT_BAR));
  x.Play("H2", Kind(x.t, SND_WHEN_BARS_AFTER, 2));
  x.Play("H3", Kind(x.t, SND_WHEN_GRID, 8));
  snd_when w = Kind(x.t, SND_WHEN_NEXT_BAR);
  w.offset_ticks = -480;  // an eighth earlier (auftakt by hand)
  x.Play("H4", w);
  x.r.RunTo(x.t0 + 3 * kBar);
  SND_CHECK_EQ(x.StartOf("h0"), x.t0 + 48000);
  SND_CHECK_EQ(x.StartOf("h1"), x.t0 + kBar);
  SND_CHECK_EQ(x.StartOf("h2"), x.t0 + 2 * kBar);
  SND_CHECK_EQ(x.StartOf("h3"), x.t0 + 36000);
  SND_CHECK_EQ(x.StartOf("h4"), x.t0 + kBar - 12000);
  // The audio follows (resampler: within one sample).
  SND_CHECK(Diff(Crossing(x.r, x.t0 + 47900, x.t0 + 48100, 0.25f), x.t0 + 48000) <= 1u);
}

SND_TEST(MusicTransport, BeatsOfAStandaloneTransport) {
  T x;
  SND_REQUIRE(x.ok());
  x.r.RunTo(x.t0 + 2 * kBar + 10);
  x.r.sys->Update();
  std::uint32_t n = 0;
  for (const Beat& b : x.r.ev.beats) {
    SND_CHECK_EQ(b.transport, x.t.value);
    SND_CHECK_EQ(b.clock, x.t0 + n * kBeat);
    SND_CHECK_EQ(b.bar, static_cast<std::int32_t>(n / 4));
    SND_CHECK_EQ(b.beat, static_cast<std::int32_t>(n % 4));
    ++n;
  }
  SND_CHECK_EQ(n, 9u);
  snd::MusicalTicks tick = 0;
  SND_CHECK(x.r.music.ClockToTicks(x.Slot(), x.t0 + 24000, &tick));
  SND_CHECK_EQ(tick, 960);
  snd::ClockSample c = 0;
  SND_CHECK(x.r.music.TicksToClock(x.Slot(), 1920 * 3, &c));
  SND_CHECK_EQ(c, x.t0 + 3 * 48000);
  snd_position pos{};
  SND_CHECK_EQ(x.r.sys->GetTransportPosition(x.t, &pos), SND_OK);
  SND_CHECK_EQ(pos.musical.bar, 2);
}

SND_TEST(MusicTransport, TempoChangeMovesPendingTargetsInMusicTime) {
  T x;
  SND_REQUIRE(x.ok());
  x.r.RunTo(x.t0 + 1000);
  x.Play("H0", Kind(x.t, SND_WHEN_BARS_AFTER, 2));  // tick 7680
  x.r.RunTo(x.t0 + 30000);
  const std::uint64_t c = x.r.Clock();
  x.r.sys->SetTempo(x.t, 60.0f, nullptr);  // now
  x.r.RunTo(x.t0 + 5 * kBar);
  const double tickAtC = static_cast<double>(c - x.t0) * 120.0 / 60.0 * 960.0 / 48000.0;
  const double expect = static_cast<double>(c) + (7680.0 - tickAtC) / (60.0 / 60.0 * 960.0 / 48000.0);
  SND_CHECK_EQ(x.StartOf("h0"), static_cast<std::uint64_t>(std::floor(expect + 0.5)));
  // Beats after the change are 48000 apart.
  x.r.sys->Update();
  std::uint64_t prev = 0;
  for (const Beat& b : x.r.ev.beats) {
    if (b.clock > c && prev > c) SND_CHECK_MSG(b.clock - prev == 48000u, "beat %d.%d at %llu after %llu", b.bar, b.beat,
                                               static_cast<unsigned long long>(b.clock - x.t0),
                                               static_cast<unsigned long long>(prev - x.t0));
    prev = b.clock;
  }
}

SND_TEST(MusicTransport, SetTempoOnTheNextBar) {
  T x;
  SND_REQUIRE(x.ok());
  x.r.RunTo(x.t0 + 30000);
  snd_when w = Kind(x.t, SND_WHEN_NEXT_BAR);
  x.r.sys->SetTempo(x.t, 240.0f, &w);
  x.Play("H0", Kind(x.t, SND_WHEN_BARS_AFTER, 2));  // bar 2: 1 bar at 120 + 1 bar at 240
  x.r.RunTo(x.t0 + 3 * kBar);
  SND_CHECK_EQ(x.StartOf("h0"), x.t0 + kBar + kBar / 2);
  x.r.sys->Update();
  std::vector<std::uint64_t> at;
  for (const Beat& b : x.r.ev.beats) at.push_back(b.clock - x.t0);
  SND_REQUIRE(at.size() >= 8u);
  SND_CHECK_EQ(at[3], 72000u);
  SND_CHECK_EQ(at[4], 96000u);   // bar 1 at the old tempo
  SND_CHECK_EQ(at[5], 108000u);  // then 12000 apart
  SND_CHECK_EQ(at[6], 120000u);
}

SND_TEST(MusicTransport, SeekRenumbersAndPassedTargetsTakeTheLateRule) {
  T x;
  SND_REQUIRE(x.ok());
  x.r.RunTo(x.t0 + 1000);
  const snd_playback_handle p = x.Play("H0", Kind(x.t, SND_WHEN_NEXT_BAR));  // tick 3840
  x.r.RunTo(x.t0 + 30000);
  const std::uint64_t c = x.r.Clock();
  snd_musical_time to{1, 2, 0};  // tick 5760: the target is behind now
  x.r.sys->SeekTransport(x.t, &to, nullptr);
  x.r.RunTo(x.t0 + 3 * kBar);
  // Next grid point of the same kind after the target: bar 2 = tick 7680, 1920 ticks after the seek.
  SND_CHECK_EQ(x.StartOf("h0"), c + 48000);
  x.r.sys->Update();
  bool late = false;
  for (const ErrorRec& e : x.r.ev.errors) late = late || e.object == p.value;
  SND_CHECK(late);
  bool renumbered = false;
  for (const Beat& b : x.r.ev.beats) {
    if (b.clock == c) renumbered = b.bar == 1 && b.beat == 2;
  }
  SND_CHECK(renumbered);
}

SND_TEST(MusicTransport, PauseHoldsTheTargetsAndStopRewinds) {
  T x;
  SND_REQUIRE(x.ok());
  x.r.RunTo(x.t0 + 1000);
  x.Play("H0", Kind(x.t, SND_WHEN_NEXT_BAR));  // tick 3840
  x.r.RunTo(x.t0 + 50000);
  const std::uint64_t cp = x.r.Clock();
  x.r.sys->SetTransportState(x.t, SND_TRANSPORT_PAUSED, nullptr);
  x.r.RunTo(x.t0 + 200000);
  SND_CHECK_EQ(x.StartOf("h0"), UINT64_MAX);
  const std::uint64_t cr = x.r.Clock();
  x.r.sys->SetTransportState(x.t, SND_TRANSPORT_PLAYING, nullptr);
  x.r.RunTo(x.t0 + 400000);
  // Held at the pause, resumed at cr: the bar line comes (96000 - (cp - t0)) samples after cr.
  SND_CHECK_EQ(x.StartOf("h0"), cr + (kBar - (cp - x.t0)));
  x.r.sys->Update();
  for (const Beat& b : x.r.ev.beats) SND_CHECK(b.clock < cp || b.clock >= cr);
  x.r.sys->SetTransportState(x.t, SND_TRANSPORT_STOPPED, nullptr);
  x.r.Steps(2);
  x.r.sys->Update();
  snd_position pos{};
  SND_CHECK_EQ(x.r.sys->GetTransportPosition(x.t, &pos), SND_OK);
  SND_CHECK_EQ(pos.musical.bar, 0);
  SND_CHECK_EQ(pos.musical.beat, 0);
}

SND_TEST(MusicTransport, LeadTimeDefersToTheNextBeat) {
  T x;
  SND_REQUIRE(x.ok());
  x.r.kind.primeFrames.store(30000);
  x.r.kind.primeAsset.store(HashId("h0"));
  x.r.RunTo(x.t0 + 30000);
  const snd_playback_handle p = x.Play("H0", Kind(x.t, SND_WHEN_NEXT_BEAT));  // 17792 samples left < 30000
  x.Play("H1", Kind(x.t, SND_WHEN_NEXT_BEAT));                              // memory material: no lead
  x.r.RunTo(x.t0 + 2 * kBar);
  SND_CHECK_EQ(x.StartOf("h0"), x.t0 + 72000);
  SND_CHECK_EQ(x.StartOf("h1"), x.t0 + 48000);
  x.r.sys->Update();
  bool late = false;
  for (const ErrorRec& e : x.r.ev.errors) late = late || e.object == p.value;
  SND_CHECK(late);
}

SND_TEST(MusicTransport, DestroyedTransportEndsItsReservations) {
  T x;
  SND_REQUIRE(x.ok());
  const snd_playback_handle p = x.Play("H0", Kind(x.t, SND_WHEN_BARS_AFTER, 4));
  x.r.Steps(4);
  SND_CHECK_EQ(x.r.sys->DestroyTransport(x.t), SND_OK);
  x.r.Steps(4);
  x.r.sys->Update();
  bool ended = false;
  for (const EndRec& e : x.r.ev.ends) {
    ended = ended || (e.handle == p.value && e.reason == SND_END_REJECTED && e.detail == SND_END_DETAIL_TRANSPORT_GONE);
  }
  SND_CHECK(ended);
  SND_CHECK_EQ(x.StartOf("h0"), UINT64_MAX);
}

SND_TEST(MusicTransport, ClockKinds) {
  T x;
  SND_REQUIRE(x.ok());
  x.r.RunTo(x.t0 + 10000);
  const std::uint64_t c = x.r.Clock();
  snd_when a{};
  a.kind = SND_WHEN_CLOCK_SAMPLE;
  a.value = x.t0 + 50000;
  x.Play("H0", a);
  snd_when d{};
  d.kind = SND_WHEN_DELAY_US;
  d.value = 100000;  // 4800 samples
  x.Play("H1", d);
  snd_when past{};
  past.kind = SND_WHEN_CLOCK_SAMPLE;
  past.value = 10;
  const snd_playback_handle pp = x.Play("H2", past);
  x.r.RunTo(x.t0 + kBar);
  SND_CHECK_EQ(x.StartOf("h0"), x.t0 + 50000);
  SND_CHECK_EQ(x.StartOf("h1"), c + 4800);
  SND_CHECK_EQ(x.StartOf("h2"), c);
  x.r.sys->Update();
  bool late = false;
  for (const ErrorRec& e : x.r.ev.errors) late = late || e.object == pp.value;
  SND_CHECK(late);
}

SND_TEST(MusicTransport, PickupAlignStartsEarlyByThePickup) {
  T x;
  SND_REQUIRE(x.ok());
  x.r.RunTo(x.t0 + 1000);
  snd_when w = Kind(x.t, SND_WHEN_NEXT_BAR);
  w.flags = SND_WHEN_FLAG_PICKUP_ALIGN;
  x.Play("Up", w);
  x.r.RunTo(x.t0 + 2 * kBar);
  SND_CHECK_EQ(x.StartOf("up"), x.t0 + kBar - 6000);
}

SND_TEST(MusicTransport, MusicPlaybackQuantisedOnAnotherTransportWithPickup) {
  // A music event started on the next bar of a standalone transport, pickup aligned:
  // its own transport's bar 0 coincides with that bar line.
  T x;
  SND_REQUIRE(x.ok());
  x.r.RunTo(x.t0 + 1000);
  snd_when w = Kind(x.t, SND_WHEN_NEXT_BAR);
  w.flags = SND_WHEN_FLAG_PICKUP_ALIGN;
  const snd_playback_handle m = x.Play("Music", w);
  x.r.RunTo(x.t0 + 2 * kBar);
  SND_CHECK_EQ(x.StartOf("up"), x.t0 + kBar - 6000);
  x.r.sys->Update();
  bool bar0 = false;
  for (const Beat& b : x.r.ev.beats) {
    if (b.transport == OwnedTransport(m).value && b.bar == 0 && b.beat == 0) bar0 = b.clock == x.t0 + kBar;
  }
  SND_CHECK(bar0);
}

SND_TEST(MusicTransport, MusicApiOnANormalPlaybackWarns) {
  T x;
  SND_REQUIRE(x.ok());
  const snd_playback_handle p = x.r.sys->Play(HashId("H0"), snd_emitter_handle{0}, nullptr);
  x.r.Step();
  x.r.sys->SetMusicState(p, HashId("S"), nullptr);
  x.r.Steps(2);
  x.r.sys->Update();
  bool warned = false;
  for (const ErrorRec& e : x.r.ev.errors) warned = warned || (e.object == p.value && e.detail == SND_END_DETAIL_WRONG_EVENT_KIND);
  SND_CHECK(warned);
}

SND_TEST(MusicTransport, ReservationsFollowSegmentsPlannedLater) {
  // D-71: BARS_AFTER 2 is accepted while only A (4/4) is planned; B (3/4) is planned
  // half a second before A's end. The target is "bar 2", resolved on the map in effect
  // when it fires: 1 bar of 4/4 + 1 bar of 3/4.
  T x;
  SND_REQUIRE(x.ok());
  const std::uint64_t m0 = x.r.Clock();
  const snd_playback_handle m = x.r.sys->Play(HashId("Meter"), snd_emitter_handle{0}, nullptr);
  x.r.RunTo(m0 + 10000);
  x.Play("H0", Kind(OwnedTransport(m), SND_WHEN_BARS_AFTER, 2));
  x.Play("H1", Kind(OwnedTransport(m), SND_WHEN_BARS_AFTER, 3));
  x.r.RunTo(m0 + 4 * kBar);
  SND_CHECK_EQ(x.StartOf("mb"), m0 + kBar);
  SND_CHECK_EQ(x.StartOf("h0"), m0 + kBar + 72000);
  SND_CHECK_EQ(x.StartOf("h1"), m0 + kBar + 2 * 72000);
}

SND_TEST(MusicTransport, ReservationOnAStoppedTransportFiresWhenItStarts) {
  // D-79: NEXT_BAR on a Stopped transport resolves to its start position (bar 0) and
  // fires the moment it starts playing.
  T x;
  SND_REQUIRE(x.ok());
  snd_transport_desc d{120.0f, 4, 4, 0, 0};
  const snd_transport_handle t2 = x.r.sys->CreateTransport(&d);
  x.r.Step();
  snd_when w = Kind(t2, SND_WHEN_NEXT_BAR);
  x.r.sys->Play(HashId("H2"), snd_emitter_handle{0}, &w);
  x.r.Steps(50);
  SND_CHECK_EQ(x.StartOf("h2"), UINT64_MAX);
  const std::uint64_t c = x.r.Clock();
  x.r.sys->SetTransportState(t2, SND_TRANSPORT_PLAYING, nullptr);
  x.r.Steps(4);
  SND_CHECK_EQ(x.StartOf("h2"), c);
}
