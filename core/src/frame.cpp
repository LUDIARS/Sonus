// The audio-thread entry: one block through the ten systems (design 3.2),
// the render-set choice of D-43, timing and the load LOD (2.4).
#include <cstring>

#include "core_impl.hpp"
#include "snd/internal/rt_check.hpp"
#include "snd/platform/clock.hpp"
#include "snd/platform/cpu.hpp"

namespace snd::core {

namespace {

void UpdateLod(Impl& im, std::uint64_t ns) noexcept {
  const double budgetNs = static_cast<double>(im.stats.block_budget_us) * 1000.0;
  if (budgetNs <= 0.0 || im.cfg.lodDownBlocks == 0u) return;
  const std::uint32_t old = im.rd.lod;
  if (static_cast<double>(ns) > budgetNs) {
    im.lodUnder = 0;
    if (++im.lodOver >= im.cfg.lodDownBlocks && im.rd.lod < 3u) {
      ++im.rd.lod;
      im.lodOver = 0;
    }
  } else if (static_cast<double>(ns) < budgetNs * 0.5) {
    im.lodOver = 0;
    if (++im.lodUnder >= im.cfg.lodUpBlocks && im.rd.lod > 0u) {
      --im.rd.lod;
      im.lodUnder = 0;
    }
  } else {
    im.lodOver = 0;
    im.lodUnder = 0;
  }
  if (im.rd.lod != old) PostEvent(im, notify::EventType::LodChange, im.rd.lod, 0u, 0, old);
}

}  // namespace

void System::RenderBlock(float* interleaved) noexcept {
  Impl& im = *impl_;
  SND_RT_SCOPE();
  const platform::FpControl fp = platform::SetFlushDenormals(true);
  const std::uint64_t t0 = platform::MonotonicNanoseconds();
  im.frameArena.Reset();
  // D-43: decided once at the block start.
  im.blockQuiescent = im.rt.IsQuiescent();
  ApplyCommands(im);
  const std::uint32_t frames = im.cfg.blockFrames;
  const std::uint32_t ch = im.cfg.outChannels;
  if (im.ap.suspended != 0u) {
    // Suspended (2.6): silence, the common clock does not move; flags still flow.
    if (interleaved != nullptr) std::memset(interleaved, 0, sizeof(float) * frames * ch);
    im.endedCount = 0;
    Publish(im);
    if (im.desc.liveTap.block != nullptr) im.desc.liveTap.block(im.desc.liveTap.user, *this);
    platform::RestoreFpControl(fp);
    return;
  }
  UpdateTransport(im);
  ApplyDueActions(im);  // the block's pause/stop requests and fired stop reservations are now known
  EvaluateLogicParams(im);
  RunEventLogic(im);
  Compute3D(im);
  EvaluateDspParams(im);
  SelectVoices(im);
  // Render set (D-43): quiescent -> alternate with the last parallel block (both free);
  // otherwise the set the last parallel block did not use (the late chunk's is the other).
  im.rd.currentSet = 1u - im.rd.lastParallelSet;
  BuildRenderList(im);
  const std::uint64_t t1 = platform::MonotonicNanoseconds();
  core::Render(im);
  const std::uint64_t t2 = platform::MonotonicNanoseconds();
  if (interleaved != nullptr) {
    for (std::uint32_t c = 0; c < ch; ++c) {
      const float* src = im.rd.outBuf + std::size_t{c} * frames;
      for (std::uint32_t f = 0; f < frames; ++f) interleaved[std::size_t{f} * ch + c] = src[f];
    }
  }
  Publish(im);
  const std::uint64_t t3 = platform::MonotonicNanoseconds();
  const std::uint64_t ns = t3 - t0;
  im.profile.block = im.block;
  im.profile.totalNs = ns;
  im.profile.logicNs = t1 - t0;
  im.profile.renderNs = t2 - t1;
  im.timeSumNs += ns;
  ++im.timeSamples;
  const float us = static_cast<float>(ns) / 1000.0f;
  if (us > im.stats.block_time_max_us) im.stats.block_time_max_us = us;
  im.stats.block_time_avg_us = static_cast<float>(static_cast<double>(im.timeSumNs) / im.timeSamples / 1000.0);
  UpdateLod(im, ns);
  im.clock += frames;
  ++im.block;
  if (im.desc.liveTap.block != nullptr) im.desc.liveTap.block(im.desc.liveTap.user, *this);
  platform::RestoreFpControl(fp);
}

void System::Render(float* interleaved, std::uint32_t frames) noexcept {
  Impl& im = *impl_;
  const std::uint32_t ch = im.cfg.outChannels;
  std::uint32_t done = 0;
  while (done < frames) {
    if (im.fifoPos == im.fifoFill) {
      RenderBlock(im.fifo);
      im.fifoPos = 0;
      im.fifoFill = im.cfg.blockFrames;
    }
    std::uint32_t n = im.fifoFill - im.fifoPos;
    if (n > frames - done) n = frames - done;
    std::memcpy(interleaved + std::size_t{done} * ch, im.fifo + std::size_t{im.fifoPos} * ch, sizeof(float) * n * ch);
    im.fifoPos += n;
    done += n;
  }
}

}  // namespace snd::core
