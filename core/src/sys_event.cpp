// System 4: RunEventLogic (design 4, 4.1, 4.2, 4.4; music_if.hpp).
//
// Per block:
//   1. fired reservations (UpdateTransport's FireList): pending starts, timed stops;
//   2. every Playback row: requests (stop / pause / resume / discard), the
//      4.1 state machine, the stop / pause fade, category pause (4.6), action
//      tracks (4.2), and the end of the playback when its last voice is gone;
//   3. voices: end of the source (Voice.renderStatus of the previous block),
//      virtual <-> real transitions (ISourceKind::Control), held voices (D-43:
//      never destroyed while held);
//   4. music logic (RunMusicLogic) and its voice requests;
//   5. the batched source calls: Start, Release, Control, QueryReady.
// Clip selection (4.2): polyphonic, random, random without repeat,
// sequential, shuffle, switch (TRAK.layerParam names the switch group),
// combo sequential (EVNT.comboWindowMs), layer crossfade (all clips start;
// EvaluateDspParams weights them by the TRAK.layerParam value). Nested events
// expand into voices of the same playback (depth <= 4).
// Limits (4.4) at voice creation: global virtual voice table, voice limit
// group (steal oldest / quietest by the previous block's audibility / lowest
// priority / never), per-event instances, retrigger interval.
// Stage 3 (group 4): D-140 virtual return "never" ends the voice (SelectVoices
// marks it), D-141 shuffle without a clip limit, D-142 loop counts (Render cuts
// the last pass), D-144 nested events with their own routing, D-145 action
// tracks one block ahead (sample-accurate starts), D-146 fade-in of a play.
#include <bit>
#include <cmath>
#include <cstring>

#include "access.hpp"
#include "core_impl.hpp"
#include "params.hpp"
#include "snd/base/hash.hpp"

namespace snd::core {

namespace {

constexpr schema::SystemId kSys = schema::SystemId::RunEventLogic;

struct Ctx {
  Impl& im;
  schema::Tables& t;
  ClockSample clock;
  std::uint32_t frames;
};

PlaybackPriv& Priv(Impl& im, SlotIndex s) noexcept { return im.ev.pb[s]; }

// 32 random bits from a playback's generator (the xorshift state after one step).
std::uint32_t NextRandomU32(std::uint32_t& state) noexcept {
  (void)NextRandom(state);
  return state;
}

// D-141: keyed permutation of [0, n) without memory: a 4-round Feistel network on the
// smallest even-bit domain >= n, cycle-walked into [0, n) (a bijection of [0, n) because
// the network is one of the whole domain; expected < 4 steps, the domain is < 4 n).
std::uint32_t PermuteIndex(std::uint32_t i, std::uint32_t n, std::uint32_t key) noexcept {
  if (n <= 1u) return 0u;
  std::uint32_t bits = 2;
  while (bits < 30u && (1u << bits) < n) bits += 2u;
  const std::uint32_t half = bits / 2u;
  const std::uint32_t mask = (1u << half) - 1u;
  std::uint32_t x = i;
  do {
    std::uint32_t l = x >> half, r = x & mask;
    for (std::uint32_t round = 0; round < 4u; ++round) {
      const std::uint32_t f = base::MixU32(r ^ (key + (round + 1u) * 0x9E3779B9u)) & mask;
      const std::uint32_t nl = r;
      r = l ^ f;
      l = nl;
    }
    x = (l << half) | r;
  } while (x >= n);
  return x;
}

float FadeStepFor(const Impl& im, float seconds) noexcept {
  if (!(seconds > 0.0f)) seconds = im.cfg.shortFadeSeconds;
  const float blocks = seconds * static_cast<float>(im.cfg.sampleRate) / static_cast<float>(im.cfg.blockFrames);
  return blocks <= 1.0f ? 1.0f : 1.0f / blocks;
}

}  // namespace

// Playback time (frames) of an action at `ms` (ACTN.timeMs). Shared with ApplyCommands (D-145).
std::uint64_t ActionFrame(const Impl& im, float ms) noexcept {
  return ms > 0.0f ? static_cast<std::uint64_t>(static_cast<double>(ms) * 0.001 * im.cfg.sampleRate) : 0u;
}

namespace {

bool CategoryPaused(const Impl& im, std::uint32_t catSlot) noexcept {
  const schema::Tables& t = im.tables;
  const CategoryRequest* req = SND_R(Category_request);
  for (std::uint32_t s = catSlot, guard = 0; s != kNoSlot && guard < im.cfg.catCap; ++guard) {
    const std::uint32_t d = base::rows::DenseOf(t.Category.rows, s);
    if (d == kNoSlot) break;
    if (req[d].paused != 0u) return true;
    if (s == im.globalCategory) return false;
    s = im.catDefs[s].parentSlot == kNoSlot ? im.globalCategory : im.catDefs[s].parentSlot;
  }
  const std::uint32_t gd = base::rows::DenseOf(t.Category.rows, im.globalCategory);
  return gd != kNoSlot && req[gd].paused != 0u;
}

bool CategoryIgnoresPause(const Impl& im, std::uint32_t catSlot) noexcept {
  for (std::uint32_t s = catSlot, guard = 0; s != kNoSlot && s != im.globalCategory && guard < im.cfg.catCap; ++guard) {
    if ((im.catDefs[s].flags & bf::kCategoryIgnorePause) != 0u) return true;
    s = im.catDefs[s].parentSlot;
  }
  return false;
}

// --- voices ------------------------------------------------------------------
void UnlinkVoice(Impl& im, SlotIndex playback, SlotIndex voice) noexcept {
  PlaybackPriv& pp = Priv(im, playback);
  VoiceStatic* vs = im.ev.vs;
  if (pp.voiceListHead == voice) {
    pp.voiceListHead = vs[voice].nextInPlayback;
    return;
  }
  for (SlotIndex s = pp.voiceListHead, guard = 0; s != kNoSlot && guard < im.cfg.maxVirtual; ++guard) {
    if (vs[s].nextInPlayback == voice) {
      vs[s].nextInPlayback = vs[voice].nextInPlayback;
      return;
    }
    s = vs[s].nextInPlayback;
  }
}

// Destroys a voice row (not held: callers check). The source instance is released in the batch.
void DestroyVoice(Ctx& c, SlotIndex voice) noexcept {
  Impl& im = c.im;
  schema::Tables& t = c.t;
  const std::uint32_t d = base::rows::DenseOf(t.Voice.rows, voice);
  if (d == kNoSlot) return;
  const SlotIndex pb = SND_W(Voice_playbackSlot)[d];
  const SourceKind kind = SND_W(Voice_sourceKind)[d];
  const SlotIndex src = SND_W(Voice_sourceSlot)[d];
  const std::uint32_t group = SND_W(Voice_limitGroup)[d];
  VoiceStatic& vs = im.ev.vs[voice];
  if (src != kNoSlot && (vs.flags & kVsReleased) == 0u && kind < SourceKind::Count &&
      im.ev.releaseCount < im.cfg.maxVirtual) {
    im.ev.releaseSlots[im.ev.releaseCount] = src;
    im.ev.releaseKind[im.ev.releaseCount] = static_cast<std::uint32_t>(kind);
    ++im.ev.releaseCount;
  }
  if (group < im.cfg.groupCap && im.ev.groupLive[group] > 0u) --im.ev.groupLive[group];
  if (pb < im.cfg.maxPlaybacks) {
    UnlinkVoice(im, pb, voice);
    const std::uint32_t pd = base::rows::DenseOf(t.Playback.rows, pb);
    if (pd != kNoSlot && SND_W(Playback_logic)[pd].voiceCount > 0u) --SND_W(Playback_logic)[pd].voiceCount;
  }
  if (im.music != nullptr && (vs.flags & kVsMusic) != 0u && im.ev.musicInputCount < im.ev.musicInputCap) {
    music::MusicInput& in = im.ev.musicInputs[im.ev.musicInputCount++];
    in = music::MusicInput{};
    in.kind = music::MusicInputKind::VoiceEnded;
    in.playbackSlot = pb;
    in.voiceSlot = voice;
    in.clock = c.clock;
  }
  vs = VoiceStatic{};
  RemoveVoiceRow(t, voice);
  im.ev.voiceFree.Push(voice);
}

bool VoiceHeld(const Ctx& c, std::uint32_t d) noexcept {
  const schema::Tables& t = c.t;
  return SND_R(Voice_renderHold)[d].held != 0u;
}

// Destroys every voice of a playback that is not held. true when none is left.
bool DestroyPlaybackVoices(Ctx& c, SlotIndex pb) noexcept {
  Impl& im = c.im;
  bool all = true;
  for (SlotIndex v = Priv(im, pb).voiceListHead, guard = 0; v != kNoSlot && guard < im.cfg.maxVirtual; ++guard) {
    const SlotIndex next = im.ev.vs[v].nextInPlayback;
    const std::uint32_t d = base::rows::DenseOf(c.t.Voice.rows, v);
    if (d != kNoSlot && VoiceHeld(c, d)) {
      all = false;
    } else {
      DestroyVoice(c, v);
    }
    v = next;
  }
  return all;
}

void SetVoicesPaused(Ctx& c, SlotIndex pb, bool paused) noexcept {
  Impl& im = c.im;
  schema::Tables& t = c.t;
  for (SlotIndex v = Priv(im, pb).voiceListHead, guard = 0; v != kNoSlot && guard < im.cfg.maxVirtual; ++guard) {
    const std::uint32_t d = base::rows::DenseOf(t.Voice.rows, v);
    if (d != kNoSlot) {
      VoiceLifecycle& lc = SND_W(Voice_lifecycle)[d];
      lc.flags = paused ? static_cast<std::uint8_t>(lc.flags | kVoicePausedFlag)
                        : static_cast<std::uint8_t>(lc.flags & ~kVoicePausedFlag);
      const SlotIndex src = SND_W(Voice_sourceSlot)[d];
      VoiceStatic& vs = im.ev.vs[v];
      vs.flags = paused ? (vs.flags | kVsWantPaused) : (vs.flags & ~kVsWantPaused);
      if (SND_R(Voice_renderHold)[d].held != 0u) {
        vs.flags |= kVsCtlPending;  // D-43: no Control while held; sent once the hold is gone
      } else if (src != kNoSlot && im.ev.controlCount < im.cfg.maxVirtual) {
        vs.flags &= ~kVsCtlPending;
        source::SourceControlOp& op = im.ev.controls[im.ev.controlCount];
        op = source::SourceControlOp{};
        op.sourceSlot = src;
        op.op = paused ? source::ControlOp::Pause : source::ControlOp::Resume;
        im.ev.controlKind[im.ev.controlCount++] = static_cast<std::uint32_t>(SND_W(Voice_sourceKind)[d]);
      }
    }
    v = im.ev.vs[v].nextInPlayback;
  }
}

// Ends a playback (4.1). false while held voices keep it alive (it ends later).
bool EndPlayback(Ctx& c, std::uint32_t d, std::uint32_t reason, std::uint32_t detail) noexcept {
  Impl& im = c.im;
  schema::Tables& t = c.t;
  const SlotIndex s = base::rows::SlotAt(t.Playback.rows, d);
  PlaybackPriv& pp = Priv(im, s);
  if (!DestroyPlaybackVoices(c, s)) {
    if ((pp.flags & kPbEndPending) == 0u) {
      pp.flags |= kPbEndPending;
      pp.endReason = reason;
      pp.endDetail = detail;
    }
    return false;
  }
  if ((pp.flags & kPbEndPending) != 0u) {
    reason = pp.endReason;
    detail = pp.endDetail;
  }
  const EventRef ref = SND_R(Playback_eventRef)[d];
  if ((pp.flags & kPbInstanceCounted) != 0u) {
    const BankRecord* br = RegisteredBank(im, ref.bankSlot);
    if (br != nullptr && ref.eventIndex < br->eventCount && br->events[ref.eventIndex].instances > 0u) {
      --br->events[ref.eventIndex].instances;
    }
  }
  if (im.music != nullptr && (pp.flags & kPbMusic) != 0u && im.ev.musicInputCount < im.ev.musicInputCap) {
    music::MusicInput& in = im.ev.musicInputs[im.ev.musicInputCount++];
    in = music::MusicInput{};
    in.kind = music::MusicInputKind::PlaybackEnded;
    in.playbackSlot = s;
    in.transportSlot = SND_R(Playback_transportSlot)[d];
    in.event = ref;
    in.voiceSlot = kNoSlot;
    in.clock = c.clock;
  }
  // Playback-scope parameters go with the playback (D-18).
  {
    const schema::TableRows& prow = SND_ROWS(ParamPlayback);
    SlotIndex ps = s * kParamInlineSlots;
    for (std::uint32_t guard = 0; guard < prow.capacity; ++guard) {
      const std::uint32_t pd = base::rows::DenseOf(prow, ps);
      if (pd == kNoSlot) break;
      const SlotIndex next = SND_R(ParamPlayback_nextSlot)[pd];
      RemoveParamPlaybackRow(t, ps);
      ps = next;
      if (ps == kNoSlot) break;
    }
  }
  SND_W(Playback_endInfo)[d] = PlaybackEnd{reason, detail};
  SND_W(Playback_state)[d] = PlaybackStateId::Ended;
  if (im.endedCount < im.cfg.maxPlaybacks) {
    im.ended[im.endedCount++] = EndedPlayback{s, t.Playback.rows.generation[s], reason, detail};
  }
  pp = PlaybackPriv{};
  pp.voiceListHead = kNoSlot;
  RemovePlaybackRow(t, s);
  return true;
}

// --- clip selection and voice creation (4.2, 4.4) ---------------------------------
struct StartContext {
  SlotIndex playback;
  HandleRef handle;
  EventRef ref;           // the playback's event (the routing source)
  SlotIndex emitter;
  ClockSample clock;      // start clock of the playback
  std::uint32_t created;
  std::uint32_t failDetail;
  std::uint32_t needsPriming;
  bool threeD;
  std::uint32_t categorySlot;
  std::uint32_t priority;
  std::uint32_t busSlot;
  std::uint32_t groupSlot;
  // Stage 3 (group 4).
  std::uint32_t virtualReturn;  // D-140: EVNT.virtualReturn of the event whose clips start
  float gainOffsetDb;           // D-144: volume of the nested-event clips above (dB, added)
  float pitchOffsetCents;       // D-144: pitch of the nested-event clips above (cents, added)
};

std::uint32_t BusSlotOf(const Impl& im, const BankRecord& r, std::uint32_t busRow) noexcept {
  if (busRow < r.busCount && r.busSlot[busRow] != kNoSlot) return r.busSlot[busRow];
  return im.rd.masterSlot;
}

// Picks the victim of a full limit group; kNoSlot = reject the new voice.
SlotIndex StealVictim(Ctx& c, std::uint32_t group, std::uint32_t newPriority) noexcept {
  Impl& im = c.im;
  schema::Tables& t = c.t;
  const GroupDef& g = im.groupDefs[group];
  if (g.stealMode == 3u) return kNoSlot;  // never
  SlotIndex best = kNoSlot;
  std::uint32_t bestPrio = 0xFFFFFFFFu;
  ClockSample bestClock = ~ClockSample{0};
  float bestAud = 0.0f;
  const schema::TableRows& rows = t.Voice.rows;
  // Quietest (4.4): Voice.audibility of the previous block (EvaluateDspParams). A voice
  // created in this block has none yet and counts as the loudest; equal values: oldest.
  const float* aud = SND_R(Voice_audibility);
  for (std::uint32_t d = 0; d < rows.count; ++d) {
    if (SND_W(Voice_limitGroup)[d] != group || VoiceHeld(c, d)) continue;
    const SlotIndex v = base::rows::SlotAt(rows, d);
    if ((im.ev.vs[v].flags & kVsStopped) != 0u) continue;
    const std::uint32_t prio = SND_W(Voice_priority)[d];
    const ClockSample clk = im.ev.vs[v].firstClock;
    const bool older = clk < bestClock || (clk == bestClock && v < best);
    float a = 0.0f;
    bool better = false;
    if (g.stealMode == 0u) {  // oldest
      better = older;
    } else if (g.stealMode == 1u) {  // quietest
      a = SND_W(Voice_createdBlock)[d] == im.block ? 3.0e38f : (aud[d] >= 0.0f ? aud[d] : 0.0f);
      better = best == kNoSlot || a < bestAud || (a == bestAud && older);
    } else {  // 2 lowest priority, then oldest
      better = prio < bestPrio || (prio == bestPrio && older);
    }
    if (better) {
      best = v;
      bestPrio = prio;
      bestClock = clk;
      bestAud = a;
    }
  }
  if (best != kNoSlot && g.stealMode == 2u && bestPrio > newPriority) return kNoSlot;
  return best;
}

// Releases a voice: its level fades linearly from `at` (0 = this block's start) over
// fadeSeconds (short fade when 0) and is silent from lifecycle.stopClock on. Render
// applies the release per sample (sample-accurate timed stops, C12).
void StopVoice(Ctx& c, SlotIndex v, float fadeSeconds, ClockSample at = 0, bool shorten = false) noexcept {
  Impl& im = c.im;
  schema::Tables& t = c.t;
  const std::uint32_t d = base::rows::DenseOf(t.Voice.rows, v);
  if (d == kNoSlot) return;
  VoiceLifecycle& lc = SND_W(Voice_lifecycle)[d];
  if (lc.state == static_cast<std::uint8_t>(VoiceStateId::Ending)) return;
  if (lc.state == static_cast<std::uint8_t>(VoiceStateId::Releasing)) {
    // Already releasing: only a shorter end (the unload discard) changes it, and the
    // envelope continues from its current level down to 0 within fadeSeconds.
    // The envelope continues from its level now and reaches 0 at max(at, now) + fadeSeconds
    // (music: an interrupting transition ends an outgoing fade sooner).
    const float sr = static_cast<float>(im.cfg.sampleRate);
    const ClockSample from = at > c.clock ? at : c.clock;
    const ClockSample newStop = from + static_cast<ClockSample>(fadeSeconds * sr + 0.5f);
    if (!shorten || !(fadeSeconds > 0.0f) || newStop >= lc.stopClock) return;
    const float relFrames = lc.releaseSeconds * sr;
    float level = relFrames > 0.0f ? static_cast<float>(lc.stopClock - c.clock) / relFrames : 0.0f;
    level = level > 1.0f ? 1.0f : level;
    if (level <= 0.0f) return;
    lc.releaseSeconds = static_cast<float>(newStop - c.clock) / sr / level;
    lc.stopClock = newStop;
    return;
  }
  lc.state = static_cast<std::uint8_t>(VoiceStateId::Releasing);
  lc.releaseSeconds = fadeSeconds > 0.0f ? fadeSeconds : im.cfg.shortFadeSeconds;
  const ClockSample from = at > c.clock ? at : c.clock;
  lc.stopClock = from + static_cast<ClockSample>(lc.releaseSeconds * static_cast<float>(im.cfg.sampleRate) + 0.5f);
  im.ev.vs[v].flags |= kVsStopped;
}

// Creates one voice and queues its source Start. false when a limit rejected it.
bool CreateVoice(Ctx& c, StartContext& sc, SourceKind kind, const void* params, const source::SourceAsset* asset,
                 std::uint32_t channels, ClipRef clipRef, std::uint32_t eventRow, std::uint32_t trackRow,
                 ClockSample startClock, std::uint64_t startFrame, float gainDb, float pitchCents, float pan,
                 float lowpassHz, float linearGain, float fadeInSeconds, std::uint32_t startFlags,
                 std::uint32_t busSlot, std::uint32_t musicTag, bool music, std::uint32_t syncGroup) noexcept {
  Impl& im = c.im;
  schema::Tables& t = c.t;
  if (sc.created >= im.cfg.maxVoicesPerPlay) return false;
  if (sc.groupSlot != kNoSlot && sc.groupSlot < im.cfg.groupCap) {
    const GroupDef& g = im.groupDefs[sc.groupSlot];
    if (g.maxVoices != 0u && im.ev.groupLive[sc.groupSlot] >= g.maxVoices) {
      const SlotIndex victim = StealVictim(c, sc.groupSlot, sc.priority);
      if (victim == kNoSlot) {
        sc.failDetail = SND_END_DETAIL_LIMIT_GROUP;
        return false;
      }
      StopVoice(c, victim, im.cfg.shortFadeSeconds);
      const std::uint32_t vd = base::rows::DenseOf(t.Voice.rows, victim);
      // The stolen voice leaves the group now (its fade still renders).
      if (vd != kNoSlot) {
        SND_W(Voice_limitGroup)[vd] = kNoSlot;
        if (im.ev.groupLive[sc.groupSlot] > 0u) --im.ev.groupLive[sc.groupSlot];
      }
    }
  }
  if (kind >= SourceKind::Count || im.kinds[static_cast<std::size_t>(kind)] == nullptr) {
    sc.failDetail = SND_END_DETAIL_NONE;
    return false;
  }
  const SlotIndex v = im.ev.voiceFree.Pop();
  if (v == base::kInvalidIndex) {
    sc.failDetail = SND_END_DETAIL_VOICE_LIMIT;
    return false;
  }
  const std::uint32_t d = base::rows::InsertNext(SND_ROWS(Voice), v);
  if (d == kNoSlot) {
    im.ev.voiceFree.Push(v);
    sc.failDetail = SND_END_DETAIL_VOICE_LIMIT;
    return false;
  }
  SND_W(Voice_playbackSlot)[d] = sc.playback;
  SND_W(Voice_sourceKind)[d] = kind;
  SND_W(Voice_sourceSlot)[d] = kNoSlot;
  SND_W(Voice_busSlot)[d] = busSlot != kNoSlot ? busSlot : im.rd.masterSlot;
  SND_W(Voice_emitterSlot)[d] = sc.emitter;  // emitter-scope parameters; 3D only with kVs3D
  SND_W(Voice_priority)[d] = sc.priority;
  SND_W(Voice_limitGroup)[d] = sc.groupSlot;
  VoiceLifecycle lc{};
  lc.startClock = startClock;
  lc.stopClock = 0;
  lc.state = static_cast<std::uint8_t>(VoiceStateId::Starting);
  lc.flags = (startFlags & source::kStartLoop) != 0u ? static_cast<std::uint8_t>(kVoiceLoop) : std::uint8_t{0};
  lc.releaseSeconds = 0.0f;
  SND_W(Voice_lifecycle)[d] = lc;
  SND_W(Voice_clipRef)[d] = clipRef;
  SND_W(Voice_createdBlock)[d] = im.block;
  if (sc.groupSlot != kNoSlot && sc.groupSlot < im.cfg.groupCap) ++im.ev.groupLive[sc.groupSlot];
  VoiceStatic& vs = im.ev.vs[v];
  vs = VoiceStatic{};
  vs.generation = t.Voice.rows.generation[v];
  vs.flags = (sc.threeD && sc.emitter != kNoSlot ? kVs3D : 0u) | (music ? kVsMusic : 0u);
  const source::ISourceKind* k = im.kinds[static_cast<std::size_t>(kind)];
  if (!k->RendersAtOutputRate()) vs.flags |= kVsSourceRate;
  vs.channels = channels == 0u ? 1u : (channels > im.cfg.maxVoiceChannels ? im.cfg.maxVoiceChannels : channels);
  vs.categorySlot = sc.categorySlot;
  vs.eventRow = eventRow;
  vs.trackRow = trackRow;
  vs.gainDb = gainDb;
  vs.pitchCents = pitchCents;
  vs.pan = pan;
  vs.lowpassHz = lowpassHz;
  vs.linearGain = linearGain;
  vs.sourceRate = asset != nullptr && asset->codec.sample_rate != 0u ? static_cast<float>(asset->codec.sample_rate)
                                                                   : static_cast<float>(im.cfg.sampleRate);
  vs.musicGain = 1.0f;
  vs.musicGainTarget = 1.0f;
  vs.fadeInSeconds = fadeInSeconds;
  vs.musicTag = musicTag;
  vs.firstClock = c.clock;
  vs.startFrame = startFrame;
  // D-140: the asset's own return mode (ASST.flags, applied by the source) wins; without
  // one the Event's EVNT.virtualReturn applies (SelectVoices / Render / RunEventLogic).
  vs.virtualReturn = (asset != nullptr && asset->virtualReturn != 0u) || sc.virtualReturn > 2u ? 0u : sc.virtualReturn;
  // D-146: snd_play_fade's fade-in, per voice from its start frame (Render, sample by sample).
  vs.fadeInFrames = sc.playback < im.cfg.maxPlaybacks
                        ? static_cast<std::uint32_t>(im.ap.playbackFadeIn[sc.playback] * static_cast<float>(im.cfg.sampleRate) + 0.5f)
                        : 0u;
  vs.nextInPlayback = Priv(im, sc.playback).voiceListHead;
  Priv(im, sc.playback).voiceListHead = v;
  const std::uint32_t pd = base::rows::DenseOf(t.Playback.rows, sc.playback);
  if (pd != kNoSlot) ++SND_W(Playback_logic)[pd].voiceCount;
  StartBatch& b = im.ev.start[static_cast<std::size_t>(kind)];
  source::SourceStartRequest& rq = b.req[b.count++];
  rq = source::SourceStartRequest{};
  rq.voiceSlot = v;
  rq.playback = sc.handle;
  rq.startClock = startClock;
  rq.startFrame = startFrame;
  rq.asset = asset;
  rq.params = params;
  rq.channels = vs.channels;
  rq.seed = Priv(im, sc.playback).rng ^ (v * 0x9E3779B9u);
  rq.syncGroup = syncGroup;
  rq.flags = startFlags;
  ++sc.created;
  return true;
}

// Expands a clip into voices (nested events recurse).
void StartClip(Ctx& c, StartContext& sc, const BankRecord& r, std::uint32_t clip, std::uint32_t eventRow,
               std::uint32_t trackRow, std::uint32_t busSlot, std::uint32_t depth, std::uint32_t& rng);

void StartEventTracks(Ctx& c, StartContext& sc, const BankRecord& r, std::uint32_t eventRow, std::uint32_t busOverride,
                      std::uint32_t depth, std::uint32_t& rng) noexcept {
  Impl& im = c.im;
  const bk::BankView& v = *r.view;
  if (eventRow >= v.Rows(bf::chunk::EVNT) || depth > kMaxNestedDepth) return;
  const std::uint32_t kind = v.Column<bk::col::EVNT_sequenceKind>()[eventRow];
  const std::uint32_t tf = v.Column<bk::col::EVNT_trackFirst>()[eventRow];
  const std::uint32_t tc = v.Column<bk::col::EVNT_trackCount>()[eventRow];
  EventState& es = r.events[eventRow];
  // Advance per-event sequence state once per play.
  std::uint32_t seqPos = es.cursor;
  if (kind == static_cast<std::uint32_t>(bf::SequenceKind::ComboSequential)) {
    const float window = v.Column<bk::col::EVNT_comboWindowMs>()[eventRow];
    const ClockSample w = static_cast<ClockSample>(window * 0.001f * static_cast<float>(im.cfg.sampleRate));
    seqPos = (es.comboClock != 0u && c.clock - (es.comboClock - 1u) <= w) ? es.cursor + 1u : 0u;
    es.cursor = seqPos;
    es.comboClock = c.clock + 1u;
  } else if (kind == static_cast<std::uint32_t>(bf::SequenceKind::Sequential)) {
    es.cursor = seqPos + 1u;
  }
  for (std::uint32_t ti = 0; ti < tc; ++ti) {
    const std::uint32_t tr = tf + ti;
    const std::uint32_t cf = v.Column<bk::col::TRAK_clipFirst>()[tr];
    const std::uint32_t cc = v.Column<bk::col::TRAK_clipCount>()[tr];
    if (cc == 0u) continue;
    const std::uint32_t trackBus = v.Column<bk::col::TRAK_bus>()[tr];
    const std::uint32_t bus = busOverride != kNoSlot ? busOverride
                              : trackBus != bf::kNone ? BusSlotOf(im, r, trackBus)
                                                       : sc.busSlot;
    auto start = [&](std::uint32_t clip) { StartClip(c, sc, r, clip, eventRow, tr, bus, depth, rng); };
    switch (static_cast<bf::SequenceKind>(kind)) {
      case bf::SequenceKind::Polyphonic:
      case bf::SequenceKind::LayerCrossfade:
        for (std::uint32_t k = 0; k < cc; ++k) start(cf + k);
        break;
      case bf::SequenceKind::Random:
      case bf::SequenceKind::RandomNoRepeat: {
        // CLIP.weight: chance proportional to the weight; a weight <= 0 (or NaN) is never
        // chosen, unless every candidate has one: then they are equally likely.
        const bool noRepeat = kind == static_cast<std::uint32_t>(bf::SequenceKind::RandomNoRepeat) && cc > 1u;
        float total = 0.0f;
        std::uint32_t eligible = 0;
        for (std::uint32_t k = 0; k < cc; ++k) {
          if (noRepeat && cf + k == es.lastChoice) continue;
          const float w = v.Column<bk::col::CLIP_weight>()[cf + k];
          total += w > 0.0f ? w : 0.0f;
          ++eligible;
        }
        const float u = NextRandom(rng);
        std::uint32_t pick = kNoSlot;
        if (total > 0.0f) {
          float x = u * total;
          for (std::uint32_t k = 0; k < cc; ++k) {
            if (noRepeat && cf + k == es.lastChoice) continue;
            const float w = v.Column<bk::col::CLIP_weight>()[cf + k];
            if (!(w > 0.0f)) continue;
            pick = cf + k;  // the last positive weight takes the rounding remainder
            if (x < w) break;
            x -= w;
          }
        } else if (eligible != 0u) {
          std::uint32_t nth = static_cast<std::uint32_t>(u * static_cast<float>(eligible));
          if (nth >= eligible) nth = eligible - 1u;
          for (std::uint32_t k = 0; k < cc; ++k) {
            if (noRepeat && cf + k == es.lastChoice) continue;
            if (nth-- == 0u) {
              pick = cf + k;
              break;
            }
          }
        }
        if (pick != kNoSlot) {
          es.lastChoice = pick;
          start(pick);
        }
        break;
      }
      case bf::SequenceKind::Sequential:
      case bf::SequenceKind::ComboSequential:
        start(cf + seqPos % cc);
        break;
      case bf::SequenceKind::Shuffle: {
        // D-141: any number of clips, no memory. A round plays a permutation of the clips
        // keyed by 32 bits of the playback's generator (drawn by the play that starts the
        // round): no clip twice in a round, a new order every round, and a round that would
        // start with the clip the previous one ended with is rotated by one.
        if (es.shufflePos == 0u || es.shufflePos >= cc) {
          es.shufflePos = 0u;
          es.shuffleKey = NextRandomU32(rng);
          es.shuffleRot = (cc > 1u && es.shuffleLast != 0u && PermuteIndex(0u, cc, es.shuffleKey) + 1u == es.shuffleLast)
                              ? 1u
                              : 0u;
        }
        const std::uint32_t k = PermuteIndex((es.shufflePos + es.shuffleRot) % cc, cc, es.shuffleKey);
        ++es.shufflePos;
        es.shuffleLast = k + 1u;  // index + 1 (0 = none yet)
        start(cf + k);
        break;
      }
      case bf::SequenceKind::Switch: {
        const Id group = v.Column<bk::col::TRAK_layerParam>()[tr];
        const Id value = SwitchValueOf<kSys>(im, group, sc.playback, sc.emitter);
        std::uint32_t pick = kNoSlot, fallback = kNoSlot;
        for (std::uint32_t k = 0; k < cc; ++k) {
          const std::uint32_t sv = v.Column<bk::col::CLIP_switchValue>()[cf + k];
          if (sv == value && pick == kNoSlot) pick = cf + k;
          if (sv == 0u && fallback == kNoSlot) fallback = cf + k;
        }
        if (pick == kNoSlot) pick = fallback;
        if (pick != kNoSlot) start(pick);
        break;
      }
    }
  }
}

void StartClip(Ctx& c, StartContext& sc, const BankRecord& r, std::uint32_t clip, std::uint32_t eventRow,
               std::uint32_t trackRow, std::uint32_t busSlot, std::uint32_t depth, std::uint32_t& rng) {
  Impl& im = c.im;
  const bk::BankView& v = *r.view;
  if (clip >= v.Rows(bf::chunk::CLIP)) return;
  const std::uint32_t ck = v.Column<bk::col::CLIP_kind>()[clip];
  const std::uint32_t target = v.Column<bk::col::CLIP_target>()[clip];
  const float rate = static_cast<float>(im.cfg.sampleRate);
  const float randVol = v.Column<bk::col::CLIP_randVolumeDb>()[clip];
  const float randPitch = v.Column<bk::col::CLIP_randPitchCents>()[clip];
  const float randPan = v.Column<bk::col::CLIP_randPan>()[clip];
  const float randStart = v.Column<bk::col::CLIP_randStartMs>()[clip];
  const float randFilter = v.Column<bk::col::CLIP_randFilterHz>()[clip];
  // Random ranges (4.2): symmetric for volume / pitch / pan, start offset 0..rand, filter below 20 kHz.
  // D-144: the volume and pitch of nested-event clips above this one add (dB, cents).
  const float gainDb = sc.gainOffsetDb + v.Column<bk::col::CLIP_volumeDb>()[clip] +
                       (randVol != 0.0f ? (NextRandom(rng) * 2.0f - 1.0f) * randVol : 0.0f);
  const float pitch = sc.pitchOffsetCents + v.Column<bk::col::CLIP_pitchCents>()[clip] +
                      (randPitch != 0.0f ? (NextRandom(rng) * 2.0f - 1.0f) * randPitch : 0.0f);
  float pan = randPan != 0.0f ? (NextRandom(rng) * 2.0f - 1.0f) * randPan : 0.0f;
  pan = pan < -1.0f ? -1.0f : (pan > 1.0f ? 1.0f : pan);
  const float lowpass = randFilter != 0.0f ? 20000.0f - NextRandom(rng) * randFilter : 0.0f;
  const float startMs = v.Column<bk::col::CLIP_startMs>()[clip];
  const ClockSample startClock = sc.clock + static_cast<ClockSample>(startMs > 0.0f ? startMs * 0.001f * rate + 0.5f : 0.0f);
  const float fadeIn = v.Column<bk::col::CLIP_fadeInMs>()[clip] * 0.001f;
  const std::uint32_t loops = v.Column<bk::col::CLIP_loopCount>()[clip];
  const ClipRef cref{static_cast<std::uint16_t>(r.handle.slot), static_cast<std::uint16_t>(trackRow & 0xFFFFu), clip};
  if (ck == static_cast<std::uint32_t>(bf::ClipKind::NestedEvent)) {
    // D-144: a nested event plays with its OWN routing and settings (bus, category, limit
    // group, priority, 3D, curves, sequence kind, virtual return), inside the parent's
    // playback (handle, end, emitter, playback-scope parameters and switches, playback
    // volume / pitch). The clip's volume and pitch (random included) carry down; its start
    // offset delays the nested event. Event limits (instances, retrigger) are not applied
    // to nested events. Deeper than kMaxNestedDepth: nothing plays, with a warning.
    if (target >= v.Rows(bf::chunk::EVNT)) return;
    if (depth + 1u > kMaxNestedDepth) {
      PostWarning(im, SND_ERR_LIMIT_REACHED, 0u, v.Column<bk::col::EVNT_id>()[target]);
      return;
    }
    StartContext child = sc;
    child.clock = startClock;
    child.priority = v.Column<bk::col::EVNT_priority>()[target];
    child.busSlot = BusSlotOf(im, r, v.Column<bk::col::EVNT_bus>()[target]);
    const std::uint32_t cat = v.Column<bk::col::EVNT_category>()[target];
    child.categorySlot = cat < r.catCount ? r.catSlot[cat] : im.globalCategory;
    const std::uint32_t grp = v.Column<bk::col::EVNT_limitGroup>()[target];
    child.groupSlot = grp < r.groupCount ? r.groupSlot[grp] : kNoSlot;
    child.threeD = (v.Column<bk::col::EVNT_flags>()[target] & bf::kEvent3D) != 0u && sc.emitter != kNoSlot;
    child.virtualReturn = v.Column<bk::col::EVNT_virtualReturn>()[target];
    child.gainOffsetDb = gainDb;
    child.pitchOffsetCents = pitch;
    // Its own random sequence: the playback's seed mixed with the nested event's id (two
    // nested events of one play do not draw the same numbers; the top event is unchanged).
    schema::Tables& t = c.t;
    const std::uint32_t pd = base::rows::DenseOf(t.Playback.rows, sc.playback);
    std::uint32_t childRng =
        base::MixU32((pd != kNoSlot ? SND_R(Playback_seed)[pd] : 0u) ^ v.Column<bk::col::EVNT_id>()[target] ^ (depth + 1u));
    StartEventTracks(c, child, r, target, kNoSlot, depth + 1u, childRng);
    sc.created = child.created;
    if (child.failDetail != SND_END_DETAIL_NONE) sc.failDetail = child.failDetail;
    if (child.needsPriming != 0u) sc.needsPriming = child.needsPriming;
    return;
  }
  if (ck == static_cast<std::uint32_t>(bf::ClipKind::Asset)) {
    if (target >= r.assetCount) return;
    const source::SourceAsset& a = r.assets[target];
    const SourceKind kind = a.form == source::LoadForm::Stream ? SourceKind::Stream : SourceKind::Sample;
    // D-142: CLIP.loopCount = passes of the loop region (the asset's loop points, else the
    // whole asset). 0 = endless. 1 = the default: one pass, except that an asset with loop
    // points loops endlessly (unchanged). N >= 2: N passes, cut at the end of the last
    // one (Render, sample accurate) - the source loops without a count.
    std::uint32_t flags = 0;
    const bool assetLoop = a.loopEnd != 0u;
    if (loops == 0u || loops >= 2u || assetLoop) flags |= source::kStartLoop;
    const std::uint64_t startFrame =
        randStart > 0.0f ? static_cast<std::uint64_t>(NextRandom(rng) * randStart * 0.001f * static_cast<float>(a.codec.sample_rate)) : 0u;
    if (!CreateVoice(c, sc, kind, nullptr, &a, a.codec.channels, cref, eventRow, trackRow, startClock, startFrame, gainDb,
                     pitch, pan, lowpass, 1.0f, fadeIn, flags, busSlot, 0u, false, 0u)) {
      return;
    }
    VoiceStatic& nv = im.ev.vs[Priv(im, sc.playback).voiceListHead];
    if (im.desc.clipMarkers != nullptr) {  // markers of effect clips (markers.hpp)
      const CoreDesc::ClipMarkers m = im.desc.clipMarkers(im.desc.clipMarkersUser, a.assetId);
      if (m.frames != nullptr && m.ids != nullptr) {
        nv.markerFrames = m.frames;
        nv.markerIds = m.ids;
        nv.markerCount = m.count;
      }
    }
    const std::uint64_t total = a.codec.total_frames;
    const std::uint64_t le = assetLoop ? (a.loopEnd < total || total == 0u ? a.loopEnd : total) : total;
    const std::uint64_t lb = assetLoop && a.loopStart < le ? a.loopStart : 0u;
    if ((flags & source::kStartLoop) == 0u) {
      nv.playFrames = total > startFrame ? total - startFrame : 1u;  // one pass to the asset end
    } else if (le > lb && startFrame < le) {
      nv.loopStartFrame = lb;  // the region the source wraps in (markers, counting)
      nv.loopEndFrame = le;
      if (loops >= 2u) nv.playFrames = (le - startFrame) + std::uint64_t{loops - 1u} * (le - lb);
    }
    // CLIP.fadeOutMs: a linear fade over the last fadeOutMs (at the clip's own rate) before
    // the clip's end; clips without an end (endless loops) have none.
    if (nv.playFrames != 0u) nv.endFadeSeconds = v.Column<bk::col::CLIP_fadeOutMs>()[clip] * 0.001f;
    return;
  }
  if (ck == static_cast<std::uint32_t>(bf::ClipKind::Synth)) {
    if (target >= r.synthCount) return;
    const std::uint32_t sk = v.Column<bk::col::SYNP_kind>()[target];
    const std::uint32_t ch = v.Column<bk::col::SYNP_channels>()[target];
    SourceKind kind = SourceKind::Oscillator;
    const void* params = &r.osc[target];
    switch (static_cast<bf::SynthKind>(sk)) {
      case bf::SynthKind::Noise:
        kind = SourceKind::Noise;
        params = &r.noise[target];
        break;
      case bf::SynthKind::Granular:
        kind = SourceKind::Granular;
        params = nullptr;
        break;
      case bf::SynthKind::Patch:
        kind = SourceKind::Patch;
        params = nullptr;
        break;
      default:
        break;
    }
    CreateVoice(c, sc, kind, params, nullptr, ch, cref, eventRow, trackRow, startClock, 0u, gainDb, pitch, pan, lowpass,
                1.0f, fadeIn, loops != 1u ? source::kStartLoop : 0u, busSlot, 0u, false, 0u);
  }
}

// Starts a playback (state None/Pending -> Playing / Priming). false: ended.
bool StartPlayback(Ctx& c, std::uint32_t d, ClockSample startClock) noexcept {
  Impl& im = c.im;
  schema::Tables& t = c.t;
  const SlotIndex s = base::rows::SlotAt(t.Playback.rows, d);
  PlaybackPriv& pp = Priv(im, s);
  const EventRef ref = SND_R(Playback_eventRef)[d];
  const DynamicSourceRef dyn = SND_R(Playback_dynamicSource)[d];
  StartContext sc{};
  sc.playback = s;
  sc.handle = HandleRef{s, t.Playback.rows.generation[s]};
  sc.ref = ref;
  sc.emitter = SND_R(Playback_emitterSlot)[d];
  sc.clock = startClock;
  sc.categorySlot = im.globalCategory;
  sc.priority = 128u;
  sc.busSlot = im.rd.masterSlot;
  sc.groupSlot = kNoSlot;
  std::uint32_t rng = pp.rng;
  SND_W(Playback_schedule)[d].startClock = startClock;
  if ((ref.flags & kEventRefDynamic) != 0u) {
    // PlaySynth / PlayUserBuffer / PlayInput: one voice; an event template gives routing.
    const DynamicDesc& dd = im.ap.dynamic[s];
    const Id tmplId = SND_R(Playback_eventId)[d];
    if (tmplId != 0u) {
      const EventRef tr = ResolveEvent(im, tmplId);
      const BankRecord* br = RegisteredBank(im, tr.bankSlot);
      if (br != nullptr) {
        const bk::BankView& v = *br->view;
        sc.priority = v.Column<bk::col::EVNT_priority>()[tr.eventIndex];
        sc.busSlot = BusSlotOf(im, *br, v.Column<bk::col::EVNT_bus>()[tr.eventIndex]);
        const std::uint32_t cat = v.Column<bk::col::EVNT_category>()[tr.eventIndex];
        if (cat < br->catCount) sc.categorySlot = br->catSlot[cat];
        sc.threeD = (v.Column<bk::col::EVNT_flags>()[tr.eventIndex] & bf::kEvent3D) != 0u;
        sc.virtualReturn = v.Column<bk::col::EVNT_virtualReturn>()[tr.eventIndex];
      }
    }
    if (dd.bus != 0u) {
      const std::uint32_t b = im.busMap.Find(dd.bus);
      if (b != IdMap::kNotFound) sc.busSlot = b;
    }
    if (sc.emitter != kNoSlot) sc.threeD = true;
    const void* params = nullptr;
    std::uint32_t ch = 1;
    float lin = 1.0f;
    std::uint32_t flags = 0;
    static_assert(sizeof(source::CallbackParams) == sizeof(snd_synth_desc));
    switch (dyn.kind) {
      case SourceKind::Callback:
        params = &dd.synth;  // CallbackParams has the same layout (one snd_synth_desc)
        ch = dd.synth.channels;
        lin = dd.synth.volume;
        if (dd.synth.priority != 0u) sc.priority = dd.synth.priority;
        if (dd.synth.mode == SND_SYNTH_LOW_LATENCY) flags |= source::kStartLowLatency;
        break;
      case SourceKind::UserBuffer: {
        params = &dd.userBuffer;  // UserBufferParams = { HandleRef buffer }
        const source::UserBufferControl* ub =
            dd.userBuffer.slot < im.cfg.maxUserBuffers ? &im.userBuffers[dd.userBuffer.slot] : nullptr;
        ch = ub != nullptr ? ub->channelCount : 1u;
        if (ub != nullptr && ub->loop != 0u) flags |= source::kStartLoop;
        break;
      }
      case SourceKind::Input:
        params = &dd.input;  // InputParams = { snd_input_desc }
        ch = dd.input.channels;
        break;
      default:
        break;
    }
    CreateVoice(c, sc, dyn.kind, params, nullptr, ch, ClipRef{kNoBank, 0, 0}, 0u, 0u, startClock, 0u, 0.0f, 0.0f, 0.0f,
                0.0f, lin, 0.0f, flags, sc.busSlot, 0u, false, 0u);
    if (dyn.kind == SourceKind::Callback && sc.created != 0u) {
      VoiceStatic& fv = im.ev.vs[Priv(im, s).voiceListHead];
      fv.followTransport = dd.followTransport;
      fv.transportBeats = -1.0;
      fv.transportBeatsPerSample = 0.0;
    }
    if (dyn.kind == SourceKind::UserBuffer && sc.created != 0u) {
      // Static user buffers play at their own rate (core resamples): VoiceStatic.sourceRate.
      const source::UserBufferControl& ub = im.userBuffers[dd.userBuffer.slot];
      im.ev.vs[Priv(im, s).voiceListHead].sourceRate = static_cast<float>(ub.sampleRate);
    }
  } else {
    const BankRecord* br = RegisteredBank(im, ref.bankSlot);
    if (br == nullptr) return EndPlayback(c, d, SND_END_NOT_LOADED, SND_END_DETAIL_NONE), false;
    const bk::BankView& v = *br->view;
    const std::uint32_t row = ref.eventIndex;
    EventState& es = br->events[row];
    const std::uint32_t maxInst = v.Column<bk::col::EVNT_maxInstances>()[row];
    if (maxInst != 0u && es.instances >= maxInst) {
      EndPlayback(c, d, SND_END_REJECTED, SND_END_DETAIL_EVENT_LIMIT);
      return false;
    }
    const float retrigger = v.Column<bk::col::EVNT_retriggerMs>()[row];
    if (retrigger > 0.0f && es.lastStartClock != 0u &&
        static_cast<float>(c.clock - (es.lastStartClock - 1u)) < retrigger * 0.001f * static_cast<float>(im.cfg.sampleRate)) {
      EndPlayback(c, d, SND_END_REJECTED, SND_END_DETAIL_RETRIGGER_INTERVAL);
      return false;
    }
    const std::uint32_t flags = v.Column<bk::col::EVNT_flags>()[row];
    sc.priority = v.Column<bk::col::EVNT_priority>()[row];
    sc.busSlot = BusSlotOf(im, *br, v.Column<bk::col::EVNT_bus>()[row]);
    const std::uint32_t cat = v.Column<bk::col::EVNT_category>()[row];
    if (cat < br->catCount) sc.categorySlot = br->catSlot[cat];
    const std::uint32_t grp = v.Column<bk::col::EVNT_limitGroup>()[row];
    sc.groupSlot = grp < br->groupCount ? br->groupSlot[grp] : kNoSlot;
    sc.threeD = (flags & bf::kEvent3D) != 0u && sc.emitter != kNoSlot;
    sc.virtualReturn = v.Column<bk::col::EVNT_virtualReturn>()[row];
    // Music events are driven by the music system (segments, 8.3): it starts their voices
    // through RunMusicLogic. Without one, their tracks play like any event.
    if ((flags & bf::kEventMusic) == 0u || im.music == nullptr) StartEventTracks(c, sc, *br, row, kNoSlot, 0u, rng);
    es.lastStartClock = c.clock + 1u;
    ++es.instances;
    pp.flags |= kPbInstanceCounted;
    if ((flags & bf::kEventMusic) != 0u || (ref.flags & kEventRefMusic) != 0u) pp.flags |= kPbMusic;
  }
  pp.rng = rng;
  pp.categorySlot = sc.categorySlot;
  // Voices whose limit rejected them leave the playback empty: end it with the reason.
  if (sc.created == 0u && (pp.flags & kPbMusic) == 0u) {
    if (sc.failDetail == SND_END_DETAIL_NONE && SND_W(Playback_logic)[d].voiceCount == 0u) {
      EndPlayback(c, d, SND_END_COMPLETED, SND_END_DETAIL_NONE);  // nothing to play (empty event)
    } else {
      EndPlayback(c, d, sc.failDetail == SND_END_DETAIL_NONE ? SND_END_ERROR : SND_END_REJECTED, sc.failDetail);
    }
    return false;
  }
  pp.flags |= kPbStarted;
  SND_W(Playback_state)[d] = PlaybackStateId::Playing;  // Priming decided after the Start batch
  if ((pp.flags & kPbMusic) != 0u && im.music != nullptr && im.ev.musicInputCount < im.ev.musicInputCap) {
    music::MusicInput& in = im.ev.musicInputs[im.ev.musicInputCount++];
    in = music::MusicInput{};
    in.kind = music::MusicInputKind::PlaybackStarted;
    in.playbackSlot = s;
    in.transportSlot = SND_R(Playback_transportSlot)[d];
    in.event = ref;
    in.voiceSlot = kNoSlot;
    in.clock = startClock;
  }
  return true;
}

// Stops a playback. at == 0: the playback fade (per block). at != 0 (Stop / SetScheduledStop
// with a When) and the unload discard: every voice fades from the sample `at` (the block
// start for a discard; sample accurate), and the playback ends once its voices are gone.
// A paused playback takes the at == 0 path (it does not sound again).
void BeginStop(Ctx& c, std::uint32_t d, float fadeSeconds, bool discard, ClockSample at = 0) noexcept {
  Impl& im = c.im;
  schema::Tables& t = c.t;
  const SlotIndex s = base::rows::SlotAt(t.Playback.rows, d);
  PlaybackPriv& pp = Priv(im, s);
  if ((pp.flags & kPbStopping) != 0u && !discard) return;
  if (fadeSeconds < 0.0f) {
    const EventRef ref = SND_R(Playback_eventRef)[d];
    const BankRecord* br = RegisteredBank(im, ref.bankSlot);
    fadeSeconds = br != nullptr ? br->view->Column<bk::col::EVNT_fadeOutMs>()[ref.eventIndex] * 0.001f : 0.0f;
  }
  if (discard) fadeSeconds = im.cfg.shortFadeSeconds;
  // A paused (or pausing) playback does not sound again for a timed stop (or a
  // discard): it stops like an untimed Stop sent while paused (from the current level).
  const bool paused = (pp.flags & (kPbPausedUser | kPbPausedCategory | kPbPausedState)) != 0u;
  if (paused) at = 0u;
  pp.flags |= kPbStopping;
  // Timed stops, and the unload discard (D-15: exactly shortFadeMs from this block,
  // per sample), release every voice; the playback ends once they are gone.
  if (!paused && (at != 0u || discard)) {
    pp.flags |= kPbTimedStop;
    for (SlotIndex v = pp.voiceListHead, guard = 0; v != kNoSlot && guard < im.cfg.maxVirtual; ++guard) {
      const SlotIndex next = im.ev.vs[v].nextInPlayback;
      StopVoice(c, v, fadeSeconds, at, discard);
      v = next;
    }
  } else {
    pp.flags &= ~kPbTimedStop;
  }
  pp.fadeTarget = 0.0f;
  pp.fadeRate = FadeStepFor(im, fadeSeconds);
  pp.endReason = discard ? SND_END_DISCARDED : SND_END_STOPPED;
  pp.endDetail = discard ? SND_END_DETAIL_BANK_UNLOADED : SND_END_DETAIL_NONE;
  SND_W(Playback_state)[d] = PlaybackStateId::Stopping;
  if ((pp.flags & kPbMusic) != 0u && im.music != nullptr && im.ev.musicInputCount < im.ev.musicInputCap) {
    music::MusicInput& in = im.ev.musicInputs[im.ev.musicInputCount++];
    in = music::MusicInput{};
    in.kind = music::MusicInputKind::PlaybackStopping;
    in.playbackSlot = s;
    in.transportSlot = SND_R(Playback_transportSlot)[d];
    in.event = SND_R(Playback_eventRef)[d];
    in.voiceSlot = kNoSlot;
    in.clock = c.clock;
  }
}

// D-145: a full queue drops the action with one LIMIT_REACHED warning per block.
void ActionLimit(Impl& im, HandleRef parent) noexcept {
  if (im.ev.actionWarned != 0u) return;
  im.ev.actionWarned = 1u;
  PostWarning(im, SND_ERR_LIMIT_REACHED, 0u, parent.Value());
}

bool Defer(Impl& im, const Deferred& df, HandleRef parent = HandleRef{}) noexcept {
  if (im.ev.deferredCount < im.ev.deferredCap) {
    im.ev.deferred[im.ev.deferredCount++] = df;
    return true;
  }
  if (parent.generation != 0u) ActionLimit(im, parent);
  return false;
}

// --- Action tracks (4.2, D-145) ---------------------------------------------------------
// The time of an action is the parent's track time (Priv.trackTime: frames played, not
// advancing while the parent pauses or stops). RunEventLogic takes the actions of the
// next block ahead (the playback rows of their children are created by ApplyCommands):
//  - Play: the child is created at the next block's ApplyCommands with a CLOCK_SAMPLE
//    start at the action's clock (sample accurate);
//  - StopEvent: at its block, the target playbacks stop from the action's clock (a timed
//    stop, like snd_stop with a clock);
//  - SetParam (the parent's own playback scope, ACTN has no scope column) and
//    ApplySnapshot (at once, ACTN has no time column): block values, so they apply from the
//    first block that starts at or after the action's clock, never before.
// The taken actions wait in EventPriv.pending until their block; if the parent pauses,
// stops or does not start before then, they are dropped (a pause rolls the parent back so
// they come again after the resume) and the children are cancelled (kPbCancelled).

// 0 = the parent's actions may happen, 1 = paused (they come again later), 2 = stopped / gone.
std::uint32_t ParentState(Ctx& c, HandleRef parent, ClockSample at) noexcept {
  Impl& im = c.im;
  schema::Tables& t = c.t;
  const std::uint32_t d = base::rows::DenseOf(t.Playback.rows, parent);
  if (d == kNoSlot) return 2u;
  const PlaybackPriv& pp = Priv(im, parent.slot);
  if ((pp.flags & (kPbStopping | kPbEndPending | kPbCancelled)) != 0u) return 2u;
  if (pp.stopClock != 0u && pp.stopClock <= at) return 2u;
  const PlaybackRequest& req = SND_R(Playback_request)[d];
  const bool fresh = req.seq != SND_W(Playback_logic)[d].handledRequestSeq;
  if (fresh && (req.flags & (kRequestStop | kRequestDiscard)) != 0u) return 2u;
  if ((pp.flags & (kPbPausedUser | kPbPausedCategory | kPbPausedState)) != 0u) return 1u;
  if (fresh && (req.flags & kRequestPause) != 0u) return 1u;
  if (!CategoryIgnoresPause(im, pp.categorySlot)) {
    const EventRef ref = SND_R(Playback_eventRef)[d];
    const BankRecord* br = RegisteredBank(im, ref.bankSlot);
    const bool eventIgnores =
        br != nullptr && (br->view->Column<bk::col::EVNT_flags>()[ref.eventIndex] & bf::kEventIgnorePause) != 0u;
    if (!eventIgnores && CategoryPaused(im, pp.categorySlot)) return 1u;
  }
  return 0u;
}

void AddPending(Impl& im, const ActionPending& e) noexcept {
  if (im.ev.pendingCount < im.ev.deferredCap) {
    im.ev.pending[im.ev.pendingCount++] = e;
  } else {
    ActionLimit(im, e.parent);
  }
}

// Marks the playbacks of `target` to stop from `clock` (handled in the loop below).
void MarkActionStop(Ctx& c, Id target, ClockSample clock) noexcept {
  Impl& im = c.im;
  schema::Tables& t = c.t;
  const schema::TableRows& rows = t.Playback.rows;
  for (std::uint32_t k = 0; k < rows.count; ++k) {
    if (SND_R(Playback_eventId)[k] != target) continue;
    PlaybackPriv& tp = Priv(im, base::rows::SlotAt(rows, k));
    if ((tp.flags & kPbActionStop) == 0u || clock < tp.actionStopClock) tp.actionStopClock = clock;
    tp.flags |= kPbActionStop;
  }
}

// Takes the actions of the parent at row d with track time t in [from, to); an action at
// track time u happens at clockAt(u). `due0` is the first block start of the next block.
template <class ClockAt>
void TakeActions(Ctx& c, std::uint32_t d, std::uint64_t from, std::uint64_t to, ClockAt&& clockAt,
                 bool includePlay = true) noexcept {
  Impl& im = c.im;
  schema::Tables& t = c.t;
  const EventRef ref = SND_R(Playback_eventRef)[d];
  const BankRecord* br = RegisteredBank(im, ref.bankSlot);
  if (br == nullptr || from >= to) return;
  const bk::BankView& v = *br->view;
  const std::uint32_t tf = v.Column<bk::col::EVNT_trackFirst>()[ref.eventIndex];
  const std::uint32_t tc = v.Column<bk::col::EVNT_trackCount>()[ref.eventIndex];
  const SlotIndex s = base::rows::SlotAt(t.Playback.rows, d);
  const HandleRef self{s, t.Playback.rows.generation[s]};
  const ClockSample blockEnd = im.clock + im.cfg.blockFrames;
  for (std::uint32_t tr = tf; tr < tf + tc; ++tr) {
    const std::uint32_t af = v.Column<bk::col::TRAK_actionFirst>()[tr];
    const std::uint32_t ac = v.Column<bk::col::TRAK_actionCount>()[tr];
    if (af == bf::kNone) continue;
    for (std::uint32_t a = af; a < af + ac; ++a) {
      const std::uint64_t at = ActionFrame(im, v.Column<bk::col::ACTN_timeMs>()[a]);
      if (at < from || at >= to) continue;
      ActionPending e{};
      e.target = v.Column<bk::col::ACTN_target>()[a];
      e.value = v.Column<bk::col::ACTN_value>()[a];
      e.parent = self;
      e.trackTime = at;
      e.clock = clockAt(at);
      switch (static_cast<bf::ActionKind>(v.Column<bk::col::ACTN_action>()[a])) {
        case bf::ActionKind::PlayEvent: {
          if (!includePlay) break;
          const HandleRef h = im.playbackSlots.Acquire();
          if (h.generation == 0u) {
            ActionLimit(im, self);
            break;
          }
          Deferred df{};
          df.kind = DeferredKind::Play;
          df.id = e.target;
          df.handle = h;
          df.ref = ResolveEvent(im, e.target);
          df.emitter = SND_R(Playback_emitterSlot)[d];
          df.emitterGen = df.emitter != kNoSlot ? t.Emitter.rows.generation[df.emitter] : 0u;
          df.startClock = e.clock;
          if (!Defer(im, df, self)) {
            im.playbackSlots.Release(h);
            break;
          }
          im.oneShot[h.slot].store(1u, std::memory_order_relaxed);
          e.kind = ActionPendingKind::Play;
          e.child = h;
          AddPending(im, e);
          break;
        }
        case bf::ActionKind::StopEvent:
          if (e.clock < blockEnd) {
            MarkActionStop(c, e.target, e.clock < im.clock ? im.clock : e.clock);  // this block
          } else {
            e.kind = ActionPendingKind::Stop;
            AddPending(im, e);
          }
          break;
        case bf::ActionKind::SetParam:
        case bf::ActionKind::ApplySnapshot: {
          if (!includePlay && e.clock <= im.clock) break;  // applied by ApplyCommands (its start frame)
          e.kind = static_cast<bf::ActionKind>(v.Column<bk::col::ACTN_action>()[a]) == bf::ActionKind::SetParam
                       ? ActionPendingKind::SetParam
                       : ActionPendingKind::Snapshot;
          // The first block start at or after the clock (block starts: blockEnd + k blocks).
          e.due = e.clock <= blockEnd
                      ? blockEnd
                      : blockEnd + ((e.clock - blockEnd + im.cfg.blockFrames - 1u) / im.cfg.blockFrames) * im.cfg.blockFrames;
          if (e.due == blockEnd) {
            Deferred df{};
            df.kind = e.kind == ActionPendingKind::SetParam ? DeferredKind::SetParam : DeferredKind::ApplySnapshot;
            df.id = e.target;
            df.value = e.value;
            df.handle = self;
            Defer(im, df, self);
          } else {
            AddPending(im, e);
          }
          break;
        }
      }
    }
  }
}

// The parent's taken actions that have not happened: dropped; a paused parent takes them
// again from the earliest one.
void DropPending(Ctx& c, HandleRef parent, bool rollBack) noexcept {
  Impl& im = c.im;
  for (std::uint32_t i = 0; i < im.ev.pendingCount;) {
    ActionPending& e = im.ev.pending[i];
    if (e.parent.slot != parent.slot || e.parent.generation != parent.generation) {
      ++i;
      continue;
    }
    if (e.kind == ActionPendingKind::Play && base::rows::IsCurrent(c.t.Playback.rows, e.child)) {
      Priv(im, e.child.slot).flags |= kPbCancelled;
    }
    if (rollBack && base::rows::IsCurrent(c.t.Playback.rows, parent)) {
      PlaybackPriv& pp = Priv(im, parent.slot);
      if (e.trackTime < pp.actionHorizon) pp.actionHorizon = e.trackTime;
    }
    im.ev.pending[i] = im.ev.pending[--im.ev.pendingCount];
  }
}

// D-145: the parent of first-block actions did not start: none of its actions happens.
void CancelActionChildren(Ctx& c, HandleRef parent) noexcept {
  Impl& im = c.im;
  const schema::TableRows& rows = c.t.Playback.rows;
  for (std::uint32_t i = 0; i < im.ap.actionChildCount; ++i) {
    const ApplyPriv::ActionChild& ac = im.ap.actionChildren[i];
    if (ac.parent.slot != parent.slot || ac.parent.generation != parent.generation) continue;
    if (base::rows::IsCurrent(rows, ac.child)) Priv(im, ac.child.slot).flags |= kPbCancelled;
  }
  // (RunEventLogic marks ApplyCommands' tentative snapshot entries: the only write of it here.)
  for (std::uint32_t i = 0; i < im.ap.tentativeCount; ++i) {
    ApplyPriv::TentativeSnapshot& ts = im.ap.tentative[i];
    if (ts.parent.slot == parent.slot && ts.parent.generation == parent.generation) ts.cancelled = 1u;
  }
  DropPending(c, parent, false);
}

// The pending actions whose block has come (start of RunEventLogic, after the commands of
// this block: a pause or stop of the parent applied now cancels them).
void RunPending(Ctx& c) noexcept {
  Impl& im = c.im;
  schema::Tables& t = c.t;
  const ClockSample blockEnd = im.clock + im.cfg.blockFrames;
  for (std::uint32_t i = 0; i < im.ev.pendingCount;) {
    ActionPending& e = im.ev.pending[i];
    if (e.kind == ActionPendingKind::Play) {
      const std::uint32_t cd = base::rows::DenseOf(t.Playback.rows, e.child);
      const PlaybackStateId cs = cd == kNoSlot ? PlaybackStateId::Ended : SND_W(Playback_state)[cd];
      if (cs != PlaybackStateId::Pending && cs != PlaybackStateId::None) {  // started or gone
        im.ev.pending[i] = im.ev.pending[--im.ev.pendingCount];
        continue;
      }
    } else if (e.kind != ActionPendingKind::Stop && e.due > blockEnd) {
      ++i;  // a later block
      continue;
    }
    const std::uint32_t ps = ParentState(c, e.parent, e.clock);
    if (ps != 0u) {
      const HandleRef parent = e.parent;
      DropPending(c, parent, ps == 1u);  // (compacts the list: look at i again)
      continue;
    }
    if (e.kind == ActionPendingKind::Play) {  // waits for the child's start (this block)
      ++i;
      continue;
    }
    if (e.kind == ActionPendingKind::Stop) {
      MarkActionStop(c, e.target, e.clock);
    } else {
      Deferred df{};
      df.kind = e.kind == ActionPendingKind::SetParam ? DeferredKind::SetParam : DeferredKind::ApplySnapshot;
      df.id = e.target;
      df.value = e.value;
      df.handle = e.parent;
      Defer(im, df, e.parent);
    }
    im.ev.pending[i] = im.ev.pending[--im.ev.pendingCount];
  }
}

// After a playback started at `startClock` in this block: the actions of its first block
// that were not taken before (by ApplyCommands for a play starting in its apply block, or
// one block ahead for a scheduled start whose clock was known), then the track time.
void ActionsAtStart(Ctx& c, std::uint32_t d, ClockSample startClock) noexcept {
  Impl& im = c.im;
  schema::Tables& t = c.t;
  const SlotIndex s = base::rows::SlotAt(t.Playback.rows, d);
  PlaybackPriv& pp = Priv(im, s);
  const ClockSample blockEnd = im.clock + im.cfg.blockFrames;
  const std::uint64_t first = startClock < blockEnd ? blockEnd - startClock : 0u;
  const PlaybackRequest& req = SND_R(Playback_request)[d];
  pp.trackTime = 0u;
  if (pp.actionStart != 0u && pp.actionStart != startClock) {  // looked ahead with another clock
    const HandleRef self{s, t.Playback.rows.generation[s]};
    DropPending(c, self, false);
    pp.actionHorizon = 0u;
  }
  auto clockAt = [&](std::uint64_t u) noexcept { return startClock + u; };
  if ((req.flags & kRequestActionsApplied) != 0u) {
    // ApplyCommands created the Play children and the actions at the start frame; the rest
    // of the first block here (Play excluded).
    TakeActions(c, d, 0u, first, clockAt, false);
    pp.actionHorizon = first;
    return;
  }
  if (pp.actionHorizon < first) TakeActions(c, d, pp.actionHorizon, first, clockAt);
  if (pp.actionHorizon < first) pp.actionHorizon = first;
}

void ApplyFireEntry(Ctx& c, const music::FireEntry& f) noexcept {
  Impl& im = c.im;
  schema::Tables& t = c.t;
  const std::uint32_t d = base::rows::DenseOf(t.Playback.rows, f.target);
  switch (f.action) {
    case ReservationAction::Play: {
      if (d == kNoSlot || SND_W(Playback_state)[d] != PlaybackStateId::Pending) break;
      const SlotIndex s = f.target.slot;
      if ((Priv(im, s).flags & kPbCancelled) != 0u) {
        EndPlayback(c, d, SND_END_REJECTED, SND_END_DETAIL_NONE);  // D-145: its parent did not start
      } else if (f.lateRuleApplied == kFireTransportGone) {
        EndPlayback(c, d, SND_END_REJECTED, SND_END_DETAIL_TRANSPORT_GONE);
      } else if (f.lateRuleApplied == static_cast<std::uint8_t>(ReservationLateRule::Cancel)) {
        EndPlayback(c, d, SND_END_REJECTED, SND_END_DETAIL_RESERVATION_MISSED);
      } else {
        Priv(im, s).flags |= kPbStarted;
        const HandleRef self{s, t.Playback.rows.generation[s]};
        if (StartPlayback(c, d, f.fireClock)) {
          ActionsAtStart(c, d, f.fireClock);
        } else {
          CancelActionChildren(c, self);  // D-145: a play that does not start has no actions
        }
      }
      break;
    }
    case ReservationAction::Stop:
    case ReservationAction::ScheduledStop: {
      if (d == kNoSlot) break;
      if (f.action == ReservationAction::ScheduledStop && SND_R(Playback_request)[d].stopReservation != f.reservation) {
        break;  // replaced by a later SetScheduledStop
      }
      if (f.lateRuleApplied == kFireTransportGone) break;  // the playback keeps playing (snd_destroy_transport)
      float fade = -1.0f;
      std::memcpy(&fade, f.payload.bytes, 4);
      PlaybackPriv& pp = Priv(im, f.target.slot);
      if (SND_W(Playback_state)[d] == PlaybackStateId::Pending) {
        EndPlayback(c, d, SND_END_STOPPED, SND_END_DETAIL_NONE);
        break;
      }
      pp.stopClock = f.fireClock < c.clock ? c.clock : f.fireClock;
      pp.stopFadeSeconds = fade;
      if (pp.stopClock < c.clock + im.cfg.blockFrames) BeginStop(c, d, fade, false, pp.stopClock);
      break;
    }
    case ReservationAction::SetMusicState:
    case ReservationAction::Stinger: {
      if (d == kNoSlot || im.ev.musicInputCount >= im.ev.musicInputCap) break;
      Id state = 0;
      std::memcpy(&state, f.payload.bytes, 4);
      music::MusicInput& in = im.ev.musicInputs[im.ev.musicInputCount++];
      in = music::MusicInput{};
      in.kind = f.action == ReservationAction::Stinger ? music::MusicInputKind::Stinger : music::MusicInputKind::SetState;
      in.playbackSlot = f.target.slot;
      in.transportSlot = SND_R(Playback_transportSlot)[d];
      in.event = SND_R(Playback_eventRef)[d];
      in.stateId = state;
      in.voiceSlot = kNoSlot;
      in.clock = f.fireClock;
      in.whenOverride = 1u;
      break;
    }
    default:
      break;
  }
}

// Executes the music system's voice requests through the normal voice path (8.3).
void RunMusic(Ctx& c) noexcept {
  Impl& im = c.im;
  schema::Tables& t = c.t;
  if (im.music == nullptr) {
    im.ev.musicInputCount = 0;
    return;
  }
  music::MusicVoiceRequestList list{im.ev.musicRequests, 0u, im.ev.musicRequestCap};
  im.music->RunMusicLogic(im.musicFrame, t, im.ev.musicInputs, im.ev.musicInputCount, list);
  // Inputs are consumed here (not at the next ApplyCommands): VoiceEnded inputs that
  // FlushSourceCalls posts below (a source refused a music voice) reach the music system
  // in the next block instead of being dropped.
  im.ev.musicInputCount = 0;
  std::uint32_t nres = 0;
  for (std::uint32_t i = 0; i < list.count && i < list.capacity; ++i) {
    const music::MusicVoiceRequest& rq = list.requests[i];
    music::MusicVoiceResult res{rq.tag, kNoSlot, SND_END_DETAIL_NONE, 0u};
    const std::uint32_t pd = rq.playbackSlot < im.cfg.maxPlaybacks ? base::rows::DenseOf(t.Playback.rows, rq.playbackSlot)
                                                                   : kNoSlot;
    switch (rq.op) {
      case music::MusicVoiceOp::StartClip: {
        const BankRecord* br = RegisteredBank(im, rq.clip.bankSlot);
        // kClipRefAssetTrack: clipIndex names an ASST row (segments / stems), else a CLIP row.
        const bool assetRow = rq.clip.trackIndex == kClipRefAssetTrack;
        if (pd == kNoSlot || br == nullptr ||
            (!assetRow && rq.clip.clipIndex >= br->view->Rows(bf::chunk::CLIP))) {
          res.endDetail = SND_END_DETAIL_BANK_UNLOADED;
          break;
        }
        const bk::BankView& v = *br->view;
        const std::uint32_t ck =
            assetRow ? static_cast<std::uint32_t>(bf::ClipKind::Asset) : v.Column<bk::col::CLIP_kind>()[rq.clip.clipIndex];
        const std::uint32_t target = assetRow ? rq.clip.clipIndex : v.Column<bk::col::CLIP_target>()[rq.clip.clipIndex];
        if (ck != static_cast<std::uint32_t>(bf::ClipKind::Asset) || target >= br->assetCount) {
          res.endDetail = SND_END_DETAIL_WRONG_EVENT_KIND;
          break;
        }
        const source::SourceAsset& a = br->assets[target];
        StartContext sc{};
        sc.playback = rq.playbackSlot;
        sc.handle = HandleRef{rq.playbackSlot, t.Playback.rows.generation[rq.playbackSlot]};
        sc.ref = SND_R(Playback_eventRef)[pd];
        sc.emitter = SND_R(Playback_emitterSlot)[pd];
        sc.clock = rq.startClock;
        sc.categorySlot = Priv(im, rq.playbackSlot).categorySlot;
        sc.priority = 200u;
        sc.groupSlot = kNoSlot;
        const BankRecord* er = RegisteredBank(im, sc.ref.bankSlot);
        sc.busSlot = er != nullptr ? BusSlotOf(im, *er, er->view->Column<bk::col::EVNT_bus>()[sc.ref.eventIndex])
                                   : im.rd.masterSlot;
        if (er != nullptr) sc.priority = er->view->Column<bk::col::EVNT_priority>()[sc.ref.eventIndex];
        const SourceKind kind = a.form == source::LoadForm::Stream ? SourceKind::Stream : SourceKind::Sample;
        if (CreateVoice(c, sc, kind, nullptr, &a, a.codec.channels, rq.clip, sc.ref.eventIndex, rq.clip.trackIndex,
                        rq.startClock, rq.startFrame, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f,
                        rq.fadeSeconds > 0.0f ? rq.fadeSeconds : 0.0f,  // StartClip: fade-in
                        a.loopEnd != 0u ? source::kStartLoop : 0u, sc.busSlot, rq.tag, true, rq.syncGroup)) {
          res.voiceSlot = Priv(im, rq.playbackSlot).voiceListHead;
          im.ev.vs[res.voiceSlot].musicGain = rq.gain;
          im.ev.vs[res.voiceSlot].musicGainTarget = rq.gain;
        } else {
          res.endDetail = sc.failDetail;
        }
        break;
      }
      case music::MusicVoiceOp::StopVoice:
        if (rq.voiceSlot < im.cfg.maxVirtual) {
          // Release from startClock (0 = now); a voice already releasing ends sooner if asked.
          StopVoice(c, rq.voiceSlot, rq.fadeSeconds > 0.0f ? rq.fadeSeconds : im.cfg.shortFadeSeconds, rq.startClock,
                    true);
          res.voiceSlot = rq.voiceSlot;
        }
        break;
      case music::MusicVoiceOp::SetGain:
        if (rq.voiceSlot < im.cfg.maxVirtual && base::rows::IsLive(t.Voice.rows, rq.voiceSlot)) {
          VoiceStatic& vs = im.ev.vs[rq.voiceSlot];
          const float blocks = rq.fadeSeconds * static_cast<float>(im.cfg.sampleRate) / static_cast<float>(im.cfg.blockFrames);
          // Linear per-block ramp to the target over fadeSeconds (EvaluateDspParams turns the
          // per-block values into per-sample ramps); within one block: the target at once.
          vs.musicGainTarget = rq.gain;
          vs.musicGainStep = blocks <= 1.0f ? 0.0f : (rq.gain - vs.musicGain) / blocks;
          if (blocks <= 1.0f) vs.musicGain = rq.gain;
          vs.lowpassHz = vs.lowpassHz;  // unchanged
          res.voiceSlot = rq.voiceSlot;
        }
        break;
      case music::MusicVoiceOp::PlayEvent: {
        const HandleRef h = im.playbackSlots.Acquire();
        if (h.generation == 0u) {
          res.endDetail = SND_END_DETAIL_PLAYBACK_TABLE_FULL;
          break;
        }
        im.oneShot[h.slot].store(1u, std::memory_order_relaxed);
        Deferred df{};
        df.kind = DeferredKind::Play;
        df.handle = h;
        df.ref = rq.event;
        df.startClock = rq.startClock;  // the child playback is created next block; it starts here
        df.emitter = pd != kNoSlot ? SND_R(Playback_emitterSlot)[pd] : kNoSlot;
        df.emitterGen = df.emitter != kNoSlot ? t.Emitter.rows.generation[df.emitter] : 0u;
        Defer(im, df);
        break;
      }
    }
    if (nres < im.ev.musicRequestCap) im.ev.musicResults[nres++] = res;
  }
  if (nres != 0u) im.music->OnVoiceResults(im.ev.musicResults, nres);
}

void FlushSourceCalls(Ctx& c) noexcept {
  Impl& im = c.im;
  schema::Tables& t = c.t;
  // Release first (batched per kind): a source slot freed this block may be reused by a Start below.
  if (im.ev.releaseCount != 0u) {
    SlotIndex* tmp = im.frameArena.AllocateArray<SlotIndex>(im.ev.releaseCount);
    for (std::size_t k = 0; k < static_cast<std::size_t>(SourceKind::Count) && tmp != nullptr; ++k) {
      std::uint32_t n = 0;
      for (std::uint32_t i = 0; i < im.ev.releaseCount; ++i) {
        if (im.ev.releaseKind[i] == k) tmp[n++] = im.ev.releaseSlots[i];
      }
      if (n != 0u && im.kinds[k] != nullptr) im.kinds[k]->Release(tmp, n);
    }
    im.ev.releaseCount = 0;
  }
  // Start, once per kind (5.1: never per voice).
  for (std::size_t k = 0; k < static_cast<std::size_t>(SourceKind::Count); ++k) {
    StartBatch& b = im.ev.start[k];
    if (b.count == 0u) continue;
    source::ISourceKind* kind = im.kinds[k];
    kind->Start(b.req, b.count, b.res);
    for (std::uint32_t i = 0; i < b.count; ++i) {
      const SlotIndex v = b.req[i].voiceSlot;
      const std::uint32_t d = base::rows::DenseOf(t.Voice.rows, v);
      if (d == kNoSlot) continue;
      const source::SourceStartResult& r = b.res[i];
      if (r.sourceSlot == kNoSlot) {
        const SlotIndex pb = SND_W(Voice_playbackSlot)[d];
        im.ev.vs[v].flags |= kVsReleased;
        DestroyVoice(c, v);
        PlaybackPriv& pp = Priv(im, pb);
        if (pp.endDetail == SND_END_DETAIL_NONE) pp.endDetail = r.endDetail;
        continue;
      }
      SND_W(Voice_sourceSlot)[d] = r.sourceSlot;
      im.ev.vs[v].flags |= kVsSourceStarted;
      Priv(im, SND_W(Voice_playbackSlot)[d]).flags |= kPbSourceStarted;
      im.ev.vs[v].sourceSlot = r.sourceSlot;
      im.ev.vs[v].sourceKind = static_cast<std::uint32_t>(k);
      if (r.needsPriming != 0u) {
        const SlotIndex pb = SND_W(Voice_playbackSlot)[d];
        const std::uint32_t pd = base::rows::DenseOf(t.Playback.rows, pb);
        if (pd != kNoSlot && SND_W(Playback_state)[pd] == PlaybackStateId::Playing) {
          SND_W(Playback_state)[pd] = PlaybackStateId::Priming;
        }
      }
    }
    b.count = 0;
  }
  // Control (pause / resume / virtualise), batched per kind.
  if (im.ev.controlCount != 0u) {
    source::SourceControlOp* tmp = im.frameArena.AllocateArray<source::SourceControlOp>(im.ev.controlCount);
    for (std::size_t k = 0; k < static_cast<std::size_t>(SourceKind::Count) && tmp != nullptr; ++k) {
      std::uint32_t n = 0;
      for (std::uint32_t i = 0; i < im.ev.controlCount; ++i) {
        if (im.ev.controlKind[i] == k) tmp[n++] = im.ev.controls[i];
      }
      if (n != 0u && im.kinds[k] != nullptr) im.kinds[k]->Control(tmp, n);
    }
    im.ev.controlCount = 0;
  }
}

// Priming playbacks (4.1): Playing once every voice source is ready.
void QueryPriming(Ctx& c) noexcept {
  Impl& im = c.im;
  schema::Tables& t = c.t;
  const schema::TableRows& rows = t.Playback.rows;
  for (std::uint32_t d = 0; d < rows.count; ++d) {
    if (SND_W(Playback_state)[d] != PlaybackStateId::Priming) continue;
    const SlotIndex s = base::rows::SlotAt(rows, d);
    bool ready = true;
    for (SlotIndex v = Priv(im, s).voiceListHead, guard = 0; v != kNoSlot && guard < im.cfg.maxVirtual; ++guard) {
      const std::uint32_t vd = base::rows::DenseOf(t.Voice.rows, v);
      if (vd != kNoSlot) {
        const SlotIndex src = SND_W(Voice_sourceSlot)[vd];
        const SourceKind k = SND_W(Voice_sourceKind)[vd];
        std::uint8_t ok = 1;
        if (src != kNoSlot && im.kinds[static_cast<std::size_t>(k)] != nullptr) {
          im.kinds[static_cast<std::size_t>(k)]->QueryReady(&src, 1u, &ok);
        }
        if (ok == 0u) ready = false;
      }
      v = im.ev.vs[v].nextInPlayback;
    }
    if (ready) SND_W(Playback_state)[d] = PlaybackStateId::Playing;
  }
}

}  // namespace

void RunEventLogic(Impl& im) noexcept {
  schema::Tables& t = im.tables;
  Ctx c{im, t, im.clock, im.cfg.blockFrames};
  im.endedCount = 0;
  im.ev.controlCount = 0;
  im.ev.actionWarned = 0u;
  const ClockSample blockEnd = im.clock + im.cfg.blockFrames;

  // 0. New playback rows (ApplyCommands created them this block): initialise, so a
  //    reservation that fires in the same block (CLOCK_SAMPLE inside or before this
  //    block, a DELAY_US shorter than a block) finds its playback PENDING.
  {
    schema::TableRows& prows = SND_ROWS(Playback);
    for (std::uint32_t d = 0; d < prows.count; ++d) {
      const SlotIndex s = base::rows::SlotAt(prows, d);
      PlaybackPriv& pp = Priv(im, s);
      if (pp.generation == prows.generation[s]) continue;
      pp = PlaybackPriv{};
      pp.generation = prows.generation[s];
      pp.rng = SND_R(Playback_seed)[d];
      pp.voiceListHead = kNoSlot;
      pp.categorySlot = im.globalCategory;
      SND_W(Playback_logic)[d] = PlaybackLogic{0u, 0u, 0u, 0u, 0u, 1.0f, 0.0f};
      SND_W(Playback_schedule)[d] = PlaybackSchedule{0, 0};
      SND_W(Playback_endInfo)[d] = PlaybackEnd{0, 0};
      const PlaybackRequest& rq = SND_R(Playback_request)[d];
      SND_W(Playback_state)[d] = (rq.startReservation != kNoSlot && (rq.flags & kRequestRejectReservationFull) == 0u)
                                     ? PlaybackStateId::Pending
                                     : PlaybackStateId::None;
    }
  }

  // 0b. D-145: immediate plays whose first-block actions ApplyCommands applied start here,
  //     before the fired reservations and the loop, so that a refused start cancels the
  //     action children before any of them plays, and the start block's StopEvent actions
  //     reach their targets in this block.
  {
    schema::TableRows& prows = SND_ROWS(Playback);
    for (std::uint32_t d = 0; d < prows.count;) {
      const SlotIndex s = base::rows::SlotAt(prows, d);
      const PlaybackRequest& rq = SND_R(Playback_request)[d];
      if ((rq.flags & kRequestActionsApplied) == 0u || SND_W(Playback_state)[d] != PlaybackStateId::None ||
          (Priv(im, s).flags & kPbStarted) != 0u) {
        ++d;
        continue;
      }
      const HandleRef self{s, prows.generation[s]};
      const std::uint32_t before = prows.count;
      if (StartPlayback(c, d, im.clock)) {
        ActionsAtStart(c, d, im.clock);
      } else {
        CancelActionChildren(c, self);
      }
      if (prows.count == before) ++d;
    }
  }
  // 0c. D-145: actions taken ahead whose block has come.
  RunPending(c);

  // 1. Fired reservations (in fire order, 2.5).
  for (std::uint32_t i = 0; i < im.fire.count; ++i) ApplyFireEntry(c, im.fire.entries[i]);

  // 2. Voices: source end, deferred controls of held voices, music gain ramps.
  //    (Virtualize / Devirtualize are sent by SelectVoices when it moves a voice.)
  {
    const schema::TableRows& vrows = t.Voice.rows;
    for (std::uint32_t d = 0; d < vrows.count; ++d) {
      const SlotIndex v = base::rows::SlotAt(vrows, d);
      VoiceStatic& vs = im.ev.vs[v];
      VoiceLifecycle& lc = SND_W(Voice_lifecycle)[d];
      // Voices created earlier in this block (fired reservations) have no render status yet.
      if (SND_W(Voice_createdBlock)[d] == im.block) continue;
      if (lc.state == static_cast<std::uint8_t>(VoiceStateId::Starting) && lc.startClock < blockEnd) {
        lc.state = static_cast<std::uint8_t>(VoiceStateId::Playing);
      }
      if (vs.musicGainStep != 0.0f) {  // music SetGain ramp: one step per block, ends on the target
        vs.musicGain += vs.musicGainStep;
        if ((vs.musicGainStep > 0.0f && vs.musicGain >= vs.musicGainTarget) ||
            (vs.musicGainStep < 0.0f && vs.musicGain <= vs.musicGainTarget)) {
          vs.musicGain = vs.musicGainTarget;
          vs.musicGainStep = 0.0f;
        }
        if (vs.musicGain < 0.0f) vs.musicGain = 0.0f;
      }
      // Pause / resume asked while the voice was held (D-43: no Control then): send the
      // final state once, now that the hold is gone.
      if ((vs.flags & kVsCtlPending) != 0u && SND_R(Voice_renderHold)[d].held == 0u) {
        vs.flags &= ~kVsCtlPending;
        const SlotIndex src = SND_W(Voice_sourceSlot)[d];
        if (src != kNoSlot && im.ev.controlCount < im.cfg.maxVirtual) {
          source::SourceControlOp& op = im.ev.controls[im.ev.controlCount];
          op = source::SourceControlOp{};
          op.sourceSlot = src;
          op.op = (vs.flags & kVsWantPaused) != 0u ? source::ControlOp::Pause : source::ControlOp::Resume;
          im.ev.controlKind[im.ev.controlCount++] = static_cast<std::uint32_t>(SND_W(Voice_sourceKind)[d]);
        }
      }
    }
    // Ended / released voices (not while held, D-43).
    for (std::uint32_t d = 0; d < vrows.count;) {
      const SlotIndex v = base::rows::SlotAt(vrows, d);
      const VoiceLifecycle lc = SND_W(Voice_lifecycle)[d];
      if (SND_W(Voice_createdBlock)[d] == im.block) {
        ++d;
        continue;
      }
      const bool ended = SND_R(Voice_renderStatus)[d].ended != 0u &&
                         lc.state != static_cast<std::uint8_t>(VoiceStateId::Starting);
      const bool released = lc.state == static_cast<std::uint8_t>(VoiceStateId::Releasing) && lc.stopClock <= im.clock;
      // D-140: a voice whose return mode is "never" ends once it went virtual (SelectVoices).
      bool dropped = (im.ev.vs[v].flags & kVsKillVirtual) != 0u;
      if (dropped) {
        // A voice of a pausing / paused playback is not ended (whatever the order of the pause
        // and the steal): it is judged again once it is resumed (SelectVoices).
        const SlotIndex vpb = SND_W(Voice_playbackSlot)[d];
        const bool pausing = (lc.flags & kVoicePausedFlag) != 0u ||
                             (vpb < im.cfg.maxPlaybacks &&
                              (Priv(im, vpb).flags & (kPbPausedUser | kPbPausedCategory | kPbPausedState)) != 0u);
        if (pausing) {
          im.ev.vs[v].flags &= ~kVsKillVirtual;
          dropped = false;
        }
      }
      if ((ended || released || dropped) && !VoiceHeld(c, d)) {
        const SlotIndex pb = SND_W(Voice_playbackSlot)[d];
        if (dropped && !ended && !released && pb < im.cfg.maxPlaybacks) Priv(im, pb).flags |= kPbVoiceDropped;
        DestroyVoice(c, v);  // the swapped-in row is now at d
        continue;
      }
      ++d;
    }
  }

  // 3. Playbacks.
  schema::TableRows& prow = SND_ROWS(Playback);
  for (std::uint32_t d = 0; d < prow.count;) {
    const SlotIndex s = base::rows::SlotAt(prow, d);
    const std::uint32_t gen = prow.generation[s];
    PlaybackPriv& pp = Priv(im, s);
    PlaybackLogic& lg = SND_W(Playback_logic)[d];
    const std::uint32_t before = prow.count;
    if (pp.generation != gen) {  // new row (ApplyCommands created it)
      pp = PlaybackPriv{};
      pp.generation = gen;
      pp.rng = SND_R(Playback_seed)[d];
      pp.voiceListHead = kNoSlot;
      pp.categorySlot = im.globalCategory;
      lg = PlaybackLogic{0u, 0u, 0u, 0u, 0u, 1.0f, 0.0f};
      SND_W(Playback_state)[d] = PlaybackStateId::None;
      SND_W(Playback_schedule)[d] = PlaybackSchedule{0, 0};
      SND_W(Playback_endInfo)[d] = PlaybackEnd{0, 0};
    }
    if ((pp.flags & kPbCancelled) != 0u) {  // D-145: an action child whose parent did not start
      if (!EndPlayback(c, d, SND_END_REJECTED, SND_END_DETAIL_NONE)) ++d;
      continue;
    }
    if ((pp.flags & kPbEndPending) != 0u) {
      if (!EndPlayback(c, d, pp.endReason, pp.endDetail)) ++d;
      continue;
    }
    // Requests (latest wins per flag; handled once per sequence number).
    const PlaybackRequest req = SND_R(Playback_request)[d];
    PlaybackStateId state = SND_W(Playback_state)[d];
    // D-97: a play of a user buffer being destroyed never starts (Pending ones neither).
    if ((req.flags & kRequestBufferDestroyed) != 0u &&
        (state == PlaybackStateId::None || state == PlaybackStateId::Pending)) {
      if (!EndPlayback(c, d, SND_END_DISCARDED, SND_END_DETAIL_NONE)) ++d;
      continue;
    }
    if (state == PlaybackStateId::None) {
      if ((req.flags & kRequestRejectReservationFull) != 0u) {
        EndPlayback(c, d, SND_END_REJECTED, SND_END_DETAIL_RESERVATION_FULL);
        continue;
      }

      const EventRef ref = SND_R(Playback_eventRef)[d];
      if (ref.bankSlot == kNoBank && (ref.flags & kEventRefDynamic) == 0u) {
        EndPlayback(c, d, SND_END_NOT_LOADED, SND_END_DETAIL_NONE);
        continue;
      }
      if (req.startReservation != kNoSlot) {
        SND_W(Playback_state)[d] = PlaybackStateId::Pending;
        state = PlaybackStateId::Pending;
      } else if (!StartPlayback(c, d, im.clock)) {
        continue;
      } else {
        ActionsAtStart(c, d, im.clock);
      }
      if (prow.count != before) continue;
      state = SND_W(Playback_state)[d];
    }
    if (req.seq != lg.handledRequestSeq) {
      lg.handledRequestSeq = req.seq;
      if ((req.flags & kRequestDiscard) != 0u) {
        if (state == PlaybackStateId::Pending) {
          EndPlayback(c, d, SND_END_DISCARDED, SND_END_DETAIL_BANK_UNLOADED);
          continue;
        }
        BeginStop(c, d, 0.0f, true);
      } else if ((req.flags & kRequestStop) != 0u) {
        if (state == PlaybackStateId::Pending) {
          EndPlayback(c, d, SND_END_STOPPED, SND_END_DETAIL_NONE);
          continue;
        }
        BeginStop(c, d, req.stopFadeSeconds, false);
      } else if ((req.flags & kRequestPause) != 0u && (pp.flags & kPbStopping) == 0u) {
        pp.flags |= kPbPausedUser;
        pp.fadeTarget = 0.0f;
        pp.fadeRate = FadeStepFor(im, req.pauseFadeSeconds);
      } else if ((req.flags & kRequestResume) != 0u && (pp.flags & kPbStopping) == 0u) {
        pp.flags &= ~kPbPausedUser;
      }
      state = SND_W(Playback_state)[d];
    }
    if ((pp.flags & kPbActionStop) != 0u) {  // D-145: a StopEvent action, from its clock
      pp.flags &= ~kPbActionStop;
      if (state == PlaybackStateId::Pending) {
        if (!EndPlayback(c, d, SND_END_STOPPED, SND_END_DETAIL_NONE)) ++d;
        continue;
      }
      BeginStop(c, d, -1.0f, false, pp.actionStopClock > im.clock ? pp.actionStopClock : im.clock);
      state = SND_W(Playback_state)[d];
    }
    if (state == PlaybackStateId::Pending) {
      // D-145: a scheduled start known ahead (CLOCK_SAMPLE / DELAY_US) that falls in the next
      // block: its first actions are taken now, so that they happen at their sample.
      const ClockSample startAt = im.ap.playbackStartClock[s];
      if (startAt >= blockEnd && startAt < blockEnd + im.cfg.blockFrames && pp.actionStart == 0u &&
          SND_R(Playback_eventRef)[d].bankSlot != kNoBank) {
        pp.actionStart = startAt;
        const std::uint64_t to = blockEnd + im.cfg.blockFrames - startAt;
        TakeActions(c, d, 0u, to, [&](std::uint64_t u) noexcept { return startAt + u; });
        pp.actionHorizon = to;
      }
      ++d;
      continue;
    }
    // Scheduled stop that fires later inside the lookahead.
    if (pp.stopClock != 0u && pp.stopClock < blockEnd && (pp.flags & kPbStopping) == 0u) {
      BeginStop(c, d, pp.stopFadeSeconds, false, pp.stopClock);
      state = SND_W(Playback_state)[d];
    }
    // Category / global pause (4.6, D-30), unless the category or event ignores pause.
    const EventRef ref = SND_R(Playback_eventRef)[d];
    bool ignorePause = CategoryIgnoresPause(im, pp.categorySlot);
    if (const BankRecord* br = RegisteredBank(im, ref.bankSlot); br != nullptr) {
      ignorePause = ignorePause || (br->view->Column<bk::col::EVNT_flags>()[ref.eventIndex] & bf::kEventIgnorePause) != 0u;
    }
    const bool catPaused = !ignorePause && CategoryPaused(im, pp.categorySlot);
    if (catPaused && (pp.flags & kPbPausedCategory) == 0u) {
      pp.flags |= kPbPausedCategory;
      pp.fadeRate = FadeStepFor(im, im.cfg.shortFadeSeconds);
    } else if (!catPaused) {
      pp.flags &= ~kPbPausedCategory;
    }
    const bool wantPause = (pp.flags & (kPbPausedUser | kPbPausedCategory)) != 0u && (pp.flags & kPbStopping) == 0u;
    // D-146: the fade-in of a play advances with played time only: held while the playback
    // pauses or stops (a stop then fades out from the level reached).
    if (im.ap.playbackFadeIn[s] > 0.0f) {
      const bool hold = (pp.flags & (kPbPausedUser | kPbPausedCategory | kPbStopping)) != 0u;
      for (SlotIndex v = pp.voiceListHead, guard = 0; v != kNoSlot && guard < im.cfg.maxVirtual; ++guard) {
        im.ev.vs[v].flags = hold ? (im.ev.vs[v].flags | kVsFadeInHold) : (im.ev.vs[v].flags & ~kVsFadeInHold);
        v = im.ev.vs[v].nextInPlayback;
      }
    }
    // Fade (stop / pause / resume): logic.fadeGain = value at the end of this block.
    const bool fadeStop = (pp.flags & kPbStopping) != 0u && (pp.flags & kPbTimedStop) == 0u;
    const float g0 = lg.fadeGain;
    // A playback stopping by voice releases keeps its level (the releases fade it).
    const bool timedStop = (pp.flags & kPbStopping) != 0u && (pp.flags & kPbTimedStop) != 0u;
    const float target = fadeStop ? 0.0f : (timedStop ? g0 : (wantPause ? 0.0f : 1.0f));
    float g1 = g0;
    if ((pp.flags & kPbPausedState) != 0u && !wantPause) {  // resume from Paused
      pp.flags &= ~kPbPausedState;
      SetVoicesPaused(c, s, false);
      SND_W(Playback_state)[d] = PlaybackStateId::Playing;
      pp.fadeRate = FadeStepFor(im, req.pauseFadeSeconds);
    }
    if ((pp.flags & kPbPausedState) == 0u) {
      const float rate = pp.fadeRate > 0.0f ? pp.fadeRate : 1.0f;
      if (g1 < target) g1 = g1 + rate > target ? target : g1 + rate;
      if (g1 > target) g1 = g1 - rate < target ? target : g1 - rate;
    }
    lg.fadeStep = g1 - g0;
    lg.fadeGain = g1;
    if (wantPause && g0 <= 0.0f && (pp.flags & kPbPausedState) == 0u) {
      pp.flags |= kPbPausedState;
      SetVoicesPaused(c, s, true);
      SND_W(Playback_state)[d] = PlaybackStateId::Paused;
      lg.fadeStep = 0.0f;
    }
    // Stop fade finished (the last block rendered the ramp to 0): end.
    if (fadeStop && g0 <= 0.0f) {
      if (!EndPlayback(c, d, pp.endReason, pp.endDetail)) ++d;
      continue;
    }
    // Position, action tracks.
    const PlaybackStateId st = SND_W(Playback_state)[d];
    if (st == PlaybackStateId::Playing || st == PlaybackStateId::Stopping) {
      const ClockSample startClock = SND_W(Playback_schedule)[d].startClock;
      std::uint64_t add = im.cfg.blockFrames;
      if (startClock > im.clock) add = startClock < blockEnd ? blockEnd - startClock : 0u;
      lg.playedFrames += add;
      // D-145: the track time does not advance while the playback pauses or stops; the
      // actions of the next block are taken ahead; none once it stops.
      if (add != 0u && ref.bankSlot != kNoBank && !wantPause && (pp.flags & kPbStopping) == 0u) {
        pp.trackTime += add;
        const std::uint64_t now = pp.trackTime;
        const std::uint64_t ahead = now + im.cfg.blockFrames;
        if (pp.actionHorizon < ahead) {
          TakeActions(c, d, pp.actionHorizon, ahead,
                      [&](std::uint64_t u) noexcept { return u >= now ? blockEnd + (u - now) : blockEnd; });
          pp.actionHorizon = ahead;
        }
      }
    }
    // All voices gone: the playback is complete (4.1).
    if (lg.voiceCount == 0u && st != PlaybackStateId::Pending &&
        ((pp.flags & kPbMusic) == 0u || (pp.flags & kPbTimedStop) != 0u)) {
      // A source that refused its voices (FlushSourceCalls: DECODER_POOL_FULL,
      // NO_STREAM_SLOT, ...) left the reason in endDetail: the playback is rejected.
      const bool stopping = (pp.flags & kPbStopping) != 0u;
      const bool refused = !stopping && pp.endDetail != SND_END_DETAIL_NONE && (pp.flags & kPbSourceStarted) == 0u;
      // D-140: a voice dropped when it went virtual ("never" return): the playback was stolen.
      const bool dropped = !stopping && !refused && (pp.flags & kPbVoiceDropped) != 0u;
      if (!EndPlayback(c, d,
                       stopping ? pp.endReason : (refused ? SND_END_REJECTED : (dropped ? SND_END_STOLEN : SND_END_COMPLETED)),
                       stopping || refused ? pp.endDetail : (dropped ? SND_END_DETAIL_VOICE_LIMIT : SND_END_DETAIL_NONE))) {
        ++d;
      }
      continue;
    }
    if (prow.count == before) ++d;
  }

  // 4. Music logic.
  RunMusic(c);

  // 5. Batched source calls.
  FlushSourceCalls(c);
  QueryPriming(c);

  // 6. Followed transports (1.2, D-90): position at this block's start, for Render.
  //    Transport.position is the block END (UpdateTransport ran before this system).
  {
    const schema::TableRows& vrows = t.Voice.rows;
    for (std::uint32_t d = 0; d < vrows.count; ++d) {
      VoiceStatic& vs = im.ev.vs[base::rows::SlotAt(vrows, d)];
      if (vs.followTransport.generation == 0u) continue;
      const std::uint32_t td = base::rows::DenseOf(t.Transport.rows, vs.followTransport);
      if (td == kNoSlot) {  // the transport is gone: no position any more
        vs.followTransport = HandleRef{};
        vs.transportBeats = -1.0;
        vs.transportBeatsPerSample = 0.0;
        continue;
      }
      const TransportPosition pos = SND_R(Transport_position)[td];
      const bool playing = SND_R(Transport_state)[td] == SND_TRANSPORT_PLAYING;
      const double tps = playing ? pos.ticksPerSample : 0.0;
      const double endTicks = static_cast<double>(pos.tick) + pos.tickFraction;
      const double back = pos.anchorClock > im.clock ? static_cast<double>(pos.anchorClock - im.clock) : 0.0;
      double ticks = endTicks - back * tps;
      if (ticks < 0.0) ticks = 0.0;
      vs.transportBeats = ticks / static_cast<double>(SND_TICKS_PER_BEAT);
      vs.transportBeatsPerSample = tps / static_cast<double>(SND_TICKS_PER_BEAT);
    }
  }
}

}  // namespace snd::core
