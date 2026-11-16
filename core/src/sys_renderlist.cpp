// System 8: BuildRenderList (5.1, 2.3). Every real voice that renders this
// block (not held, not paused, started by the end of the block) becomes a
// RenderItem, sorted by bus graph order, then source kind, then real slot
// (deterministic), split into chunks of Config.jobs.voicesPerChunk. The list
// goes into the render set chosen for this block (D-43, Render).
#include <algorithm>

#include "access.hpp"
#include "core_impl.hpp"

namespace snd::core {

namespace {
constexpr schema::SystemId kSys = schema::SystemId::BuildRenderList;
struct Entry {
  std::uint32_t order;
  std::uint32_t kind;
  SlotIndex real;
  SlotIndex voice;
};
}  // namespace

void BuildRenderList(Impl& im) noexcept {
  schema::Tables& t = im.tables;
  RenderSet& set = im.rd.sets[im.rd.currentSet];
  set.itemCount = 0;
  set.chunkCount = 0;
  const schema::TableRows& rr = t.RealVoice.rows;
  const schema::TableRows& vr = t.Voice.rows;
  const ClockSample blockEnd = im.clock + im.cfg.blockFrames;
  Entry* e = im.frameArena.AllocateArray<Entry>(im.cfg.realTotal);
  if (e == nullptr) return;
  std::uint32_t n = 0;
  for (std::uint32_t r = 0; r < im.cfg.realTotal; ++r) {
    if (!base::rows::IsLive(rr, r)) continue;
    const SlotIndex v = SND_R(RealVoice_voiceSlot)[r];
    const std::uint32_t d = base::rows::DenseOf(vr, v);
    if (d == kNoSlot || SND_R(Voice_realSlot)[d] != r) continue;
    if (SND_R(Voice_renderHold)[d].held != 0u) continue;
    const VoiceLifecycle& lc = SND_R(Voice_lifecycle)[d];
    if ((lc.flags & kVoicePausedFlag) != 0u || lc.startClock >= blockEnd) continue;
    if (SND_R(Voice_sourceSlot)[d] == kNoSlot) continue;
    const SlotIndex bus = SND_R(Voice_busSlot)[d];
    const std::uint32_t bd = bus == kNoSlot ? kNoSlot : base::rows::DenseOf(t.Bus.rows, bus);
    const std::uint32_t order = bd == kNoSlot ? 0xFFFFFFFFu : SND_R(Bus_graphOrder)[bd];
    e[n++] = Entry{order, static_cast<std::uint32_t>(SND_R(Voice_sourceKind)[d]), r, v};
  }
  std::sort(e, e + n, [](const Entry& a, const Entry& b) {
    if (a.order != b.order) return a.order < b.order;
    if (a.kind != b.kind) return a.kind < b.kind;
    return a.real < b.real;
  });
  for (std::uint32_t i = 0; i < n; ++i) {
    source::RenderItem& it = set.items[i];
    it = source::RenderItem{};
    it.voiceSlot = e[i].voice;
    it.realSlot = e[i].real;
    it.sourceSlot = SND_R(Voice_sourceSlot)[base::rows::DenseOf(vr, e[i].voice)];
  }
  set.itemCount = n;
  const std::uint32_t vpc = im.cfg.voicesPerChunk;
  set.chunkCount = (n + vpc - 1u) / vpc;
  for (std::uint32_t c = 0; c < set.chunkCount; ++c) {
    set.chunks[c].firstItem = c * vpc;
    set.chunks[c].itemCount = (c + 1u) * vpc <= n ? vpc : n - c * vpc;
    set.chunks[c].segCount = 0;
  }
}

}  // namespace snd::core
