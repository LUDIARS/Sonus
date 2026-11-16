// Services for source kinds and the music system (D-08): lock-free request
// queues filled on the audio thread / RT workers, drained by PumpServices on a
// non-RT thread. Jobs are wrapped so that core can tell when every job posted
// before a bank was unregistered has finished (bank epoch, 3.3).
#include <cstring>

#include "core_impl.hpp"

namespace snd::core {

bool PostJob(void* ctx, const JobDesc& job, JobLane lane) noexcept {
  Impl& im = *static_cast<Impl*>(ctx);
  if (!im.jobQueue.TryPush(PostedJob{job, lane, 0u})) {
    im.droppedJobs.fetch_add(1u, std::memory_order_relaxed);
    return false;
  }
  return true;
}

bool PostIo(void* ctx, const source::IoReadRequest& req) noexcept {
  Impl& im = *static_cast<Impl*>(ctx);
  if (!im.ioQueue.TryPush(req)) {
    im.droppedIo.fetch_add(1u, std::memory_order_relaxed);
    return false;
  }
  return true;
}

ICodec* FindCodec(void* ctx, std::uint32_t fourcc) noexcept {
  const Impl& im = *static_cast<const Impl*>(ctx);
  return im.desc.findCodec != nullptr ? im.desc.findCodec(im.desc.codecUser, fourcc) : nullptr;
}

source::UserBufferControl* UserBufferOf(void* ctx, HandleRef buffer) noexcept {
  Impl& im = *static_cast<Impl*>(ctx);
  if (buffer.slot >= im.cfg.maxUserBuffers || buffer.generation == 0u) return nullptr;
  source::UserBufferControl& ub = im.userBuffers[buffer.slot];
  return ub.generation.load(std::memory_order_acquire) == buffer.generation ? &ub : nullptr;
}

const std::uint8_t* MusicBankChunk(void* ctx, std::uint16_t bankSlot, std::uint32_t fourcc,
                                   std::uint64_t* bytes) noexcept {
  const Impl& im = *static_cast<const Impl*>(ctx);
  const BankRecord* r = RegisteredBank(im, bankSlot);
  if (r == nullptr) {
    if (bytes != nullptr) *bytes = 0;
    return nullptr;
  }
  const bk::ByteSpan s = r->view->Chunk(fourcc);
  if (bytes != nullptr) *bytes = s.bytes;
  return s.data;
}

std::uint32_t MusicPrimeFrames(void* ctx, ClipRef clip, std::uint64_t startFrame) noexcept {
  const Impl& im = *static_cast<const Impl*>(ctx);
  const BankRecord* r = RegisteredBank(im, clip.bankSlot);
  if (r == nullptr) return 0;
  const bk::BankView& v = *r->view;
  if (clip.clipIndex >= v.Rows(bf::chunk::CLIP)) return 0;
  if (v.Column<bk::col::CLIP_kind>()[clip.clipIndex] != static_cast<std::uint32_t>(bf::ClipKind::Asset)) return 0;
  const std::uint32_t asset = v.Column<bk::col::CLIP_target>()[clip.clipIndex];
  if (asset >= r->assetCount) return 0;
  const source::SourceAsset& a = r->assets[asset];
  const SourceKind k = a.form == source::LoadForm::Stream ? SourceKind::Stream : SourceKind::Sample;
  source::ISourceKind* kind = im.kinds[static_cast<std::size_t>(k)];
  return kind != nullptr ? kind->PrimeFrames(a, startFrame) : 0u;
}

namespace {
void SND_CALL JobTrampoline(void* data) {
  JobWrap& w = *static_cast<JobWrap*>(data);
  Impl& im = *w.owner;
  RunJob(w.original);  // the original's cancel flag and done counter (contract)
  const std::uint32_t idx = w.index;
  w.done = 0;
  // Release: everything the job did happens-before the epoch check that sees 0.
  std::atomic_ref<std::uint32_t>(w.epochBucket).store(0u, std::memory_order_release);
  im.jobWrapFree.Push(idx);
}
}  // namespace


void System::PumpServices() noexcept {
  Impl& im = *impl_;
  base::LockGuard lock(im.serviceMutex);
  PostedJob pj;
  while (im.jobQueue.TryPop(pj)) {
    if (im.desc.jobs == nullptr) {
      RunJob(pj.job);  // no job system: run inline on this (non-RT) thread
      continue;
    }
    const std::uint32_t idx = im.jobWrapFree.Pop();
    if (idx == base::kInvalidIndex) {
      im.desc.jobs->Submit(&pj.job, 1u, pj.lane);  // untracked (wrap pool exhausted)
      continue;
    }
    JobWrap& w = im.jobWraps[idx];
    w.original = pj.job;
    w.owner = &im;
    w.index = idx;
    // Ticket (never 0) = position in submission order; checked by the bank epoch.
    const std::uint32_t ticket = im.jobEpoch.fetch_add(1u, std::memory_order_relaxed) + 1u;
    std::atomic_ref<std::uint32_t>(w.epochBucket).store(ticket == 0u ? 1u : ticket, std::memory_order_release);
    JobDesc d = pj.job;
    d.fn = &JobTrampoline;
    d.data = &w;
    d.done_counter = nullptr;
    d.cancel_flag = nullptr;  // the trampoline always runs (it honours the original cancel flag)
    im.desc.jobs->Submit(&d, 1u, pj.lane);
  }
  source::IoReadRequest io;
  while (im.ioQueue.TryPop(io)) {
    if (im.desc.submitIo == nullptr || !im.desc.submitIo(im.desc.ioUser, io)) {
      im.droppedIo.fetch_add(1u, std::memory_order_relaxed);
    }
  }
}

// True when no wrapped job with a ticket <= `ticket` is still running.
bool JobsDoneUpTo(const Impl& im, std::uint32_t ticket) noexcept {
  const std::uint32_t n = im.jobWrapFree.Capacity();
  for (std::uint32_t i = 0; i < n; ++i) {
    const std::uint32_t t =
        std::atomic_ref<std::uint32_t>(const_cast<std::uint32_t&>(im.jobWraps[i].epochBucket))
            .load(std::memory_order_acquire);
    if (t != 0u && static_cast<std::int32_t>(t - ticket) <= 0) return false;
  }
  return true;
}

}  // namespace snd::core
