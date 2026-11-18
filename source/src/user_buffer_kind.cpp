// UserBuffer kind (user_buffer_kind.hpp).
#include "user_buffer_kind.hpp"

#include <cstring>
#include <new>

namespace snd::source::detail {

namespace {
constexpr std::size_t Align64(std::size_t n) noexcept { return (n + 63u) & ~std::size_t{63u}; }
std::uint32_t Cap(const SourceLimits& l) noexcept { return l.maxVoices == 0u ? 1u : l.maxVoices; }
std::uint32_t LoadState(const UserBufferInst& in) noexcept {
  return std::atomic_ref<std::uint32_t>(const_cast<std::uint32_t&>(in.state)).load(std::memory_order_acquire);
}
}  // namespace

std::size_t UserBufferKind::PoolBytes(const SourceLimits& limits) const noexcept {
  const std::uint32_t n = Cap(limits);
  return Align64(sizeof(UserBufferInst) * n) + sizeof(std::uint32_t) * n;
}

bool UserBufferKind::Init(void* pool, std::size_t bytes, const SourceLimits& limits,
                          const SourceServices& services) noexcept {
  const std::uint32_t n = Cap(limits);
  if (pool == nullptr || bytes < PoolBytes(limits)) return false;
  auto* base = static_cast<std::uint8_t*>(pool);
  sv_ = services;
  cap_ = n;
  inst_ = reinterpret_cast<UserBufferInst*>(base);
  freeInst_ = reinterpret_cast<std::uint32_t*>(base + Align64(sizeof(UserBufferInst) * n));
  for (std::uint32_t i = 0; i < n; ++i) {
    new (&inst_[i]) UserBufferInst();
    freeInst_[i] = n - 1u - i;
  }
  freeCount_ = n;
  live_.store(0, std::memory_order_relaxed);
  return true;
}

void UserBufferKind::Shutdown() noexcept {
  inst_ = nullptr;
  freeInst_ = nullptr;
  freeCount_ = 0;
  cap_ = 0;
  live_.store(0, std::memory_order_relaxed);
}

void UserBufferKind::Start(const SourceStartRequest* requests, std::uint32_t count, SourceStartResult* results) noexcept {
  for (std::uint32_t k = 0; k < count; ++k) {
    const SourceStartRequest& rq = requests[k];
    SourceStartResult& res = results[k];
    res = SourceStartResult{kInvalidSlot, SND_END_DETAIL_NONE, 0u, 0u};
    const auto* p = static_cast<const UserBufferParams*>(rq.params);
    UserBufferControl* ub = p != nullptr && sv_.userBuffer != nullptr ? sv_.userBuffer(sv_.userBufferCtx, p->buffer) : nullptr;
    if (ub == nullptr || ub->channelCount == 0u || ub->frames == 0u) continue;
    if (ub->mode == SND_USER_BUFFER_STREAM) {  // one reader per stream ring (D-91)
      bool busy = false;
      for (std::uint32_t i = 0; i < cap_ && !busy; ++i) busy = inst_[i].state == 1u && inst_[i].control == ub;
      if (busy) {
        res.endDetail = SND_END_DETAIL_VOICE_LIMIT;
        continue;
      }
    }
    if (freeCount_ == 0u) {
      res.endDetail = SND_END_DETAIL_VOICE_LIMIT;
      continue;
    }
    const std::uint32_t i = freeInst_[--freeCount_];
    UserBufferInst& in = inst_[i];
    in.control = ub;
    in.generation = p->buffer.generation;
    in.played = 0;
    std::atomic_ref<std::uint32_t>(in.state).store(1u, std::memory_order_release);
    live_.fetch_add(1u, std::memory_order_relaxed);
    res.sourceSlot = i;
  }
}

void UserBufferKind::Release(const SlotIndex* sourceSlots, std::uint32_t count) noexcept {
  for (std::uint32_t k = 0; k < count; ++k) {
    const SlotIndex i = sourceSlots[k];
    if (i >= cap_ || inst_[i].state != 1u) continue;
    std::atomic_ref<std::uint32_t>(inst_[i].state).store(0u, std::memory_order_release);
    inst_[i].control = nullptr;
    freeInst_[freeCount_++] = i;
    live_.fetch_sub(1u, std::memory_order_relaxed);
  }
}

// Seek on a static buffer moves the playhead (core owns it); a stream has no position to seek to.
void UserBufferKind::Control(const SourceControlOp*, std::uint32_t) noexcept {}

void UserBufferKind::QueryReady(const SlotIndex*, std::uint32_t count, std::uint8_t* ready) const noexcept {
  for (std::uint32_t k = 0; k < count; ++k) ready[k] = 1u;
}

std::uint32_t UserBufferKind::Read(UserBufferInst& in, std::uint64_t* pos, float* const* out, std::uint32_t channels,
                                   std::uint32_t frames, bool* ended, bool* starved) noexcept {
  UserBufferControl& ub = *in.control;
  *ended = false;
  *starved = false;
  if (ub.generation.load(std::memory_order_acquire) != in.generation) {  // released under the voice
    *ended = true;
    return 0;
  }
  const std::uint32_t cap = ub.frames;
  const std::uint32_t ch = ub.channelCount;
  if (ub.mode == SND_USER_BUFFER_STREAM) {
    const std::uint64_t w = ub.writePos.load(std::memory_order_acquire);
    std::uint64_t r = ub.readPos.load(std::memory_order_relaxed);
    const std::uint64_t avail = w > r ? w - r : 0u;
    const std::uint32_t take = static_cast<std::uint32_t>(avail < frames ? avail : frames);
    for (std::uint32_t f = 0; f < take;) {
      const std::uint32_t at = static_cast<std::uint32_t>((r + f) % cap);
      std::uint32_t len = take - f;
      if (len > cap - at) len = cap - at;
      for (std::uint32_t c = 0; c < channels; ++c) {
        if (out != nullptr) std::memcpy(out[c] + f, ub.channels[c < ch ? c : ch - 1u] + at, sizeof(float) * len);
      }
      f += len;
    }
    r += take;
    ub.readPos.store(r, std::memory_order_release);
    *pos += take;
    if (take != 0u) in.played = 1;
    if (take < frames && in.played != 0u) *starved = true;
    return frames;  // the rest is silence (out is zeroed by core)
  }
  // Static.
  std::uint32_t written = 0;
  while (written < frames) {
    std::uint64_t p = *pos;
    if (p >= cap) {
      if (ub.loop == 0u) {
        *ended = true;
        break;
      }
      p %= cap;
      *pos = p;
    }
    std::uint32_t len = frames - written;
    if (len > cap - p) len = static_cast<std::uint32_t>(cap - p);
    for (std::uint32_t c = 0; c < channels; ++c) {
      if (out != nullptr) {
        std::memcpy(out[c] + written, ub.channels[c < ch ? c : ch - 1u] + p, sizeof(float) * len);
      }
    }
    written += len;
    *pos = p + len;
  }
  if (ub.loop == 0u && *pos >= cap) *ended = true;
  return written;
}

void UserBufferKind::RenderBatch(const source::RenderBatch& batch, VoiceRenderStatus* status) noexcept {
  for (std::uint32_t k = 0; k < batch.count; ++k) {
    const RenderItem& it = batch.items[k];
    VoiceRenderStatus& st = status[k];
    st = VoiceRenderStatus{};
    if (it.sourceSlot >= cap_) continue;
    UserBufferInst& in = inst_[it.sourceSlot];
    if (LoadState(in) != 1u || in.control == nullptr) {
      st.ended = 1;
      continue;
    }
    bool ended = false, starved = false;
    std::uint64_t pos = it.playhead->frame;
    st.framesRendered = Read(in, &pos, it.out, it.channels, it.framesRequested, &ended, &starved);
    it.playhead->frame = pos;
    st.ended = ended ? 1u : 0u;
    if (starved) {
      st.starved = 1;
      underruns_.fetch_add(1u, std::memory_order_relaxed);
    }
  }
}

void UserBufferKind::AdvanceVirtual(const AdvanceItem* items, std::uint32_t count, VoiceRenderStatus* status) noexcept {
  for (std::uint32_t k = 0; k < count; ++k) {
    const AdvanceItem& a = items[k];
    status[k] = VoiceRenderStatus{};
    if (a.sourceSlot >= cap_ || inst_[a.sourceSlot].state != 1u || inst_[a.sourceSlot].control == nullptr) {
      status[k].ended = 1;
      continue;
    }
    const float pitch = a.pitch > 0.0f ? a.pitch : 1.0f;
    const std::uint32_t frames = static_cast<std::uint32_t>(static_cast<float>(a.outputFrames) * pitch);
    bool ended = false, starved = false;
    std::uint64_t pos = a.playhead->frame;
    Read(inst_[a.sourceSlot], &pos, nullptr, 0u, frames, &ended, &starved);  // consumed, not heard
    a.playhead->frame = pos;
    status[k].ended = ended ? 1u : 0u;
  }
}

}  // namespace snd::source::detail
