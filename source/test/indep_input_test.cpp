// Independent tests of the Input kind and its ports (stage 7-D, CONTRACTS.md D-194),
// written from the specification: bit-exact delivery at zero delay, the live start, the
// writer-side overrun and the reader-side latency bound, underrun accounting, the ring
// wrapping over long runs, channel mismatches, one reader per port, virtual / control
// handling, and the SPSC ring under real threads (run under ThreadSanitizer with
//   ctest -j1 --repeat until-fail:20 -R indep_input_test).
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <thread>
#include <vector>

#include "indep_src1_util.hpp"
#include "snd/test/test.hpp"
#include "synth_driver.hpp"

using synthtest::Driver;
namespace src = snd::source;

namespace {

src::InputParams In(std::uint32_t device, std::uint32_t channels = 2) {
  src::InputParams p{};
  p.desc.device_index = device;
  p.desc.channels = channels;
  return p;
}

Driver::Opt Ports(std::uint32_t frames, std::uint32_t ports = 2, std::uint32_t channels = 2) {
  Driver::Opt o;
  o.inputPorts = ports;
  o.inputPortFrames = frames;
  o.inputPortChannels = channels;
  return o;
}

// Writes values base + 1 .. base + n (channel 1 negated) and returns the frames accepted.
std::uint32_t WriteSeq(src::InputPort& port, std::uint64_t base, std::uint32_t n) {
  std::vector<float> a(n), b(n);
  for (std::uint32_t i = 0; i < n; ++i) {
    a[i] = static_cast<float>(base + i + 1u);
    b[i] = -a[i];
  }
  const float* ch[2] = {a.data(), b.data()};
  return port.Write(ch, 2, n);
}

std::uint64_t Invariant(const src::InputPort& port) {  // written - read - skipped - buffered (0 when quiet)
  const src::InputPortStats s = port.Stats();
  return s.writtenFrames - s.readFrames - s.skippedFrames - port.Buffered();
}

}  // namespace

// I1 / I12: what is written comes out bit for bit at zero delay, including special values,
// over a long run in odd chunk sizes that wrap the (small) ring many times.
SND_TEST(IndepInput, BitExactAcrossManyWraps) {
  Driver d(snd::SourceKind::Input, Ports(100));  // NextPow2(100) = 128 frames
  SND_REQUIRE(d.Ok());
  src::InputPort& port = *d.runtime.InputPortOf(0);
  SND_CHECK_EQ(port.Capacity(), 128u);
  src::InputParams p = In(0);
  Driver::Voice v;
  SND_REQUIRE(d.Start(v, &p, 2));
  const float specials[] = {0.0f, -0.0f, 1e-40f, -1e-40f, 1e30f, -3.5f, std::numeric_limits<float>::min(),
                            std::numeric_limits<float>::max()};
  std::uint32_t rng = 7;
  std::vector<float> sent0, sent1;
  std::size_t got = 0;
  for (int round = 0; round < 6000; ++round) {
    rng = rng * 1664525u + 1013904223u;
    const std::uint32_t n = 1u + (rng >> 26);  // 1 .. 64 (<= the latency bound 64)
    std::vector<float> a(n), b(n);
    for (std::uint32_t i = 0; i < n; ++i) {
      rng = rng * 1664525u + 1013904223u;
      a[i] = (rng & 15u) < 8u ? specials[rng & 7u] : static_cast<float>(static_cast<std::int32_t>(rng)) * 1e-9f;
      b[i] = static_cast<float>(sent1.size()) * 0.5f;
      sent0.push_back(a[i]);
      sent1.push_back(b[i]);
    }
    const float* ch[2] = {a.data(), b.data()};
    SND_REQUIRE_EQ(port.Write(ch, 2, n), n);
    d.Block({&v}, Driver::Mod{}, n);  // read exactly what was written
    got += n;
  }
  SND_REQUIRE_EQ(v.out[0].size(), got);
  std::size_t bad = 0;
  for (std::size_t i = 0; i < got; ++i) {
    if (std::memcmp(&v.out[0][i], &sent0[i], 4) != 0 || std::memcmp(&v.out[1][i], &sent1[i], 4) != 0) ++bad;
  }
  SND_CHECK_MSG(bad == 0u, "%u frames differ", unsigned(bad));
  SND_CHECK_EQ(v.starvedBlocks, 0u);
  SND_CHECK_EQ(v.ph.frame, std::uint64_t{got});
  const src::InputPortStats s = port.Stats();
  SND_CHECK_EQ(s.writtenFrames, std::uint64_t{got});
  SND_CHECK_EQ(s.readFrames, std::uint64_t{got});
  SND_CHECK_EQ(s.droppedFrames + s.skippedFrames + s.underruns, std::uint64_t{0});
}

// I3: channel mismatches on the writer side (more: ignored, fewer: the last repeats) and
// on the reader side (a mono voice reads channel 0). Write keeps no state between calls.
SND_TEST(IndepInput, ChannelMismatches) {
  Driver d(snd::SourceKind::Input, Ports(256, 2, 2));
  SND_REQUIRE(d.Ok());
  src::InputPort& port = *d.runtime.InputPortOf(0);
  src::InputParams p = In(0);
  Driver::Voice v;
  SND_REQUIRE(d.Start(v, &p, 2));
  std::vector<float> a(10, 1.0f), b(10, 2.0f), c(10, 3.0f);
  const float* three[3] = {a.data(), b.data(), c.data()};
  SND_CHECK_EQ(port.Write(three, 3, 10), 10u);
  const float* one[1] = {c.data()};
  SND_CHECK_EQ(port.Write(one, 1, 10), 10u);
  d.Block({&v}, Driver::Mod{}, 20);
  for (std::size_t i = 0; i < 10; ++i) {
    SND_CHECK_EQ(v.out[0][i], 1.0f);
    SND_CHECK_EQ(v.out[1][i], 2.0f);
    SND_CHECK_EQ(v.out[0][10 + i], 3.0f);
    SND_CHECK_EQ(v.out[1][10 + i], 3.0f);
  }
  // A mono voice on port 1 hears channel 0 only.
  src::InputParams p1 = In(1, 1);
  Driver::Voice m;
  SND_REQUIRE(d.Start(m, &p1, 1));
  SND_CHECK_EQ(d.runtime.InputPortOf(1)->Write(three, 2, 10), 10u);
  d.Block({&m}, Driver::Mod{}, 10);
  for (std::size_t i = 0; i < 10; ++i) SND_CHECK_EQ(m.out[0][i], 1.0f);
  // Entry rules of Write: no channels / 0 count / 0 frames -> 0, nothing written.
  SND_CHECK_EQ(port.Write(nullptr, 2, 10), 0u);
  SND_CHECK_EQ(port.Write(three, 0, 10), 0u);
  SND_CHECK_EQ(port.Write(three, 2, 0), 0u);
  SND_CHECK_EQ(port.Buffered(), 0u);
  SND_CHECK_EQ(port.Stats().droppedFrames, std::uint64_t{0});
}

// I3 / I11: writer overrun: Write accepts what fits (returns it), drops the newest and
// counts them; written + dropped = requested. A Write inside an RT scope (a capture
// thread) is allocation- and lock-free.
SND_TEST(IndepInput, OverrunAccounting) {
  Driver d(snd::SourceKind::Input, Ports(1000));  // 1024
  SND_REQUIRE(d.Ok());
  src::InputPort& port = *d.runtime.InputPortOf(0);
  std::uint64_t requested = 0, accepted = 0;
  std::vector<float> a(5000, 0.5f), b(5000, -0.5f);
  const float* ch[2] = {a.data(), b.data()};
  {
    snd::rt::RtScope rt;  // the buffers are made outside: only Write runs in the scope
    for (std::uint32_t n : {300u, 300u, 300u, 300u, 300u, 1u, 5000u}) {
      requested += n;
      accepted += port.Write(ch, 2, n);
    }
  }
  SND_CHECK_EQ(accepted, std::uint64_t{1024});
  const src::InputPortStats s = port.Stats();
  SND_CHECK_EQ(s.writtenFrames, accepted);
  SND_CHECK_EQ(s.droppedFrames, requested - accepted);
  SND_CHECK_EQ(port.Buffered(), 1024u);
  SND_CHECK_EQ(Invariant(port), std::uint64_t{0});
}

// I4: the reader's latency bound. Above MaxLatencyFrames() buffered, the oldest are dropped
// down to half of it, counted as skipped; the bound is clamped to [1, capacity], 0 = half.
SND_TEST(IndepInput, ReaderLatencyBound) {
  Driver d(snd::SourceKind::Input, Ports(4096));
  SND_REQUIRE(d.Ok());
  src::InputPort& port = *d.runtime.InputPortOf(0);
  SND_CHECK_EQ(port.MaxLatencyFrames(), 2048u);
  port.Configure(0, 100000);
  SND_CHECK_EQ(port.MaxLatencyFrames(), 4096u);
  port.Configure(0, 0);
  SND_CHECK_EQ(port.MaxLatencyFrames(), 2048u);
  src::InputParams p = In(0);
  Driver::Voice v;
  SND_REQUIRE(d.Start(v, &p, 2));
  std::uint64_t base = 0;
  for (std::uint32_t bound : {2048u, 1000u, 301u, 64u}) {
    port.Configure(0, bound);
    // Exactly the bound: kept whole.
    SND_REQUIRE_EQ(WriteSeq(port, base, bound), bound);
    const std::size_t at = v.out[0].size();
    d.Block({&v}, Driver::Mod{}, 32);
    SND_CHECK_EQ(v.out[0][at], static_cast<float>(base + 1));
    base += bound;
    d.Control(v, src::ControlOp::Resume);  // back to live
    // One more than the bound: everything but the newest bound / 2.
    const std::uint64_t skipped0 = port.Stats().skippedFrames;
    SND_REQUIRE_EQ(WriteSeq(port, base, bound + 1), bound + 1);
    const std::size_t at2 = v.out[0].size();
    d.Block({&v}, Driver::Mod{}, 16);
    const std::uint64_t keep = bound / 2;
    SND_CHECK_MSG(v.out[0][at2] == static_cast<float>(base + bound + 1 - keep + 1), "bound %u: first %f", bound,
                  double(v.out[0][at2]));
    SND_CHECK_EQ(port.Stats().skippedFrames - skipped0, std::uint64_t{bound + 1 - keep});
    base += bound + 1;
    d.Control(v, src::ControlOp::Resume);
    SND_CHECK_EQ(Invariant(port), std::uint64_t{0});
  }
}

// I2 / C4: underruns. Before the voice ever delivered frames, empty blocks are silent and
// not starved. Once it played, a short block is silent after the data, starved, and
// counted once per block in the port and in RuntimeStats. Nothing ends the voice.
SND_TEST(IndepInput, UnderrunAccounting) {
  Driver d(snd::SourceKind::Input, Ports(4096));
  SND_REQUIRE(d.Ok());
  src::InputPort& port = *d.runtime.InputPortOf(0);
  src::InputParams p = In(0);
  Driver::Voice v;
  SND_REQUIRE(d.Start(v, &p, 2));
  for (int i = 0; i < 5; ++i) d.Block({&v}, Driver::Mod{}, 256);
  SND_CHECK_EQ(v.starvedBlocks, 0u);
  SND_CHECK_EQ(port.Stats().underruns, std::uint64_t{0});
  SND_CHECK_EQ(WriteSeq(port, 0, 256), 256u);
  d.Block({&v}, Driver::Mod{}, 256);
  SND_CHECK_EQ(v.starvedBlocks, 0u);
  SND_CHECK_EQ(WriteSeq(port, 256, 100), 100u);
  d.Block({&v}, Driver::Mod{}, 256);  // 100 of 256
  SND_CHECK_EQ(v.starvedBlocks, 1u);
  for (int i = 0; i < 3; ++i) d.Block({&v}, Driver::Mod{}, 256);  // nothing
  SND_CHECK_EQ(v.starvedBlocks, 4u);
  SND_CHECK_EQ(v.last.ended, 0u);
  SND_CHECK_EQ(v.last.framesRendered, 256u);
  for (std::size_t i = 0; i < 356; ++i) SND_CHECK_EQ(v.out[0][1280 + i], static_cast<float>(i + 1));
  for (std::size_t i = 1280 + 356; i < v.out[0].size(); ++i) SND_CHECK_EQ(v.out[0][i], 0.0f);
  SND_CHECK_EQ(port.Stats().underruns, std::uint64_t{4});
  SND_CHECK_EQ(d.runtime.Stats().inputUnderruns, std::uint64_t{4});
  SND_CHECK_EQ(v.ph.frame, std::uint64_t{10 * 256});  // the playhead follows the requests
}

// I5 / I6 / C2: one reader per port; unknown ports (count, max), no params: refused. A
// released port starts again live: the backlog written while it was owned and while it
// was free is skipped.
SND_TEST(IndepInput, OneReaderPerPortAndRestartIsLive) {
  Driver d(snd::SourceKind::Input, Ports(1024, 3));
  SND_REQUIRE(d.Ok());
  SND_CHECK_EQ(d.runtime.InputPortCount(), 3u);
  SND_CHECK(d.runtime.InputPortOf(3) == nullptr);
  SND_CHECK(d.runtime.InputPortOf(0xFFFFFFFFu) == nullptr);
  src::InputParams bad3 = In(3), badMax = In(0xFFFFFFFFu);
  Driver::Voice x;
  SND_CHECK(!d.Start(x, &bad3, 2));
  SND_CHECK(!d.Start(x, &badMax, 2));
  SND_CHECK(!d.Start(x, nullptr, 2));
  SND_CHECK_EQ(d.runtime.Stats().inputInstances, 0u);
  src::InputPort& port = *d.runtime.InputPortOf(2);
  src::InputParams p = In(2);
  Driver::Voice a, b;
  SND_REQUIRE(d.Start(a, &p, 2));
  SND_CHECK(!d.Start(b, &p, 2));
  SND_CHECK_EQ(b.endDetail, std::uint32_t{SND_END_DETAIL_VOICE_LIMIT});
  SND_CHECK_EQ(WriteSeq(port, 0, 300), 300u);
  d.Block({&a}, Driver::Mod{}, 100);
  SND_CHECK_EQ(WriteSeq(port, 300, 50), 50u);
  d.Release(a);
  SND_CHECK_EQ(WriteSeq(port, 350, 70), 70u);  // nobody reads
  SND_CHECK_EQ(port.Buffered(), 320u);
  SND_REQUIRE(d.Start(b, &p, 2));
  SND_CHECK_EQ(port.Buffered(), 0u);
  SND_CHECK_EQ(WriteSeq(port, 420, 10), 10u);
  d.Block({&b}, Driver::Mod{}, 10);
  SND_CHECK_EQ(b.out[0][0], 421.0f);
  SND_CHECK_EQ(b.starvedBlocks, 0u);
  SND_CHECK_EQ(d.runtime.Stats().inputInstances, 1u);
  SND_CHECK_EQ(Invariant(port), std::uint64_t{0});
  // Control on a released slot does nothing.
  d.Control(a, src::ControlOp::Resume);
}

// I7 / I8: a virtual voice consumes outputFrames x pitch (rounded) without output and
// without underruns; Seek / Resume / Devirtualize go live; Pause / Virtualize / SetLoop keep.
SND_TEST(IndepInput, VirtualAndControls) {
  Driver d(snd::SourceKind::Input, Ports(4096));
  SND_REQUIRE(d.Ok());
  src::InputPort& port = *d.runtime.InputPortOf(0);
  src::InputParams p = In(0);
  Driver::Voice v;
  SND_REQUIRE(d.Start(v, &p, 2));
  SND_CHECK_EQ(WriteSeq(port, 0, 2000), 2000u);
  d.Advance(v, 256, 0.5f);
  SND_CHECK_EQ(port.Buffered(), 2000u - 128u);
  d.Advance(v, 100, 2.0f);
  SND_CHECK_EQ(port.Buffered(), 2000u - 328u);
  d.Advance(v, 3000, 1.0f);  // more than buffered: no underrun while virtual
  SND_CHECK_EQ(port.Buffered(), 0u);
  SND_CHECK_EQ(v.last.starved, 0u);
  SND_CHECK_EQ(v.last.ended, 0u);
  SND_CHECK_EQ(port.Stats().underruns, std::uint64_t{0});
  SND_CHECK_EQ(port.Stats().readFrames, std::uint64_t{2000});
  for (src::ControlOp op : {src::ControlOp::Seek, src::ControlOp::Resume, src::ControlOp::Devirtualize}) {
    SND_CHECK_EQ(WriteSeq(port, 5000, 40), 40u);
    d.Control(v, op, 12);
    SND_CHECK_EQ(port.Buffered(), 0u);
  }
  for (src::ControlOp op : {src::ControlOp::Pause, src::ControlOp::Virtualize, src::ControlOp::SetLoop}) {
    SND_CHECK_EQ(WriteSeq(port, 6000, 40), 40u);
    d.Control(v, op, 12);
  }
  SND_CHECK_EQ(port.Buffered(), 120u);
  d.Block({&v}, Driver::Mod{}, 120);
  SND_CHECK_EQ(v.out[0][0], 6001.0f);
  SND_CHECK_EQ(v.out[0][119], 6040.0f);
  SND_CHECK_EQ(Invariant(port), std::uint64_t{0});
}

// D-196 / Runtime: without ports there is no Input kind; port geometry from RuntimeDesc
// (frames rounded up to a power of two, at least 64).
SND_TEST(IndepInput, RuntimeGeometry) {
  {
    src::Runtime rt;
    src::RuntimeDesc rd;
    rd.allocator = &snd::base::DefaultAllocator();
    rd.inputPorts = 0;
    SND_REQUIRE(rt.Init(rd));
    SND_CHECK(rt.Kinds().kinds[static_cast<std::size_t>(snd::SourceKind::Input)] == nullptr);
    SND_CHECK(rt.InputPortOf(0) == nullptr);
    SND_CHECK_EQ(rt.InputPortCount(), 0u);
  }
  {
    src::Runtime rt;
    src::RuntimeDesc rd;
    rd.allocator = &snd::base::DefaultAllocator();
    rd.enableInput = false;
    SND_REQUIRE(rt.Init(rd));
    SND_CHECK(rt.Kinds().kinds[static_cast<std::size_t>(snd::SourceKind::Input)] == nullptr);
    SND_CHECK(rt.InputPortOf(0) == nullptr);
  }
  {
    src::Runtime rt;
    src::RuntimeDesc rd;
    rd.allocator = &snd::base::DefaultAllocator();
    SND_REQUIRE(rt.Init(rd));  // defaults: 4 x 8192 x 2
    SND_CHECK(rt.Kinds().kinds[static_cast<std::size_t>(snd::SourceKind::Input)] != nullptr);
    SND_CHECK(rt.Kinds().kinds[static_cast<std::size_t>(snd::SourceKind::Oscillator)] != nullptr);
    SND_CHECK(rt.Kinds().kinds[static_cast<std::size_t>(snd::SourceKind::Noise)] != nullptr);
    SND_CHECK_EQ(rt.InputPortCount(), 4u);
    SND_CHECK_EQ(rt.InputPortOf(3)->Capacity(), 8192u);
    SND_CHECK_EQ(rt.InputPortOf(3)->Channels(), 2u);
    SND_CHECK_EQ(rt.InputPortOf(0)->SampleRate(), 0u);
  }
  for (std::uint32_t frames : {1u, 63u, 64u, 65u, 5000u}) {
    src::Runtime rt;
    src::RuntimeDesc rd;
    rd.allocator = &snd::base::DefaultAllocator();
    rd.inputPorts = 1;
    rd.inputPortFrames = frames;
    rd.inputPortChannels = 8;
    SND_REQUIRE(rt.Init(rd));
    std::uint32_t want = 64;
    while (want < frames) want *= 2;
    SND_CHECK_EQ(rt.InputPortOf(0)->Capacity(), want);
    SND_CHECK_EQ(rt.InputPortOf(0)->Channels(), 8u);
  }
}

// I11 / I12 under threads: a writer thread writes an increasing sequence in random chunk
// sizes into a small ring (overruns happen) and records which frames were accepted; the
// reader renders blocks of random sizes and sometimes goes live (Resume). Every frame
// heard was accepted, in order, without duplicates; channel 1 is the negation of channel 0
// in every frame (no torn frames); the counters add up.
SND_TEST(IndepInput, ThreadedWriterExactAccounting) {
  Driver d(snd::SourceKind::Input, Ports(256));
  SND_REQUIRE(d.Ok());
  src::InputPort& port = *d.runtime.InputPortOf(0);
  src::InputParams p = In(0);
  Driver::Voice v;
  SND_REQUIRE(d.Start(v, &p, 2));
  constexpr std::uint64_t kTotal = 400000;
  std::vector<std::pair<std::uint64_t, std::uint64_t>> accepted;  // [first, last] values, writer thread
  accepted.reserve(kTotal);
  std::atomic<bool> done{false};
  std::thread writer([&] {
    std::uint64_t at = 0;
    std::uint32_t r = 99;
    while (at < kTotal) {
      r = r * 1664525u + 1013904223u;
      std::uint32_t n = 1u + (r >> 25) % 200u;
      if (at + n > kTotal) n = static_cast<std::uint32_t>(kTotal - at);
      const std::uint32_t ok = WriteSeq(port, at, n);
      if (ok != 0u) accepted.emplace_back(at + 1, at + ok);
      at += n;
      if ((r & 3u) == 0u) std::this_thread::yield();
    }
    done.store(true, std::memory_order_release);
  });
  std::vector<float> heard;
  heard.reserve(kTotal);
  std::uint32_t r = 5;
  std::uint64_t torn = 0;
  while (!done.load(std::memory_order_acquire) || port.Buffered() != 0u) {
    r = r * 22695477u + 1u;
    d.Block({&v}, Driver::Mod{}, 1u + (r >> 24) % 128u);
    if ((r & 63u) == 0u) d.Control(v, src::ControlOp::Resume);
    for (std::size_t i = 0; i < v.out[0].size(); ++i) {
      if (v.out[1][i] != -v.out[0][i]) ++torn;
      if (v.out[0][i] != 0.0f) heard.push_back(v.out[0][i]);
    }
    v.out[0].clear();
    v.out[1].clear();
  }
  writer.join();
  SND_CHECK_EQ(torn, std::uint64_t{0});
  std::uint64_t acceptedFrames = 0;
  for (const auto& a : accepted) acceptedFrames += a.second - a.first + 1;
  const src::InputPortStats s = port.Stats();
  SND_CHECK_EQ(s.writtenFrames, acceptedFrames);
  SND_CHECK_EQ(s.writtenFrames + s.droppedFrames, kTotal);
  SND_CHECK_EQ(s.readFrames, std::uint64_t{heard.size()});
  SND_CHECK_EQ(s.readFrames + s.skippedFrames, s.writtenFrames);
  SND_CHECK_EQ(port.Buffered(), 0u);
  std::uint64_t order = 0, foreign = 0;
  for (std::size_t i = 0; i < heard.size(); ++i) {
    if (i > 0 && !(heard[i] > heard[i - 1])) ++order;
    const std::uint64_t val = static_cast<std::uint64_t>(heard[i]);
    const auto it = std::upper_bound(accepted.begin(), accepted.end(), std::make_pair(val, ~std::uint64_t{0}));
    if (it == accepted.begin() || std::prev(it)->second < val) ++foreign;
  }
  SND_CHECK_MSG(order == 0u, "%u frames out of order / duplicated", unsigned(order));
  SND_CHECK_MSG(foreign == 0u, "%u frames heard that were never accepted", unsigned(foreign));
  SND_CHECK(s.droppedFrames > 0u);   // the small ring did overrun
  SND_CHECK(s.skippedFrames > 0u);   // and the reader went live / trimmed
}

// I11 / D-43-like hand-over: the port's consumer moves between threads (Start / Control /
// AdvanceVirtual on the "audio thread", RenderBatch on a fresh "RT worker" thread each block,
// joined like RtWorkerPool::Run) while a writer thread writes; Release and Start of the port
// race the writer as well. TSan must see no race; the counters must add up.
SND_TEST(IndepInput, ConsumerHandOffAndRestartsAgainstAWriter) {
  Driver d(snd::SourceKind::Input, Ports(512));
  SND_REQUIRE(d.Ok());
  src::InputPort& port = *d.runtime.InputPortOf(1);
  src::InputParams p = In(1);
  std::atomic<bool> stop{false};
  std::atomic<std::uint64_t> requested{0};
  std::thread writer([&] {
    std::uint64_t at = 0;
    while (!stop.load(std::memory_order_acquire)) {
      WriteSeq(port, at, 37);
      at += 37;
      requested.store(at, std::memory_order_relaxed);
      std::this_thread::yield();
    }
  });
  Driver::Voice v;
  std::uint64_t torn = 0;
  for (int round = 0; round < 60; ++round) {
    SND_REQUIRE(d.Start(v, &p, 2));
    for (int b = 0; b < 12; ++b) {
      if (b % 4 == 3) {
        d.Advance(v, 64, 1.0f);
      } else {
        std::thread worker([&] { d.Block({&v}, Driver::Mod{}, 64); });
        worker.join();
      }
      if (b == 6) d.Control(v, src::ControlOp::Devirtualize);
    }
    for (std::size_t i = 0; i < v.out[0].size(); ++i) torn += v.out[1][i] != -v.out[0][i] ? 1u : 0u;
    d.Release(v);
  }
  stop.store(true, std::memory_order_release);
  writer.join();
  SND_CHECK_EQ(torn, std::uint64_t{0});
  const src::InputPortStats s = port.Stats();
  SND_CHECK_EQ(s.writtenFrames + s.droppedFrames, requested.load());
  SND_CHECK_EQ(Invariant(port), std::uint64_t{0});
  SND_CHECK_EQ(d.runtime.Stats().inputInstances, 0u);
}

// Two ports, two writer threads, both voices in one batch rendered on a worker thread:
// the ports do not mix.
SND_TEST(IndepInput, TwoPortsTwoWritersOneBatch) {
  Driver d(snd::SourceKind::Input, Ports(2048, 2, 1));
  SND_REQUIRE(d.Ok());
  src::InputPort* ports[2] = {d.runtime.InputPortOf(0), d.runtime.InputPortOf(1)};
  src::InputParams p0 = In(0, 1), p1 = In(1, 1);
  Driver::Voice a, b;
  SND_REQUIRE(d.Start(a, &p0, 1));
  SND_REQUIRE(d.Start(b, &p1, 1));
  std::atomic<bool> stop{false};
  auto writer = [&](int k) {
    std::vector<float> chunk(50);
    std::uint64_t at = 0;
    while (!stop.load(std::memory_order_acquire)) {
      for (std::size_t i = 0; i < chunk.size(); ++i) {
        chunk[i] = static_cast<float>(k == 0 ? (at + i + 1) : -static_cast<double>(at + i + 1));
      }
      const float* ch[1] = {chunk.data()};
      at += ports[k]->Write(ch, 1, 50);
      std::this_thread::yield();
    }
  };
  std::thread w0(writer, 0), w1(writer, 1);
  std::uint64_t mixed = 0;
  float last[2] = {0.0f, 0.0f};
  std::uint64_t order = 0;
  for (int blk = 0; blk < 2000; ++blk) {
    std::thread worker([&] { d.Block({&a, &b}, Driver::Mod{}, 64); });
    worker.join();
    for (float x : a.out[0]) {
      if (x < 0.0f) ++mixed;
      if (x != 0.0f) {
        if (!(x > last[0])) ++order;
        last[0] = x;
      }
    }
    for (float x : b.out[0]) {
      if (x > 0.0f) ++mixed;
      if (x != 0.0f) {
        if (!(-x > last[1])) ++order;
        last[1] = -x;
      }
    }
    a.out[0].clear();
    b.out[0].clear();
  }
  stop.store(true, std::memory_order_release);
  w0.join();
  w1.join();
  SND_CHECK_EQ(mixed, std::uint64_t{0});
  SND_CHECK_EQ(order, std::uint64_t{0});
}
