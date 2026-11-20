// Independent tests, stage 3 group 4: analysis (D-147 meters and loudness per bus,
// D-148 snd_get_spectrum). See indep_g4_fixture.hpp.
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <thread>
#include <vector>

#include "indep_g4_fixture.hpp"

using namespace g4;

namespace {

constexpr std::uint32_t kBins = 513;  // 1024 / 2 + 1

std::vector<float> SineData(std::size_t frames, double hz, float amp) { return Sine(frames, hz, 48000.0, amp); }

// Buses: 0 "A" (meter) -> 4, 1 "Bq" (meter) -> 4, 2 "Plain" -> 4, 3 "Cm" (meter) -> 4, 4 "Master".
B MeterBank() {
  B bk;
  bk.b.buses.clear();
  bk.b.buses.push_back(TBus{HashId("A"), 4u, 0, bf::kBusMeter, 0.0f, {}, {}});
  bk.b.buses.push_back(TBus{HashId("Bq"), 4u, 0, bf::kBusMeter, 0.0f, {}, {}});
  bk.b.buses.push_back(TBus{HashId("Plain"), 4u, 0, 0, 0.0f, {}, {}});
  bk.b.buses.push_back(TBus{HashId("Cm"), 4u, 0, bf::kBusMeter, 0.0f, {}, {}});
  bk.b.buses.push_back(TBus{HashId("Master"), kNone, 2, bf::kBusMaster, 0.0f, {}, {}});
  return bk;
}
// Event `name` on bus row `bus`: a looping 1 kHz sine of amplitude `amp` (4800 frames = 100 periods).
void SineEvent(B& bk, const char* name, std::uint32_t bus, float amp, double hz = 1000.0) {
  char an[48];
  std::snprintf(an, sizeof an, "%s.sine", name);
  TClip c = B::AssetClip(bk.Asset(an, SineData(4800, hz, amp)));
  c.loopCount = 0;
  bk.Event(name, {c}, 0, bus);
}
// BS.1770 of the rendered output over [from, to): at 1 kHz the K filter's gain is +0.691 dB,
// so the loudness is 10 log10(sum of the channels' mean squares).
double LoudnessOf1kHz(const Rig& r, std::size_t from, std::size_t to) {
  double s = 0;
  for (std::uint32_t c = 0; c < r.Ch(); ++c) {
    double ms = 0;
    for (std::size_t f = from; f < to; ++f) ms += double(r.At(f, c)) * r.At(f, c);
    s += ms / double(to - from);
  }
  return 10.0 * std::log10(s);
}
snd_meter Meter(Rig& r, const char* bus) {
  snd_meter m{};
  r.sys->Update();
  const snd_result res = r.sys->GetMeter(HashId(bus), &m);
  if (res != SND_OK) m.channels = 0xFFFFFFFFu;
  return m;
}
// Reads the spectrum every block until it is ready (at most `blocks`); returns the result.
snd_result ReadSpectrum(Rig& r, snd::Id bus, std::uint32_t ch, std::vector<float>& out, std::uint32_t blocks = 12) {
  snd_result res = SND_ERR_NOT_READY;
  for (std::uint32_t i = 0; i <= blocks; ++i) {
    r.sys->Update();
    res = r.sys->GetSpectrum(bus, ch, out.data(), static_cast<std::uint32_t>(out.size()));
    if (res != SND_ERR_NOT_READY) return res;
    r.Render();
  }
  return res;
}

}  // namespace

// ---------------------------------------------------------------------------
// Meters and loudness (D-147)
// ---------------------------------------------------------------------------

// Peak and RMS of a known signal on every bus (metered or not), and 0 elsewhere.
SND_TEST(IndepG4Meter, PeakAndRmsOnEveryBus) {
  Rig r;
  B bk = MeterBank();
  SineEvent(bk, "SP", 2, 0.4f);
  r.Load("b", bk.Build("B"));
  r.sys->Play(HashId("SP"), NoEm(), nullptr);
  r.Steps(400);  // the RMS has a ballistic (it rises over a few hundred ms)
  const snd_meter p = Meter(r, "Plain"), a = Meter(r, "A"), m = Meter(r, "Master");
  snd::test::Note("Plain peak %g rms %g; A peak %g; Master peak %g rms %g", double(p.peak[0]), double(p.rms[0]),
                  double(a.peak[0]), double(m.peak[0]), double(m.rms[0]));
  SND_CHECK_EQ(p.channels, 2u);
  SND_CHECK_NEAR(p.peak[0], 0.4f * kC, 0.004f);
  SND_CHECK_NEAR(p.rms[0], 0.4f * kC / 1.41421356f, 0.003f);
  SND_CHECK_NEAR(p.peak[1], 0.4f * kC, 0.004f);
  SND_CHECK_NEAR(m.peak[0], 0.4f * kC, 0.004f);
  SND_CHECK_NEAR(a.peak[0], 0.0f, 1e-6f);
  SND_CHECK_NEAR(a.rms[0], 0.0f, 1e-6f);
}

// Loudness per bus: a 1 kHz sine on A and a quieter one on Bq read their own BS.1770 values
// (short-term after > 3 s and integrated, constant signal: equal), the output reads the sum.
SND_TEST(IndepG4Meter, LoudnessPerMeteredBus) {
  Rig r;
  B bk = MeterBank();
  SineEvent(bk, "SA", 0, 0.4f);
  SineEvent(bk, "SB", 1, 0.05f);
  r.Load("b", bk.Build("B"));
  r.sys->Play(HashId("SA"), NoEm(), nullptr);
  r.Steps(1);
  // A alone for a reference of the measurement on the output.
  r.Steps(700);  // 3.7 s
  const std::size_t to = r.Now();
  const double wantA = LoudnessOf1kHz(r, to - 144000, to);  // the last 3 s of the output = bus A
  const snd_meter a = Meter(r, "A"), m = Meter(r, "Master");
  snd::test::Note("A: short %g integrated %g (expected %g, 0.4 x centre: %g); Master short %g",
                  double(a.loudness_short), double(a.loudness_integrated), wantA,
                  20.0 * std::log10(0.4) - 3.0103, double(m.loudness_short));
  SND_CHECK_NEAR(a.loudness_short, float(wantA), 0.3f);
  SND_CHECK_NEAR(a.loudness_integrated, float(wantA), 0.3f);
  SND_CHECK_NEAR(m.loudness_short, float(wantA), 0.3f);
  // B on Bq: Bq reads its own (-18 dB lower), A unchanged.
  r.sys->Play(HashId("SB"), NoEm(), nullptr);
  r.Steps(700);
  const snd_meter a2 = Meter(r, "A"), b2 = Meter(r, "Bq");
  const double wantB = wantA + 20.0 * std::log10(0.05 / 0.4);
  snd::test::Note("A %g, Bq short %g integrated %g (expected %g)", double(a2.loudness_short), double(b2.loudness_short),
                  double(b2.loudness_integrated), wantB);
  SND_CHECK_NEAR(a2.loudness_short, float(wantA), 0.3f);
  SND_CHECK_NEAR(b2.loudness_short, float(wantB), 0.3f);
  SND_CHECK_NEAR(b2.loudness_integrated, float(wantB), 0.3f);
}

// A bus without kBusMeter does not get the output's loudness (the stage 2 shortcut D-147
// replaces); records what it reads.
SND_TEST(IndepG4Meter, UnmeteredBusDoesNotReportTheOutputLoudness) {
  Rig r;
  B bk = MeterBank();
  SineEvent(bk, "SA", 0, 0.4f);
  SineEvent(bk, "SP", 2, 0.02f);
  r.Load("b", bk.Build("B"));
  r.sys->Play(HashId("SA"), NoEm(), nullptr);
  r.sys->Play(HashId("SP"), NoEm(), nullptr);
  r.Steps(200);
  const snd_meter p = Meter(r, "Plain"), m = Meter(r, "Master");
  snd::test::Note("Plain loudness short %g integrated %g; Master %g", double(p.loudness_short),
                  double(p.loudness_integrated), double(m.loudness_short));
  SND_CHECK(!(std::fabs(p.loudness_short - m.loudness_short) < 0.05f));
}

// Constant then silence: the short-term loudness falls after the signal stops (3 s window),
// the integrated keeps the gated value (silence is gated out).
SND_TEST(IndepG4Meter, ShortTermFollowsAndIntegratedIsGated) {
  Rig r;
  B bk = MeterBank();
  SineEvent(bk, "SA", 0, 0.4f);
  r.Load("b", bk.Build("B"));
  const snd_playback_handle h = r.sys->Play(HashId("SA"), NoEm(), nullptr);
  r.Steps(700);
  const snd_meter before = Meter(r, "A");
  r.sys->Stop(h, 0.0f, nullptr);
  r.Steps(700);
  const snd_meter after = Meter(r, "A");
  snd::test::Note("before short %g int %g; after short %g int %g", double(before.loudness_short),
                  double(before.loudness_integrated), double(after.loudness_short), double(after.loudness_integrated));
  SND_CHECK(after.loudness_short < before.loudness_short - 40.0f || std::isinf(after.loudness_short));
  SND_CHECK_NEAR(after.loudness_integrated, before.loudness_integrated, 0.3f);
}

// More metered buses than CoreLimits.maxMeteredBuses: the extra ones are not measured
// (spectrum UNSUPPORTED), nothing fails; unloading the bank returns the slots.
SND_TEST(IndepG4Meter, MeteredBusLimitAndSlotsComeBackAfterUnload) {
  snd::core::CoreLimits lim;
  lim.maxMeteredBuses = 2;
  Rig r(Config(), lim);
  B bk = MeterBank();  // three metered buses: A, Bq, Cm
  SineEvent(bk, "SA", 0, 0.4f);
  SineEvent(bk, "SB", 1, 0.4f);
  SineEvent(bk, "SC", 3, 0.4f);
  const snd_bank_handle bank = r.Load("b", bk.Build("B"));
  SND_REQUIRE_EQ(r.sys->GetBankState(bank), SND_BANK_STATE_LOADED);
  for (const char* e : {"SA", "SB", "SC"}) r.sys->Play(HashId(e), NoEm(), nullptr);
  std::vector<float> out(kBins);
  int measured = 0, unsupported = 0;
  for (const char* bus : {"A", "Bq", "Cm"}) {
    const snd_result res = ReadSpectrum(r, HashId(bus), 0, out);
    snd::test::Note("bus %s: spectrum %d", bus, int(res));
    measured += res == SND_OK ? 1 : 0;
    unsupported += res == SND_ERR_UNSUPPORTED ? 1 : 0;
  }
  SND_CHECK_EQ(measured, 2);
  SND_CHECK_EQ(unsupported, 1);
  // The output always has one.
  SND_CHECK_EQ(ReadSpectrum(r, SND_ID_NONE, 0, out), SND_OK);
  r.sys->UnloadBank(bank, SND_UNLOAD_STOP_AND_UNLOAD);
  r.Steps(10);
  B b2;
  b2.b.buses.clear();
  b2.b.buses.push_back(TBus{HashId("D1"), 2u, 0, bf::kBusMeter, 0.0f, {}, {}});
  b2.b.buses.push_back(TBus{HashId("D2"), 2u, 0, bf::kBusMeter, 0.0f, {}, {}});
  b2.b.buses.push_back(TBus{HashId("Master"), kNone, 2, bf::kBusMaster, 0.0f, {}, {}});
  SineEvent(b2, "S1", 0, 0.4f);
  SineEvent(b2, "S2", 1, 0.4f);
  const snd_bank_handle bank2 = r.Load("b2", b2.Build("B2"));
  SND_REQUIRE_EQ(r.sys->GetBankState(bank2), SND_BANK_STATE_LOADED);
  r.sys->Play(HashId("S1"), NoEm(), nullptr);
  r.sys->Play(HashId("S2"), NoEm(), nullptr);
  r.Steps(8);
  SND_CHECK_EQ(ReadSpectrum(r, HashId("D1"), 0, out), SND_OK);
  SND_CHECK_EQ(ReadSpectrum(r, HashId("D2"), 0, out), SND_OK);
  SND_CHECK_NEAR(*std::max_element(out.begin(), out.end()), 0.4f * kC, 0.02f);
  SND_CHECK_EQ(ReadSpectrum(r, HashId("A"), 0, out), SND_ERR_NOT_FOUND);
}

// ---------------------------------------------------------------------------
// Spectrum (D-148)
// ---------------------------------------------------------------------------

// A sine on bin 64 (3000 Hz): that bin reads its amplitude, far bins nothing; the output
// and SND_ID_NONE read the same; the bus reads its own signal.
SND_TEST(IndepG4Spectrum, SineOnABinReadsItsAmplitude) {
  Rig r;
  B bk = MeterBank();
  SineEvent(bk, "SA", 0, 0.8f, 3000.0);
  r.Load("b", bk.Build("B"));
  r.sys->Play(HashId("SA"), NoEm(), nullptr);
  r.Steps(8);
  std::vector<float> out(kBins), master(kBins);
  SND_REQUIRE_EQ(ReadSpectrum(r, HashId("A"), 0, out), SND_OK);
  SND_REQUIRE_EQ(r.sys->GetSpectrum(SND_ID_NONE, 1, master.data(), kBins), SND_OK);
  std::vector<float> byId(kBins);
  SND_REQUIRE_EQ(r.sys->GetSpectrum(HashId("Master"), 1, byId.data(), kBins), SND_OK);
  SND_CHECK(std::memcmp(master.data(), byId.data(), kBins * sizeof(float)) == 0);
  const std::size_t peak = std::size_t(std::max_element(out.begin(), out.end()) - out.begin());
  snd::test::Note("peak bin %zu = %g (expected 64 = %g); bins 10 %g, 200 %g", peak, double(out[peak]),
                  double(0.8f * kC), double(out[10]), double(out[200]));
  SND_CHECK_EQ(peak, 64u);
  SND_CHECK_NEAR(out[64], 0.8f * kC, 0.8f * kC * 0.02f);
  SND_CHECK_NEAR(master[64], 0.8f * kC, 0.8f * kC * 0.02f);
  SND_CHECK(out[10] < 1e-3f && out[200] < 1e-3f);
  // Hann: the neighbours of an on-bin sine read half, the next ones nothing.
  SND_CHECK_NEAR(out[63], 0.5f * out[64], 0.02f * out[64]);
  SND_CHECK_NEAR(out[65], 0.5f * out[64], 0.02f * out[64]);
  SND_CHECK(out[62] < 0.01f * out[64] && out[66] < 0.01f * out[64]);
  // The other buses are quiet.
  SND_REQUIRE_EQ(ReadSpectrum(r, HashId("Bq"), 0, out), SND_OK);
  SND_CHECK(*std::max_element(out.begin(), out.end()) < 1e-6f);
}

// Half a bin off (64.5 bins): Hann reads 0.849 of the amplitude in both bins (a rectangular
// window 0.637), so the window is Hann.
SND_TEST(IndepG4Spectrum, HalfBinOffShowsTheHannWindow) {
  Rig r;
  B bk = MeterBank();
  TClip c = B::AssetClip(bk.Asset("s", SineData(96000, 64.5 * 48000.0 / 1024.0, 1.0f)));
  bk.Event("S", {c}, 0, 0);
  r.Load("b", bk.Build("B"));
  r.sys->Play(HashId("S"), NoEm(), nullptr);
  r.Steps(100);
  std::vector<float> out(kBins);
  SND_REQUIRE_EQ(ReadSpectrum(r, HashId("A"), 0, out), SND_OK);
  snd::test::Note("bins 64 %g, 65 %g (Hann 0.849 x %g)", double(out[64]), double(out[65]), double(kC));
  SND_CHECK_NEAR(out[64] / kC, 0.8488f, 0.02f);
  SND_CHECK_NEAR(out[65] / kC, 0.8488f, 0.02f);
}

// Bin count larger than fft / 2 + 1: the rest is 0; smaller: only that many written.
SND_TEST(IndepG4Spectrum, BinCountLargerAndSmaller) {
  Rig r;
  B bk = MeterBank();
  SineEvent(bk, "SA", 0, 0.8f, 3000.0);
  r.Load("b", bk.Build("B"));
  r.sys->Play(HashId("SA"), NoEm(), nullptr);
  r.Steps(8);
  std::vector<float> big(700, -1.0f);
  SND_REQUIRE_EQ(ReadSpectrum(r, HashId("A"), 0, big), SND_OK);
  int restNonZero = 0, negative = 0;
  for (std::size_t i = kBins; i < big.size(); ++i) restNonZero += big[i] != 0.0f ? 1 : 0;
  for (std::size_t i = 0; i < kBins; ++i) negative += big[i] < 0.0f ? 1 : 0;
  SND_CHECK_EQ(restNonZero, 0);
  SND_CHECK_EQ(negative, 0);
  std::vector<float> small(80, -1.0f);
  SND_REQUIRE_EQ(r.sys->GetSpectrum(HashId("A"), 0, small.data(), 70), SND_OK);
  SND_CHECK_NEAR(small[64], big[64], 1e-6f);
  for (std::size_t i = 70; i < 80; ++i) SND_CHECK_EQ(small[i], -1.0f);
  std::vector<float> one(2, -1.0f);
  SND_REQUIRE_EQ(r.sys->GetSpectrum(HashId("A"), 0, one.data(), 1), SND_OK);
  SND_CHECK_EQ(one[1], -1.0f);
}

// Errors: bin_count 0 / NULL -> INVALID_ARGUMENT; unknown bus -> NOT_FOUND; no meter or a
// channel not below the bus's -> UNSUPPORTED.
SND_TEST(IndepG4Spectrum, Errors) {
  Rig r;
  B bk = MeterBank();
  SineEvent(bk, "SA", 0, 0.8f, 3000.0);
  r.Load("b", bk.Build("B"));
  r.sys->Play(HashId("SA"), NoEm(), nullptr);
  r.Steps(8);
  std::vector<float> out(kBins);
  SND_REQUIRE_EQ(ReadSpectrum(r, HashId("A"), 0, out), SND_OK);
  SND_CHECK_EQ(r.sys->GetSpectrum(HashId("A"), 0, out.data(), 0), SND_ERR_INVALID_ARGUMENT);
  SND_CHECK_EQ(r.sys->GetSpectrum(HashId("A"), 0, nullptr, kBins), SND_ERR_INVALID_ARGUMENT);
  SND_CHECK_EQ(r.sys->GetSpectrum(HashId("Nope"), 0, out.data(), kBins), SND_ERR_NOT_FOUND);
  SND_CHECK_EQ(r.sys->GetSpectrum(HashId("Plain"), 0, out.data(), kBins), SND_ERR_UNSUPPORTED);
  SND_CHECK_EQ(r.sys->GetSpectrum(HashId("A"), 2, out.data(), kBins), SND_ERR_UNSUPPORTED);
  SND_CHECK_EQ(r.sys->GetSpectrum(SND_ID_NONE, 2, out.data(), kBins), SND_ERR_UNSUPPORTED);
  SND_CHECK_EQ(r.sys->GetSpectrum(HashId("A"), 0xFFFFFFFFu, out.data(), kBins), SND_ERR_UNSUPPORTED);
}

// NOT_READY: the first read of a new system, then data within a few updates; kept up to date
// while it is read; after ~1 s without a read NOT_READY again; 0.5 s is not enough.
SND_TEST(IndepG4Spectrum, NotReadyProtocol) {
  Rig r;
  B bk = MeterBank();
  SineEvent(bk, "SA", 0, 0.8f, 3000.0);
  r.Load("b", bk.Build("B"));
  r.sys->Play(HashId("SA"), NoEm(), nullptr);
  r.Steps(250);  // > 1 s without a read
  std::vector<float> out(kBins);
  SND_CHECK_EQ(r.sys->GetSpectrum(HashId("A"), 0, out.data(), kBins), SND_ERR_NOT_READY);
  int updates = 0;
  snd_result res = SND_ERR_NOT_READY;
  while (res == SND_ERR_NOT_READY && updates < 20) {
    r.sys->Update();
    ++updates;
    res = r.sys->GetSpectrum(HashId("A"), 0, out.data(), kBins);
    if (res == SND_ERR_NOT_READY) r.Render();
  }
  snd::test::Note("ready after %d update(s) (comment: from the next snd_update())", updates);
  SND_CHECK_EQ(res, SND_OK);
  SND_CHECK(updates <= 6);
  SND_CHECK_NEAR(out[64], 0.8f * kC, 0.02f);
  // Read every block for 2 s: always OK.
  int notOk = 0;
  for (int i = 0; i < 375; ++i) {
    r.Step();
    notOk += r.sys->GetSpectrum(HashId("A"), 0, out.data(), kBins) != SND_OK ? 1 : 0;
  }
  SND_CHECK_EQ(notOk, 0);
  // 0.5 s pause: still OK.
  r.Steps(94);
  SND_CHECK_EQ(r.sys->GetSpectrum(HashId("A"), 0, out.data(), kBins), SND_OK);
  // 1.5 s pause: NOT_READY.
  r.Steps(282);
  SND_CHECK_EQ(r.sys->GetSpectrum(HashId("A"), 0, out.data(), kBins), SND_ERR_NOT_READY);
}

// The data follows the signal: after the sine stops the bins fall to 0 (as of the latest update).
SND_TEST(IndepG4Spectrum, FollowsTheSignal) {
  Rig r;
  B bk = MeterBank();
  SineEvent(bk, "SA", 0, 0.8f, 3000.0);
  r.Load("b", bk.Build("B"));
  const snd_playback_handle h = r.sys->Play(HashId("SA"), NoEm(), nullptr);
  std::vector<float> out(kBins);
  SND_REQUIRE_EQ(ReadSpectrum(r, HashId("A"), 0, out), SND_OK);
  r.sys->Stop(h, 0.0f, nullptr);
  for (int i = 0; i < 8; ++i) {
    r.Step();
    r.sys->GetSpectrum(HashId("A"), 0, out.data(), kBins);
  }
  r.sys->Update();
  SND_CHECK_EQ(r.sys->GetSpectrum(HashId("A"), 0, out.data(), kBins), SND_OK);
  SND_CHECK(*std::max_element(out.begin(), out.end()) < 1e-6f);
}

// Several threads read spectra and meters while the owner updates and renders (TSan):
// every read is OK or NOT_READY and every OK read is a plausible spectrum.
SND_TEST(IndepG4Spectrum, ConcurrentReadersDuringUpdates) {
  Rig r;
  B bk = MeterBank();
  SineEvent(bk, "SA", 0, 0.8f, 3000.0);
  r.Load("b", bk.Build("B"));
  r.sys->Play(HashId("SA"), NoEm(), nullptr);
  std::atomic<bool> stop{false};
  std::atomic<std::uint64_t> ok{0}, bad{0};
  std::vector<std::thread> readers;
  for (int t = 0; t < 3; ++t) {
    readers.emplace_back([&, t] {
      std::vector<float> out(kBins);
      while (!stop.load()) {
        const snd::Id bus = t == 0 ? SND_ID_NONE : HashId("A");
        const snd_result res = r.sys->GetSpectrum(bus, std::uint32_t(t & 1), out.data(), kBins);
        if (res == SND_OK) {
          bool sane = true;
          for (float v : out) sane = sane && std::isfinite(v) && v >= 0.0f && v < 2.0f;
          sane = sane && out[64] <= 0.8f * kC * 1.05f;  // the first windows hold the start
          (sane ? ok : bad).fetch_add(1);
        } else if (res != SND_ERR_NOT_READY) {
          bad.fetch_add(1);
        }
        snd_meter m{};
        if (r.sys->GetMeter(HashId("A"), &m) == SND_OK && !(m.peak[0] >= 0.0f && m.peak[0] < 2.0f)) bad.fetch_add(1);
      }
    });
  }
  r.Steps(400);
  stop.store(true);
  for (std::thread& t : readers) t.join();
  snd::test::Note("ok %llu bad %llu", static_cast<unsigned long long>(ok.load()), static_cast<unsigned long long>(bad.load()));
  SND_CHECK_EQ(bad.load(), 0u);
  SND_CHECK(ok.load() > 0u);
}
