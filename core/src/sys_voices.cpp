// System 7: SelectVoices (4.4). Chooses the real voices: priority first,
// audibility second (EvaluateDspParams: gain x previous bus gain incl.
// ducking), partial selection of the top N (N = maxRealVoices, lowered by the
// load LOD). A voice that is already real gets the hysteresis bonus
// (Config.tuning.stealHysteresisDb). Demoted voices fade out over the short
// fade (D-15) on a fade-reserve slot (Config.fadeReserveVoices); without a
// free reserve the demotion waits. Held voices (D-43) keep their real slot.
#include <algorithm>
#include <cmath>

#include "access.hpp"
#include "core_impl.hpp"

namespace snd::core {

namespace {
constexpr schema::SystemId kSys = schema::SystemId::SelectVoices;

struct Candidate {
  float key;        // audibility in dB (+ hysteresis)
  std::uint32_t priority;
  SlotIndex voice;
  std::uint32_t dense;
  std::uint32_t real;  // current real slot or kNoSlot
};

bool Better(const Candidate& a, const Candidate& b) noexcept {
  if (a.priority != b.priority) return a.priority > b.priority;
  if (a.key != b.key) return a.key > b.key;
  return a.voice < b.voice;
}
}  // namespace

void SelectVoices(Impl& im) noexcept {
  schema::Tables& t = im.tables;
  SelectPriv& sp = im.sel;
  const schema::TableRows& vr = t.Voice.rows;
  schema::TableRows& rr = SND_ROWS(RealVoice);
  SlotIndex* realSlot = SND_W(Voice_realSlot);
  SlotIndex* rvVoice = SND_W(RealVoice_voiceSlot);
  RealVoiceAssign* assign = SND_W(RealVoice_assign);
  const VoiceRenderHold* hold = SND_R(Voice_renderHold);
  const VoiceLifecycle* lc = SND_R(Voice_lifecycle);
  const Playhead* playhead = SND_R(Voice_playhead);
  // Virtualize / Devirtualize go to the source in the block the voice moves, before its
  // first render (re-seek / prime first). Never for held voices (D-43).
  const std::uint32_t opCap = im.cfg.realTotal * 2u;
  source::SourceControlOp* ops = im.frameArena.AllocateArray<source::SourceControlOp>(opCap);
  std::uint32_t* opKind = im.frameArena.AllocateArray<std::uint32_t>(opCap);
  std::uint32_t nOps = 0;
  auto control = [&](SlotIndex v, source::ControlOp op, std::uint64_t frame) {
    VoiceStatic& vs = im.ev.vs[v];
    if ((vs.flags & kVsSourceStarted) == 0u || vs.sourceSlot == kNoSlot || ops == nullptr || opKind == nullptr ||
        nOps >= opCap) {
      return;
    }
    ops[nOps] = source::SourceControlOp{};
    ops[nOps].sourceSlot = vs.sourceSlot;
    ops[nOps].op = op;
    ops[nOps].frame = frame;
    opKind[nOps++] = vs.sourceKind;
    vs.flags = op == source::ControlOp::Virtualize ? (vs.flags | kVsVirtualized) : (vs.flags & ~kVsVirtualized);
  };
  // 0. Validate every voice's realSlot (new rows carry stale values) and free
  //    real rows whose voice is gone or whose steal fade finished.
  for (std::uint32_t d = 0; d < vr.count; ++d) {
    const SlotIndex v = base::rows::SlotAt(vr, d);
    const SlotIndex r = realSlot[d];
    if (r == kNoSlot) continue;
    if (r >= im.cfg.realTotal || !base::rows::IsLive(rr, r) || rvVoice[r] != v ||
        sp.voiceGen[r] != vr.generation[v]) {
      realSlot[d] = kNoSlot;
    }
  }
  for (std::uint32_t r = 0; r < im.cfg.realTotal; ++r) {
    if (!base::rows::IsLive(rr, r)) continue;
    const SlotIndex v = rvVoice[r];
    const std::uint32_t vd = base::rows::DenseOf(vr, v);
    const bool voiceLive = vd != kNoSlot && vr.generation[v] == sp.voiceGen[r] && realSlot[vd] == r;
    const bool held = voiceLive && hold[vd].held != 0u;
    if (held) continue;  // D-43: never freed or reassigned while held
    bool free = !voiceLive;
    if (!free && assign[r].fadingOut != 0u) {
      sp.fadeRemaining[r] -= static_cast<std::int32_t>(im.cfg.blockFrames);
      free = sp.fadeRemaining[r] <= 0;
    }
    if (!free && (lc[vd].flags & kVoicePausedFlag) != 0u) free = true;  // paused: silent, no fade needed
    if (free) {
      if (assign[r].fadingOut != 0u && sp.fadingCount > 0u) --sp.fadingCount;
      if (voiceLive) {
        realSlot[vd] = kNoSlot;
        control(v, source::ControlOp::Virtualize, playhead[vd].frame);
      }
      base::rows::Remove(rr, r);
      sp.realFree.Push(r);
    }
  }
  // 1. Candidates.
  Candidate* cand = im.frameArena.AllocateArray<Candidate>(vr.count == 0u ? 1u : vr.count);
  if (cand == nullptr) return;
  const float* aud = SND_R(Voice_audibility);
  const std::uint32_t* prio = SND_R(Voice_priority);
  std::uint32_t n = 0;
  std::uint32_t heldReal = 0;
  std::uint32_t activeReal = 0;  // real, not fading
  for (std::uint32_t d = 0; d < vr.count; ++d) {
    const SlotIndex v = base::rows::SlotAt(vr, d);
    const SlotIndex r = realSlot[d];
    if (r != kNoSlot && assign[r].fadingOut == 0u) ++activeReal;
    if (hold[d].held != 0u) {
      if (r != kNoSlot && assign[r].fadingOut == 0u) ++heldReal;
      continue;
    }
    if ((lc[d].flags & kVoicePausedFlag) != 0u || lc[d].state == static_cast<std::uint8_t>(VoiceStateId::Ending)) continue;
    if (r != kNoSlot && assign[r].fadingOut != 0u) continue;  // finishing its steal fade
    const float a = aud[d] > 1e-12f ? 20.0f * std::log10(aud[d]) : -240.0f;
    cand[n++] = Candidate{r != kNoSlot ? a + im.cfg.stealHysteresisDb : a, prio[d], v, d, r};
  }
  std::uint32_t maxReal = im.cfg.maxReal;
  if (im.rd.lod >= 1u) maxReal = maxReal - maxReal / 4u;  // LOD step 1 (2.4)
  const std::uint32_t slotsForCandidates = maxReal > heldReal ? maxReal - heldReal : 0u;
  const std::uint32_t take = n < slotsForCandidates ? n : slotsForCandidates;
  if (take < n && take > 0u) {
    std::nth_element(cand, cand + (take - 1u), cand + n, Better);
    std::sort(cand, cand + take, Better);  // deterministic order of promotion
  } else if (take == n && n > 1u) {
    std::sort(cand, cand + n, Better);
  }
  // 2. Demote the real voices outside the top N (short fade on a reserve slot).
  for (std::uint32_t i = take; i < n; ++i) {
    const Candidate& c = cand[i];
    if (c.real == kNoSlot) continue;
    if (sp.fadingCount >= im.cfg.fadeReserve) break;  // no reserve: the demotion waits
    RealVoiceAssign& a = assign[c.real];
    a.fadingOut = 1u;
    a.fadeSeconds = im.cfg.shortFadeSeconds;
    sp.fadeRemaining[c.real] = static_cast<std::int32_t>(im.cfg.shortFadeFrames == 0u ? 1u : im.cfg.shortFadeFrames);
    ++sp.fadingCount;
    --activeReal;
  }
  // 3. Promote the top N that are virtual.
  for (std::uint32_t i = 0; i < take; ++i) {
    const Candidate& c = cand[i];
    if (c.real != kNoSlot) continue;
    if (activeReal >= maxReal) break;
    const std::uint32_t r = sp.realFree.Pop();
    if (r == base::kInvalidIndex) break;
    base::rows::InsertNext(rr, r);
    rvVoice[r] = c.voice;
    sp.assignSeq = sp.assignSeq + 1u == 0u ? 1u : sp.assignSeq + 1u;
    assign[r] = RealVoiceAssign{sp.assignSeq, r >= im.cfg.maxReal ? std::uint8_t{1} : std::uint8_t{0}, 0u, 0u, 0.0f};
    sp.voiceGen[r] = vr.generation[c.voice];
    realSlot[c.dense] = r;
    ++activeReal;
    if ((im.ev.vs[c.voice].flags & kVsVirtualized) != 0u) {
      control(c.voice, source::ControlOp::Devirtualize, playhead[c.dense].frame);
    }
  }
  for (std::size_t k = 0; k < static_cast<std::size_t>(SourceKind::Count) && nOps != 0u; ++k) {
    if (im.kinds[k] == nullptr) continue;
    source::SourceControlOp* batch = im.frameArena.AllocateArray<source::SourceControlOp>(nOps);
    if (batch == nullptr) break;
    std::uint32_t nb = 0;
    for (std::uint32_t i = 0; i < nOps; ++i) {
      if (opKind[i] == k) batch[nb++] = ops[i];
    }
    if (nb != 0u) im.kinds[k]->Control(batch, nb);
  }
}

}  // namespace snd::core
