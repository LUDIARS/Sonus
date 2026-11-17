// D-43 at kind level: a chunk that missed the deadline may still be inside
// RenderBatch for its voices while the audio thread goes on to Publish
// (ScheduleBackground). Nothing synchronises the two (core does not join a
// late chunk), so ScheduleBackground must not write what RenderBatch of the
// same voice reads. Here one RenderBatch runs on another thread while the
// "audio thread" runs ScheduleBackground, with a seek pending (the common case:
// Seek / Devirtualize in RunEventLogic, the voice's chunk late in Render).
//
// These are detectors for ThreadSanitizer: in other builds they only check
// that the voice still plays exactly afterwards. Under TSan they report the
// race (observed through core in IndepSourceCore.ThreadsHoldsTinyCacheAndUnload:
// SeekRing writing RingCtx::pendingSeek vs RenderRing reading it).
#include <thread>
#include <vector>

#include "indep_driver.hpp"
#include "reference.hpp"

using namespace indep;
namespace src = snd::source;

namespace {

void LateChunkAgainstPublish(src::ISourceKind& k, IndepDriver::Voice& v) {
  std::vector<float> buf(256, 0.0f);
  float* ptr = buf.data();
  src::RenderItem it{};
  it.sourceSlot = v.slot;
  it.framesRequested = 256;
  it.channels = 1;
  it.pitch = snd::Ramp{1.0f, 1.0f};
  snd::Playhead ph = v.ph;
  it.playhead = &ph;
  it.out = &ptr;
  std::vector<std::uint8_t> scratch(1024);
  src::Scratch sc{scratch.data(), scratch.size(), 0};
  src::RenderBatch b{};
  b.kind = k.Kind();
  b.count = 1;
  b.items = &it;
  b.block = src::BlockInfo{1000, 1000u * 256u, 256, 48000};
  b.scratch = &sc;
  snd::VoiceRenderStatus st{};
  std::thread late([&] { k.RenderBatch(b, &st); });  // the late chunk
  k.ScheduleBackground(b.block);                      // Publish of the same block
  late.join();
  v.ph = ph;
}

}  // namespace

SND_TEST(IndepLateChunk, RingSeekInPublishAgainstALateChunk) {
  AssetSpec a;
  a.name = "ra";
  a.pcm = Signal(1, 48000, 96000, &Sine1k);
  a.loadForm = "compressed";
  a.codec = "ADPC";
  Built b = Build("ilate1", {a});
  SND_REQUIRE_MSG(b.ok, "%s", b.diag.c_str());
  const Decoded ref = Reference(b, "ra");
  SND_REQUIRE(ref.ok);
  IndepDriver d(b, IndepDriver::Opt{});
  SND_REQUIRE(d.Ok());
  IndepDriver::Voice v;
  v.slot = d.Start(d.Sample(), d.Asset("ra"), 0, 0);
  SND_REQUIRE(v.slot != snd::kInvalidSlot);
  for (int i = 0; i < 10; ++i) d.Block(d.Sample(), {&v}, 256);
  d.Control(d.Sample(), v.slot, src::ControlOp::Seek, 40000);
  LateChunkAgainstPublish(d.Sample(), v);
  const std::size_t before = v.out[0].size();
  for (int i = 0; i < 20; ++i) d.Block(d.Sample(), {&v}, 256);
  SND_CHECK(v.out[0].size() > before + 2000u);
}

SND_TEST(IndepLateChunk, StreamSeekInPublishAgainstALateChunk) {
  AssetSpec a;
  a.name = "sa";
  a.pcm = Signal(1, 48000, 96000, &Sine1k);
  a.loadForm = "stream";
  a.codec = "ADPC";
  Built b = Build("ilate2", {a});
  SND_REQUIRE_MSG(b.ok, "%s", b.diag.c_str());
  IndepDriver d(b, IndepDriver::Opt{});
  SND_REQUIRE(d.Ok());
  IndepDriver::Voice v;
  v.slot = d.Start(d.Stream(), d.Asset("sa"), 0, 0);
  SND_REQUIRE(v.slot != snd::kInvalidSlot);
  for (int i = 0; i < 30; ++i) d.Block(d.Stream(), {&v}, 256);
  d.Control(d.Stream(), v.slot, src::ControlOp::Seek, 40000);
  LateChunkAgainstPublish(d.Stream(), v);
  const std::size_t before = v.out[0].size();
  for (int i = 0; i < 40; ++i) d.Block(d.Stream(), {&v}, 256);
  SND_CHECK(v.out[0].size() > before + 2000u);
}
