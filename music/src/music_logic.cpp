// Music logic inside RunEventLogic (music_if.hpp): inputs from core, the per
// playback update (transition timing, playlist continuation, voice requests,
// stem volumes, markers, stalls of the sync group) and the voice results.
//
// Voice requests are derived from state every block (idempotent): a request
// that does not fit into the list this block is made again in the next one.
// Each local voice remembers the core voice slot and the block it was created
// in; before a StopVoice / SetGain the music system checks that the slot still
// holds that voice of this playback (a slot is reused after its voice ended).
#include <bit>
#include <cmath>

#include "music_state.hpp"
#include "snd/base/sparse_set.hpp"

namespace snd::music {

namespace {

using namespace detail;
constexpr schema::SystemId kSys = schema::SystemId::RunEventLogic;
#define SND_MR(col) (::snd::schema::ReadColumn<kSys, ::snd::schema::ColumnId::col>(t))

MusicPb* Acquire(State& s, const schema::Tables& t, SlotIndex playback, const EventRef& event) noexcept {
  if (playback >= s.limits.maxPlaybacks) return nullptr;
  const std::uint32_t pd = base::rows::DenseOf(t.Playback.rows, playback);
  if (pd == kInvalidSlot) return nullptr;
  const std::uint32_t gen = t.Playback.rows.generation[playback];
  if (MusicPb* pb = PbOf(s, playback); pb != nullptr) {
    if (pb->generation == gen) return pb;
    ReleasePb(s, *pb);  // the slot was reused
  }
  for (std::uint32_t i = 0; i < s.pbCount; ++i) {
    MusicPb& pb = s.pbs[i];
    if (pb.used != 0u) continue;
    pb = MusicPb{};
    pb.used = 1;
    pb.playback = playback;
    pb.generation = gen;
    pb.bank = event.bankSlot;
    pb.eventRow = event.eventIndex;
    pb.transport = SND_MR(Playback_transportSlot)[pd];
    pb.rng = SND_MR(Playback_seed)[pd] | 1u;
    pb.syncGroup = playback + 1u;
    pb.head = -1;
    pb.stateRow = bf::kNone;
    pb.phase = Phase::Idle;
    for (Instance& in : pb.inst) in.role = Role::Free;
    s.pbOfSlot[playback] = static_cast<std::uint16_t>(i);
    return &pb;
  }
  return nullptr;
}

bool BindBank(State& s, std::uint16_t bank) noexcept {
  if (s.tb.bank == bank && s.tb.evnt.Rows() != 0u) return true;
  return s.tb.Bind(s.services, bank);
}

// --- parameters (4.3 lookup order: playback -> emitter -> global -> PARM default) ---
bool FindChain(const schema::TableRows& rows, const Id* ids, const SlotIndex* next, const float* cur, SlotIndex owner,
               Id id, float* out) noexcept {
  if (owner == kInvalidSlot) return false;
  SlotIndex sl = owner * kParamInlineSlots;
  for (std::uint32_t guard = 0; guard < rows.capacity && sl != kInvalidSlot; ++guard) {
    const std::uint32_t d = base::rows::DenseOf(rows, sl);
    if (d == kInvalidSlot) return false;
    if (ids[d] == id) {
      *out = cur[d];
      return true;
    }
    sl = next[d];
  }
  return false;
}

float ParamValue(const State& s, const schema::Tables& t, Id id, SlotIndex playback, SlotIndex emitter) noexcept {
  float v = 0.0f;
  if (FindChain(t.ParamPlayback.rows, SND_MR(ParamPlayback_paramId), SND_MR(ParamPlayback_nextSlot),
                SND_MR(ParamPlayback_current), playback, id, &v)) {
    return v;
  }
  if (FindChain(t.ParamEmitter.rows, SND_MR(ParamEmitter_paramId), SND_MR(ParamEmitter_nextSlot),
                SND_MR(ParamEmitter_current), emitter, id, &v)) {
    return v;
  }
  const schema::TableRows& g = t.ParamGlobal.rows;
  const Id* gid = SND_MR(ParamGlobal_paramId);
  for (std::uint32_t d = 0; d < g.count; ++d) {
    if (gid[d] == id) return SND_MR(ParamGlobal_current)[d];
  }
  for (std::uint32_t r = 0; r < s.tb.parm.Rows(); ++r) {
    if (s.tb.parm.U32(bf::col::PARM_id, r) == id) return s.tb.parm.F32(bf::col::PARM_defaultValue, r);
  }
  return 0.0f;
}

float EvaluateCurve(const BankTables& tb, std::uint32_t curve, float x) noexcept {
  if (!tb.curv.Has(curve)) return 0.0f;
  const std::uint32_t first = tb.curv.U32(bf::col::CURV_pointFirst, curve);
  const std::uint32_t n = tb.curv.U32(bf::col::CURV_pointCount, curve);
  if (n == 0u || first >= tb.cpnt.Rows() || n > tb.cpnt.Rows() - first) return 0.0f;
  if (x <= tb.cpnt.F32(bf::col::CPNT_x, first) || n == 1u) return tb.cpnt.F32(bf::col::CPNT_y, first);
  for (std::uint32_t i = 1; i < n; ++i) {
    const std::uint32_t r = first + i;
    const float x1 = tb.cpnt.F32(bf::col::CPNT_x, r);
    if (x <= x1) {
      const float x0 = tb.cpnt.F32(bf::col::CPNT_x, r - 1u);
      const float y0 = tb.cpnt.F32(bf::col::CPNT_y, r - 1u);
      const float y1 = tb.cpnt.F32(bf::col::CPNT_y, r);
      float u = x1 > x0 ? (x - x0) / (x1 - x0) : 1.0f;
      const float shape = tb.cpnt.F32(bf::col::CPNT_shape, r - 1u);
      if (shape > 0.0f && shape != 1.0f) u = std::pow(u, shape);
      return y0 + (y1 - y0) * u;
    }
  }
  return tb.cpnt.F32(bf::col::CPNT_y, first + n - 1u);
}

// Linear gain of a stem volume curve (CURV target volume, dB).
float CurveGain(const State& s, const schema::Tables& t, std::uint32_t curve, SlotIndex playback,
                SlotIndex emitter) noexcept {
  const BankTables& tb = s.tb;
  if (!tb.curv.Has(curve)) return 1.0f;
  if (tb.curv.U32(bf::col::CURV_target, curve) != static_cast<std::uint32_t>(bf::CurveTarget::Volume)) return 1.0f;
  const float x = ParamValue(s, t, tb.curv.U32(bf::col::CURV_input, curve), playback, emitter);
  const float db = EvaluateCurve(tb, curve, x);
  return db <= -144.0f ? 0.0f : std::pow(10.0f, db / 20.0f);
}

// Gain of a local voice: a separate-file stem's curve, or for the main material
// the curves of the interleaved stems that cover all of its channels.
float VoiceGain(const State& s, const schema::Tables& t, const MusicPb& pb, const LocalVoice& lv,
                std::uint32_t seg, SlotIndex emitter) noexcept {
  const BankTables& tb = s.tb;
  if (lv.stem != bf::kNone) {
    return CurveGain(s, t, tb.mstm.U32(bf::col::MSTM_volumeCurve, lv.stem), pb.playback, emitter);
  }
  float g = 1.0f;
  const std::uint32_t channels = tb.asst.U32(bf::col::ASST_channels, lv.asset);
  for (std::uint32_t r = 0; r < tb.mstm.Rows(); ++r) {
    if (tb.mstm.U32(bf::col::MSTM_segment, r) != seg || tb.mstm.U32(bf::col::MSTM_asset, r) != bf::kNone) continue;
    if (tb.mstm.U32(bf::col::MSTM_channelFirst, r) != 0u || tb.mstm.U32(bf::col::MSTM_channelCount, r) < channels) continue;
    g *= CurveGain(s, t, tb.mstm.U32(bf::col::MSTM_volumeCurve, r), pb.playback, emitter);
  }
  return g;
}

bool VoiceAlive(const schema::Tables& t, const MusicPb& pb, const LocalVoice& lv) noexcept {
  if (lv.status != VoiceStatus::Live) return false;
  const std::uint32_t d = base::rows::DenseOf(t.Voice.rows, lv.slot);
  return d != kInvalidSlot && SND_MR(Voice_playbackSlot)[d] == pb.playback && SND_MR(Voice_createdBlock)[d] == lv.createdBlock;
}

MusicVoiceRequest* NewRequest(MusicVoiceRequestList& out) noexcept {
  if (out.requests == nullptr || out.count >= out.capacity) return nullptr;
  MusicVoiceRequest& r = out.requests[out.count++];
  r = MusicVoiceRequest{};
  r.voiceSlot = kInvalidSlot;
  return &r;
}

std::int32_t FreeVoice(const MusicPb& pb) noexcept {
  for (std::uint32_t i = 0; i < kMaxLocalVoices; ++i) {
    if (pb.voice[i].status == VoiceStatus::Free) return static_cast<std::int32_t>(i);
  }
  return -1;
}

std::uint32_t FreeVoiceCount(const MusicPb& pb) noexcept {
  std::uint32_t n = 0;
  for (const LocalVoice& lv : pb.voice) n += lv.status == VoiceStatus::Free ? 1u : 0u;
  return n;
}

std::uint32_t TagOf(const State& s, const MusicPb& pb, std::uint32_t local) noexcept {
  const std::uint32_t index = static_cast<std::uint32_t>(&pb - s.pbs);
  return ((index + 1u) << 20) | ((pb.serial & 0xFFFu) << 8) | local;
}

// Starts the voices of a planned instance once its first sample is near (prime time ahead).
void IssueInstance(State& s, const schema::Tables& t, MusicPb& pb, Timeline& tl, Instance& in, std::uint32_t idx,
                   SlotIndex emitter, MusicVoiceRequestList& out) noexcept {
  const BankTables& tb = s.tb;
  SegmentInfo si;
  if (!DescribeSegment(tb, in.seg, s.rate, &si)) {
    in.role = Role::Free;
    return;
  }
  double cAnchor = 0.0;
  if (!tl.ClockAtTick(static_cast<double>(in.anchorTick), &cAnchor)) return;
  double startClock = 0.0;
  double startFrame = 0.0;
  if (in.anchorTick == in.startTick) {  // from the pickup
    startClock = cAnchor - static_cast<double>(si.entry - si.pickup) * si.ratio;
    startFrame = static_cast<double>(si.pickup);
  } else {  // entering in the middle (same bar, marker, last position, StartLate)
    startClock = cAnchor;
    startFrame = static_cast<double>(si.entry) +
                 SegmentSamples(tb, si, static_cast<double>(in.anchorTick - in.startTick), s.rate) / si.ratio;
  }
  if (startClock < static_cast<double>(s.blockStart)) {
    startFrame += (static_cast<double>(s.blockStart) - startClock) / si.ratio;
    startClock = static_cast<double>(s.blockStart);
  }
  const ClockSample sc = RoundClock(startClock);
  const std::uint64_t sf = static_cast<std::uint64_t>(std::floor(startFrame + 0.5));
  if (sc >= s.BlockEnd() + PrimeOf(s, pb.bank, si.asset, sf)) return;  // not yet
  if (si.totalFrames != 0u && sf >= si.totalFrames) {  // nothing left to play
    in.phase = InstPhase::Issued;
    in.voiceClock = sc;
    in.voiceFrame = sf;
    in.markerCursor = sc;
    return;
  }
  // Separate-file stems of the segment (8.4: one sync group per playback).
  std::uint32_t stems[kMaxInstanceVoices - 1u];
  std::uint32_t nStems = 0;
  for (std::uint32_t r = 0; r < tb.mstm.Rows() && nStems < kMaxInstanceVoices - 1u; ++r) {
    if (tb.mstm.U32(bf::col::MSTM_segment, r) == in.seg && tb.asst.Has(tb.mstm.U32(bf::col::MSTM_asset, r))) {
      stems[nStems++] = r;
    }
  }
  if (FreeVoiceCount(pb) < 1u + nStems || out.count + 1u + nStems > out.capacity) return;  // next block
  pb.soundPriority = si.soundPriority;
  in.phase = InstPhase::Issued;
  in.voiceClock = sc;
  in.voiceFrame = sf;
  in.markerCursor = sc;
  in.voiceCount = 0;
  for (std::uint32_t k = 0; k <= nStems; ++k) {
    const std::int32_t local = FreeVoice(pb);
    LocalVoice& lv = pb.voice[local];
    lv = LocalVoice{};
    lv.inst = static_cast<std::uint8_t>(idx);
    lv.stem = k == 0u ? bf::kNone : stems[k - 1u];
    lv.asset = k == 0u ? si.asset : tb.mstm.U32(bf::col::MSTM_asset, stems[k - 1u]);
    lv.status = VoiceStatus::Requested;
    ++pb.serial;
    lv.tag = TagOf(s, pb, static_cast<std::uint32_t>(local));
    lv.slot = kInvalidSlot;
    lv.gain = VoiceGain(s, t, pb, lv, in.seg, emitter);
    std::uint64_t frame = sf;
    if (k != 0u) {  // a stem file at its own rate
      const std::uint32_t rate = tb.asst.U32(bf::col::ASST_sampleRate, lv.asset);
      if (rate != 0u && rate != si.assetRate) {
        frame = static_cast<std::uint64_t>(std::floor(startFrame * rate / si.assetRate + 0.5));
      }
    }
    in.voices[in.voiceCount++] = static_cast<std::uint8_t>(local);
    MusicVoiceRequest* rq = NewRequest(out);
    rq->op = MusicVoiceOp::StartClip;
    rq->playbackSlot = pb.playback;
    rq->tag = lv.tag;
    rq->clip = AssetClip(pb.bank, lv.asset);
    rq->startClock = sc;
    rq->startFrame = frame;
    rq->gain = lv.gain;
    rq->fadeSeconds = in.fadeIn;
    rq->syncGroup = pb.syncGroup;
  }
}

void ProcessInstances(State& s, const schema::Tables& t, MusicPb& pb, Timeline& tl, SlotIndex emitter,
                      MusicVoiceRequestList& out) noexcept {
  const double nowTick = tl.TickAtClock(static_cast<double>(s.blockStart));
  const std::uint64_t handle = HandleRef{pb.playback, pb.generation}.Value();
  for (std::uint32_t i = 0; i < kMaxInstances; ++i) {
    Instance& in = pb.inst[i];
    if (in.role == Role::Free) continue;
    if (in.phase == InstPhase::Planned) {
      if (pb.stopping == 0u) IssueInstance(s, t, pb, tl, in, i, emitter, out);
      if (in.phase == InstPhase::Planned) continue;
    }
    // Stops (sample accurate release from stopClock).
    bool anyVoice = false;
    for (std::uint32_t k = 0; k < in.voiceCount; ++k) {
      const std::uint8_t v = in.voices[k];
      if (v >= kMaxLocalVoices) continue;
      LocalVoice& lv = pb.voice[v];
      if (lv.status == VoiceStatus::Free || lv.inst != i) continue;
      if (lv.status == VoiceStatus::Live && !VoiceAlive(t, pb, lv)) {
        lv.status = VoiceStatus::Free;  // ended without a VoiceEnded we could match
        continue;
      }
      anyVoice = true;
      if (in.stopPending != 0u && lv.stopSent == 0u && lv.status == VoiceStatus::Live) {
        MusicVoiceRequest* rq = NewRequest(out);
        if (rq == nullptr) continue;
        rq->op = MusicVoiceOp::StopVoice;
        rq->playbackSlot = pb.playback;
        rq->voiceSlot = lv.slot;
        rq->startClock = in.stopClock > s.blockStart ? in.stopClock : 0u;
        rq->fadeSeconds = in.stopFade;
        lv.stopSent = 1;
      }
    }
    // Markers crossed in this block (8.1 notifications).
    if (s.tb.mmrk.Rows() != 0u && in.markerCursor < s.BlockEnd()) {
      SegmentInfo si;
      if (DescribeSegment(s.tb, in.seg, s.rate, &si) && si.markerFirst != bf::kNone) {
        for (std::uint32_t k = 0; k < si.markerCount; ++k) {
          const std::uint32_t r = si.markerFirst + k;
          const std::uint64_t f = s.tb.mmrk.U64(bf::col::MMRK_frame, r);
          if (f < in.voiceFrame) continue;
          const ClockSample c =
              RoundClock(static_cast<double>(in.voiceClock) + static_cast<double>(f - in.voiceFrame) * in.ratio);
          if (c < in.markerCursor || c >= s.BlockEnd()) continue;
          if (in.stopPending != 0u && c >= in.stopClock) continue;
          PushNote(s, notify::EventType::Marker, s.tb.mmrk.U32(bf::col::MMRK_id, r), handle, c);
        }
      }
      in.markerCursor = s.BlockEnd();
    }
    // Done: no voice left and musically over (or stopped).
    const bool over = nowTick >= static_cast<double>(in.EndTick()) - 0.5 ||
                      (in.stopPending != 0u && s.blockStart >= in.stopClock);
    if (!anyVoice && over) {
      if (pb.head == static_cast<std::int32_t>(i) && in.successor == 0u) continue;  // keep the playlist head
      in.role = Role::Free;
      if (pb.head == static_cast<std::int32_t>(i)) pb.head = -1;
    }
  }
}

void UpdateStemGains(State& s, const schema::Tables& t, MusicPb& pb, SlotIndex emitter,
                     MusicVoiceRequestList& out) noexcept {
  const float ramp = static_cast<float>(s.blockFrames) / static_cast<float>(s.rate);
  for (LocalVoice& lv : pb.voice) {
    if (lv.status != VoiceStatus::Live || lv.inst >= kMaxInstances) continue;
    const Instance& in = pb.inst[lv.inst];
    if (in.role == Role::Free) continue;
    const float g = VoiceGain(s, t, pb, lv, in.seg, emitter);
    if (std::fabs(g - lv.gain) <= 1e-5f || !VoiceAlive(t, pb, lv)) continue;
    MusicVoiceRequest* rq = NewRequest(out);
    if (rq == nullptr) return;
    rq->op = MusicVoiceOp::SetGain;
    rq->playbackSlot = pb.playback;
    rq->voiceSlot = lv.slot;
    rq->gain = g;
    rq->fadeSeconds = ramp;
    lv.gain = g;
  }
}

// 8.4 "sound priority": the transport waits for the sync group's starved data.
void StallCheck(State& s, const schema::Tables& t, MusicPb& pb, Timeline& tl) noexcept {
  if (pb.soundPriority == 0u) return;
  const ClockSample prevStart = s.blockStart >= s.blockFrames ? s.blockStart - s.blockFrames : 0u;
  std::uint32_t deficit = 0;
  for (const LocalVoice& lv : pb.voice) {
    if (!VoiceAlive(t, pb, lv) || lv.createdBlock >= s.block) continue;
    const std::uint32_t d = base::rows::DenseOf(t.Voice.rows, lv.slot);
    const VoiceRenderStatus st = SND_MR(Voice_renderStatus)[d];
    if (st.starved == 0u) continue;
    const VoiceLifecycle lc = SND_MR(Voice_lifecycle)[d];
    std::uint32_t expected = s.blockFrames;
    if (lc.startClock > prevStart) {
      expected = lc.startClock < s.blockStart ? static_cast<std::uint32_t>(s.blockStart - lc.startClock) : 0u;
    }
    const std::uint32_t miss = expected > st.framesRendered ? expected - st.framesRendered : 0u;
    if (miss > deficit) deficit = miss;
  }
  if (deficit == 0u) return;
  tl.Delay(deficit);
  for (Instance& in : pb.inst) {
    if (in.role == Role::Free || in.phase != InstPhase::Issued) continue;
    in.voiceClock += deficit;  // the material is that much later now
    in.markerCursor += deficit;
    if (in.stopPending != 0u) in.stopClock += deficit;
  }
  if (pb.transEnd != 0u) pb.transEnd += deficit;
}

void IssueStingers(State& s, MusicPb& pb, MusicVoiceRequestList& out) noexcept {
  std::uint32_t kept = 0;
  for (std::uint32_t i = 0; i < pb.stingerCount; ++i) {
    const std::uint32_t row = FindStinger(s.tb, pb.stingers[i]);
    if (row == bf::kNone) continue;
    MusicVoiceRequest* rq = NewRequest(out);
    if (rq == nullptr) {
      pb.stingers[kept++] = pb.stingers[i];
      continue;
    }
    rq->op = MusicVoiceOp::PlayEvent;
    rq->playbackSlot = pb.playback;
    rq->event = EventRef{pb.bank, 0u, s.tb.mstg.U32(bf::col::MSTG_event, row)};
    rq->startClock = s.blockStart;
  }
  pb.stingerCount = kept;
}

}  // namespace

namespace detail {

void ReleasePb(State& s, MusicPb& pb) noexcept {
  if (pb.playback < s.limits.maxPlaybacks) s.pbOfSlot[pb.playback] = kNoPb;
  pb.used = 0;
}

void UpdatePlayback(State& s, const schema::Tables& t, MusicPb& pb, MusicVoiceRequestList& out) noexcept {
  const std::uint32_t pd = base::rows::DenseOf(t.Playback.rows, pb.playback);
  if (pd == kInvalidSlot || t.Playback.rows.generation[pb.playback] != pb.generation) {
    ReleasePb(s, pb);
    return;
  }
  Timeline* tl = TimelineOf(s, pb.transport);
  if (pb.started == 0u || tl == nullptr || !BindBank(s, pb.bank)) return;
  const SlotIndex emitter = SND_MR(Playback_emitterSlot)[pd];
  // Pause of the playback freezes its transport (voices hold their position).
  const bool paused = SND_MR(Playback_state)[pd] == PlaybackStateId::Paused;
  if (paused && pb.paused == 0u) {
    tl->PauseAt(s.blockStart);
    pb.paused = 1;
  } else if (!paused && pb.paused != 0u) {
    tl->ResumeAt(s.blockStart);
    pb.paused = 0;
  }
  if (pb.paused != 0u) return;
  if (pb.seekPending != 0u) {
    pb.seekPending = 0;
    if (pb.stopping == 0u) Relocate(s, s.tb, pb, *tl);
  }
  StallCheck(s, t, pb, *tl);
  if (pb.stopping == 0u) {
    // Transition state machine (8.6).
    if (pb.phase == Phase::Transitioning && s.blockStart >= pb.transEnd) {
      pb.phase = Phase::Idle;
      if (pb.hasQueued != 0u) {
        pb.hasQueued = 0;
        HandleSetState(s, s.tb, pb, pb.queuedTarget, s.blockStart, false);
      }
    }
    if (pb.phase == Phase::Pending) {
      double c = 0.0;
      if (tl->ClockAtTick(pb.pendTick, &c) &&
          c < static_cast<double>(s.BlockEnd()) + static_cast<double>(pb.pendPrime)) {
        ExecuteTransition(s, s.tb, pb, *tl);
      }
    }
    // Playlist continuation: pick the next segment ahead of the current one's end.
    if (pb.head >= 0) {
      Instance& h = pb.inst[pb.head];
      double cEnd = 0.0;
      if (h.role == Role::Main && h.successor == 0u && tl->ClockAtTick(static_cast<double>(h.EndTick()), &cEnd) &&
          cEnd < static_cast<double>(s.BlockEnd()) + kPlanAheadSeconds * s.rate) {
        bool hasFree = false;
        for (const Instance& in : pb.inst) hasFree = hasFree || in.role == Role::Free;
        if (hasFree) {
          const std::uint32_t seg = CursorNext(pb.cursor, s.tb, pb.rng);
          h.successor = 1;
          const MusicalTicks end = h.EndTick();
          const std::uint32_t stateRow = h.stateRow;
          if (seg != bf::kNone) {
            const std::int32_t n = PlanInstance(s, s.tb, pb, *tl, Role::Main, seg, stateRow, end, end, 0.0f);
            if (n >= 0) pb.head = n;
          }
        }
      }
    }
  }
  ProcessInstances(s, t, pb, *tl, emitter, out);
  if (pb.stopping == 0u) {
    UpdateStemGains(s, t, pb, emitter, out);
    IssueStingers(s, pb, out);
  }
}

}  // namespace detail

void MusicSystem::RunMusicLogic(const MusicFrame& frame, const schema::Tables& t, const MusicInput* inputs,
                                std::uint32_t count, MusicVoiceRequestList& out) noexcept {
  if (s_ == nullptr) return;
  State& s = *s_;
  s.block = frame.block.index;
  s.blockStart = frame.block.clock;
  s.blockFrames = frame.block.frames != 0u ? frame.block.frames : s.limits.blockFrames;
  s.tb.bank = 0xFFFFu;
  s.tb.evnt.Bind(0, nullptr, 0);
  for (std::uint32_t i = 0; i < count; ++i) {
    const MusicInput& in = inputs[i];
    switch (in.kind) {
      case MusicInputKind::PlaybackStarted: {
        MusicPb* pb = Acquire(s, t, in.playbackSlot, in.event);
        if (pb == nullptr || pb->started != 0u) break;
        if (in.transportSlot != kInvalidSlot) pb->transport = in.transportSlot;
        if (BindBank(s, pb->bank)) StartMusic(s, s.tb, *pb, in.clock);
        break;
      }
      case MusicInputKind::PlaybackStopping:
        if (MusicPb* pb = PbOf(s, in.playbackSlot); pb != nullptr) {
          pb->stopping = 1;
          pb->hasQueued = 0;
          if (pb->phase == Phase::Pending) pb->phase = Phase::Idle;
        }
        break;
      case MusicInputKind::PlaybackEnded:
        if (MusicPb* pb = PbOf(s, in.playbackSlot); pb != nullptr) ReleasePb(s, *pb);
        break;
      case MusicInputKind::SetState: {
        MusicPb* pb = Acquire(s, t, in.playbackSlot, in.event);
        if (pb != nullptr && BindBank(s, pb->bank)) {
          HandleSetState(s, s.tb, *pb, in.stateId, in.clock < s.blockStart ? s.blockStart : in.clock,
                         in.whenOverride != 0u);
        }
        break;
      }
      case MusicInputKind::PrepareState:
        if (MusicPb* pb = Acquire(s, t, in.playbackSlot, in.event); pb != nullptr) pb->prepared = in.stateId;
        break;
      case MusicInputKind::Stinger:
        if (MusicPb* pb = PbOf(s, in.playbackSlot); pb != nullptr && pb->stingerCount < kMaxPendingStingers) {
          pb->stingers[pb->stingerCount++] = in.stateId;
        }
        break;
      case MusicInputKind::VoiceEnded:
        if (MusicPb* pb = PbOf(s, in.playbackSlot); pb != nullptr) {
          for (LocalVoice& lv : pb->voice) {
            if (lv.status == VoiceStatus::Live && lv.slot == in.voiceSlot) lv.status = VoiceStatus::Free;
          }
        }
        break;
    }
  }
  for (std::uint32_t i = 0; i < s.pbCount; ++i) {
    if (s.pbs[i].used != 0u) UpdatePlayback(s, t, s.pbs[i], out);
  }
}

void MusicSystem::OnVoiceResults(const MusicVoiceResult* results, std::uint32_t count) noexcept {
  if (s_ == nullptr) return;
  State& s = *s_;
  for (std::uint32_t i = 0; i < count; ++i) {
    const MusicVoiceResult& r = results[i];
    const std::uint32_t index = r.tag >> 20;
    if (index == 0u || index > s.pbCount) continue;
    MusicPb& pb = s.pbs[index - 1u];
    const std::uint32_t local = r.tag & 0xFFu;
    if (pb.used == 0u || local >= kMaxLocalVoices) continue;
    LocalVoice& lv = pb.voice[local];
    if (lv.status != VoiceStatus::Requested || lv.tag != r.tag) continue;
    if (r.voiceSlot == kInvalidSlot) {
      lv.status = VoiceStatus::Free;
      continue;
    }
    lv.status = VoiceStatus::Live;
    lv.slot = r.voiceSlot;
    lv.createdBlock = s.block;
  }
}

}  // namespace snd::music
