// Input kind and input ports (stage 7-D, D-194) driven directly (synth_driver.hpp):
// written PCM plays unchanged, the start skips the backlog, underrun = silence +
// starved, overrun on the writer side, the latency bound on the reader side, one
// reader per port, the port rate reported to core, virtual advance and resume, and
// a writer thread against a reader thread (run under TSan:
//   ctest --repeat until-fail:20 -R source_input_test).
#include <atomic>
#include <cmath>
#include <cstdint>
#include <thread>
#include <vector>

#include "snd/test/test.hpp"
#include "synth_driver.hpp"

using namespace synthtest;

namespace {

src::InputParams In(std::uint32_t device, std::uint32_t channels) {
  src::InputParams p{};
  p.desc.device_index = device;
  p.desc.channels = channels;
  return p;
}

// Writes frames [from, from + n) of the test signal (ch0 = k + 1, ch1 = -(k + 1), as floats
// exact up to 2^24) to the port.
std::uint32_t WriteSeq(src::InputPort& port, std::uint64_t from, std::uint32_t n) {
  std::vector<float> a(n), b(n);
  for (std::uint32_t i = 0; i < n; ++i) {
    a[i] = static_cast<float>(from + i + 1u);
    b[i] = -a[i];
  }
  const float* ch[2] = {a.data(), b.data()};
  return port.Write(ch, 2, n);
}

}  // namespace

SND_TEST(SourceInput, WrittenPcmPlaysUnchangedAndUnderrunIsSilence) {
  Driver d(snd::SourceKind::Input, Driver::Opt{});
  SND_REQUIRE(d.Ok());
  src::InputPort* port = d.runtime.InputPortOf(0);
  SND_REQUIRE(port != nullptr);
  SND_CHECK_EQ(port->Channels(), 2u);
  SND_CHECK_EQ(port->Capacity(), 4096u);
  SND_CHECK(!d.Kind().RendersAtOutputRate());
  // Written before the start: skipped (the voice plays live).
  SND_CHECK_EQ(WriteSeq(*port, 100000, 500), 500u);
  src::InputParams p = In(0, 2);
  Driver::Voice v;
  SND_REQUIRE(d.Start(v, &p, 2));
  SND_CHECK_EQ(v.reserved, 0u);  // the internal rate
  SND_CHECK_EQ(WriteSeq(*port, 0, 1000), 1000u);
  for (int b = 0; b < 3; ++b) d.Block({&v}, Driver::Mod{}, 256);
  SND_CHECK_EQ(v.starvedBlocks, 0u);
  d.Block({&v}, Driver::Mod{}, 256);  // 232 frames left: the rest is silence, starved
  SND_CHECK_EQ(v.starvedBlocks, 1u);
  SND_CHECK_EQ(v.last.framesRendered, 256u);
  SND_CHECK_EQ(v.last.ended, 0u);
  SND_REQUIRE_EQ(v.out[0].size(), std::size_t{1024});
  for (std::size_t i = 0; i < 1024; ++i) {
    const float want = i < 1000 ? static_cast<float>(i + 1) : 0.0f;
    SND_CHECK_EQ(v.out[0][i], want);
    SND_CHECK_EQ(v.out[1][i], -want);
  }
  SND_CHECK_EQ(v.ph.frame, std::uint64_t{1024});
  // Writing again continues without a gap.
  SND_CHECK_EQ(WriteSeq(*port, 1000, 256), 256u);
  d.Block({&v}, Driver::Mod{}, 256);
  SND_CHECK_EQ(v.out[0][1024], 1001.0f);
  SND_CHECK_EQ(v.out[0][1279], 1256.0f);
  const src::InputPortStats st = port->Stats();
  SND_CHECK_EQ(st.writtenFrames, std::uint64_t{1756});
  SND_CHECK_EQ(st.skippedFrames, std::uint64_t{500});
  SND_CHECK_EQ(st.readFrames, std::uint64_t{1256});
  SND_CHECK_EQ(st.underruns, std::uint64_t{1});
  SND_CHECK_EQ(d.runtime.Stats().inputUnderruns, std::uint64_t{1});
  SND_CHECK_EQ(d.runtime.Stats().inputInstances, 1u);
}

// Before the voice delivered anything a short block is not an underrun.
SND_TEST(SourceInput, NoUnderrunBeforeTheFirstFrames) {
  Driver d(snd::SourceKind::Input, Driver::Opt{});
  SND_REQUIRE(d.Ok());
  src::InputParams p = In(1, 1);
  Driver::Voice v;
  SND_REQUIRE(d.Start(v, &p, 1));
  for (int b = 0; b < 4; ++b) d.Block({&v}, Driver::Mod{}, 256);
  SND_CHECK_EQ(v.starvedBlocks, 0u);
  // Mono write into the stereo port: the port repeats it; the mono voice reads channel 0.
  std::vector<float> m(300, 0.25f);
  const float* ch[1] = {m.data()};
  SND_CHECK_EQ(d.runtime.InputPortOf(1)->Write(ch, 1, 300), 300u);
  d.Block({&v}, Driver::Mod{}, 256);
  for (std::size_t i = 0; i < 256; ++i) SND_CHECK_EQ(v.out[0][1024 + i], 0.25f);
}

SND_TEST(SourceInput, OverrunDropsTheNewestAndTheReaderBoundsLatency) {
  Driver d(snd::SourceKind::Input, Driver::Opt{});
  SND_REQUIRE(d.Ok());
  src::InputPort& port = *d.runtime.InputPortOf(0);
  // No reader: the ring fills, Write keeps what fits.
  SND_CHECK_EQ(WriteSeq(port, 0, 5000), 4096u);
  SND_CHECK_EQ(port.Stats().droppedFrames, std::uint64_t{904});
  SND_CHECK_EQ(WriteSeq(port, 5000, 10), 0u);
  SND_CHECK_EQ(port.Buffered(), 4096u);
  // A voice skips the backlog, then a burst over the bound (2048 = capacity / 2):
  // the oldest are dropped down to 1024.
  src::InputParams p = In(0, 2);
  Driver::Voice v;
  SND_REQUIRE(d.Start(v, &p, 2));
  SND_CHECK_EQ(port.Buffered(), 0u);
  SND_CHECK_EQ(WriteSeq(port, 0, 3000), 3000u);
  d.Block({&v}, Driver::Mod{}, 256);
  SND_CHECK_EQ(v.out[0][0], static_cast<float>(3000 - 1024 + 1));
  SND_CHECK_EQ(v.out[0][255], static_cast<float>(3000 - 1024 + 256));
  SND_CHECK_EQ(port.Buffered(), 1024u - 256u);
  // A configured bound.
  port.Configure(0, 512);
  SND_CHECK_EQ(port.MaxLatencyFrames(), 512u);
  SND_CHECK_EQ(WriteSeq(port, 3000, 1000), 1000u);  // 1768 buffered > 512 -> keep 256
  d.Block({&v}, Driver::Mod{}, 256);
  SND_CHECK_EQ(v.out[0][256], static_cast<float>(4000 - 256 + 1));
  SND_CHECK_EQ(port.Buffered(), 0u);
}

SND_TEST(SourceInput, OneReaderPerPortAndUnknownPortsAreRefused) {
  Driver d(snd::SourceKind::Input, Driver::Opt{});
  SND_REQUIRE(d.Ok());
  src::InputParams p0 = In(0, 2), p1 = In(1, 2), p9 = In(9, 2);
  Driver::Voice a, b, c, e;
  SND_CHECK(d.Start(a, &p0, 2));
  SND_CHECK(!d.Start(b, &p0, 2));
  SND_CHECK_EQ(b.endDetail, std::uint32_t{SND_END_DETAIL_VOICE_LIMIT});
  SND_CHECK(d.Start(c, &p1, 2));
  SND_CHECK(!d.Start(e, &p9, 2));
  SND_CHECK_EQ(e.endDetail, std::uint32_t{SND_END_DETAIL_NONE});
  SND_CHECK(!d.Start(e, nullptr, 2));
  d.Release(a);
  SND_CHECK(d.Start(b, &p0, 2));
  SND_CHECK(d.runtime.InputPortOf(2) == nullptr);
  SND_CHECK_EQ(d.runtime.InputPortCount(), 2u);
}

// D-194: the port's rate goes to core in SourceStartResult::reserved (0 = internal).
SND_TEST(SourceInput, PortRateIsReportedAtTheStart) {
  Driver d(snd::SourceKind::Input, Driver::Opt{});
  SND_REQUIRE(d.Ok());
  src::InputPort& port = *d.runtime.InputPortOf(0);
  src::InputParams p = In(0, 2);
  for (std::uint32_t rate : {44100u, 48000u, 16000u, 0u}) {
    port.Configure(rate, 0);
    Driver::Voice v;
    SND_REQUIRE(d.Start(v, &p, 2));
    SND_CHECK_EQ(v.reserved, rate == 48000u ? 0u : rate);
    d.Release(v);
  }
}

// Virtual: the frames are consumed (pitch included). Resume / Seek / Devirtualize: live.
SND_TEST(SourceInput, VirtualConsumesAndResumeIsLive) {
  Driver d(snd::SourceKind::Input, Driver::Opt{});
  SND_REQUIRE(d.Ok());
  src::InputPort& port = *d.runtime.InputPortOf(0);
  src::InputParams p = In(0, 2);
  Driver::Voice v;
  SND_REQUIRE(d.Start(v, &p, 2));
  SND_CHECK_EQ(WriteSeq(port, 0, 1000), 1000u);
  d.Advance(v, 256, 1.5f);  // 384 frames
  SND_CHECK_EQ(port.Buffered(), 1000u - 384u);
  d.Block({&v}, Driver::Mod{}, 10);
  SND_CHECK_EQ(v.out[0][0], 385.0f);
  d.Control(v, src::ControlOp::Pause);
  SND_CHECK_EQ(port.Buffered(), 1000u - 394u);  // Pause keeps the backlog
  d.Control(v, src::ControlOp::Resume);
  SND_CHECK_EQ(port.Buffered(), 0u);
  SND_CHECK_EQ(WriteSeq(port, 5000, 100), 100u);
  d.Control(v, src::ControlOp::SetLoop, 0);
  d.Control(v, src::ControlOp::Virtualize, 0);
  SND_CHECK_EQ(port.Buffered(), 100u);
  d.Control(v, src::ControlOp::Devirtualize, 0);
  SND_CHECK_EQ(port.Buffered(), 0u);
}

// A writer thread and a reader thread (the voice's RenderBatch / AdvanceVirtual): the
// voice hears an increasing sequence with jumps only where frames were skipped or
// dropped, channel 1 always the negation of channel 0, and every frame read once.
SND_TEST(SourceInput, WriterAndReaderThreads) {
  Driver::Opt o;
  o.inputPortFrames = 1024;
  Driver d(snd::SourceKind::Input, o);
  SND_REQUIRE(d.Ok());
  src::InputPort& port = *d.runtime.InputPortOf(0);
  src::InputParams p = In(0, 2);
  Driver::Voice v;
  SND_REQUIRE(d.Start(v, &p, 2));
  constexpr std::uint64_t kTotal = 300000;
  std::atomic<bool> done{false};
  std::thread writer([&] {
    std::uint64_t at = 0;
    std::uint32_t r = 12345;
    while (at < kTotal) {
      r = r * 1664525u + 1013904223u;
      const std::uint32_t n = 1u + (r >> 24) % 300u;
      const std::uint32_t want = static_cast<std::uint32_t>(at + n > kTotal ? kTotal - at : n);
      at += want;  // what did not fit is dropped (a gap in the sequence)
      WriteSeq(port, at - want, want);
      if ((r & 7u) == 0u) std::this_thread::yield();
    }
    done.store(true, std::memory_order_release);
  });
  std::uint64_t blocks = 0;
  while (!done.load(std::memory_order_acquire) || port.Buffered() != 0u) {
    d.Block({&v}, Driver::Mod{}, 64);
    if ((++blocks & 15u) == 0u) {
      // Keep the output vectors small: check and drop.
      float prev = 0.0f;
      for (std::size_t i = 0; i < v.out[0].size(); ++i) {
        const float a = v.out[0][i];
        SND_CHECK_EQ(v.out[1][i], -a);
        if (a != 0.0f) {
          SND_CHECK_MSG(a > prev, "not increasing at %u: %f after %f", static_cast<unsigned>(i), double(a), double(prev));
          prev = a;
        }
      }
      v.out[0].clear();
      v.out[1].clear();
    }
    std::this_thread::yield();
  }
  writer.join();
  const src::InputPortStats st = port.Stats();
  SND_CHECK_EQ(st.writtenFrames + st.droppedFrames, kTotal);
  SND_CHECK_EQ(st.readFrames + st.skippedFrames, st.writtenFrames);
}
