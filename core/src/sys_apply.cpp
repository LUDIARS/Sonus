// System 1: ApplyCommands (design 2.1, 3.2). Drains every producer in
// registration order (normal region, reserve region, fallback section of each
// arena, commands.hpp), creates / destroys rows, writes requests, and handles
// bank registration and unloading (10.3).
#include <bit>
#include <cmath>
#include <cstring>

#include "access.hpp"
#include "core_impl.hpp"

namespace snd::core {

namespace {

constexpr schema::SystemId kSys = schema::SystemId::ApplyCommands;

bool IsMusicalWhen(std::uint8_t kind) noexcept { return kind >= SND_WHEN_NEXT_BEAT && kind <= SND_WHEN_SEGMENT_END; }

ReservationWhen ToReservationWhen(const snd_when& w) noexcept {
  ReservationWhen r{};
  r.value = w.value;
  r.offsetTicks = w.offset_ticks;
  r.count = w.count;
  r.kind = w.kind;
  r.flags = w.flags;
  r.lateRule = static_cast<std::uint8_t>(ReservationLateRule::NextGrid);
  return r;
}

// Transport slot of a transport handle (standalone or playback-owned), kNoSlot if gone.
SlotIndex TransportSlotOf(Impl& im, HandleRef h) noexcept {
  schema::Tables& t = im.tables;
  if (h.generation == 0u) return kNoSlot;
  if ((h.slot & SND_TRANSPORT_PLAYBACK_BIT) != 0u) {
    const HandleRef pb{h.slot & ~SND_TRANSPORT_PLAYBACK_BIT, h.generation};
    const std::uint32_t d = base::rows::DenseOf(t.Playback.rows, pb);
    return d == kNoSlot ? kNoSlot : SND_W(Playback_transportSlot)[d];
  }
  return base::rows::IsCurrent(t.Transport.rows, h) ? h.slot : kNoSlot;
}

// Creates a Reservation row (8.2). kNoSlot when the table is full.
SlotIndex NewReservation(Impl& im, SlotIndex transport, const snd_when& when, ReservationAction action,
                         HandleRef target, const ReservationPayload& payload) noexcept {
  schema::Tables& t = im.tables;
  schema::TableRows& rows = SND_ROWS(Reservation);
  const std::uint32_t cap = rows.capacity;
  for (std::uint32_t n = 0; n < cap; ++n) {
    const std::uint32_t s = (im.ap.reservationCursor + n) % cap;
    if (base::rows::IsLive(rows, s)) continue;
    const std::uint32_t d = base::rows::InsertNext(rows, s);
    if (d == kNoSlot) return kNoSlot;
    im.ap.reservationCursor = (s + 1u) % cap;
    SND_W(Reservation_transportSlot)[d] = transport;
    SND_W(Reservation_when)[d] = ToReservationWhen(when);
    SND_W(Reservation_action)[d] = action;
    SND_W(Reservation_target)[d] = target;
    SND_W(Reservation_payload)[d] = payload;
    SND_W(Reservation_acceptedBlock)[d] = im.block;
    SND_W(Reservation_sequence)[d] = ++im.ap.reservationSeq;
    return s;
  }
  return kNoSlot;
}

// For a non-immediate When: the transport slot (musical kinds), or kNoSlot.
// Musical kinds with a dead transport become immediate (+ warning, snd_when doc).
bool ScheduledWhen(Impl& im, snd_when& w, SlotIndex* transport, std::uint64_t object) noexcept {
  *transport = kNoSlot;
  if (w.kind == SND_WHEN_IMMEDIATE) return false;
  if (IsMusicalWhen(w.kind)) {
    *transport = TransportSlotOf(im, HandleRef::FromValue(w.transport.value));
    if (*transport == kNoSlot) {
      PostWarning(im, SND_ERR_INVALID_HANDLE, SND_END_DETAIL_TRANSPORT_GONE, object);
      w.kind = static_cast<std::uint8_t>(SND_WHEN_IMMEDIATE);
      return false;
    }
  }
  return true;
}

ReservationPayload PayloadOf(const void* p, std::size_t n) noexcept {
  ReservationPayload r{};
  std::memcpy(r.bytes, p, n < sizeof(r.bytes) ? n : sizeof(r.bytes));
  return r;
}

// --- parameter chains (3.1: 8 inline rows per owner + shared overflow blocks) ---
void ResetChain(Impl& im, ParamChain& ch, bool playbackScope) noexcept {
  base::IndexFreeList& blocks = playbackScope ? im.ap.playbackBlocks : im.ap.emitterBlocks;
  std::uint32_t* next = playbackScope ? im.ap.playbackBlockNext : im.ap.emitterBlockNext;
  for (std::uint32_t b = ch.firstBlock; b != kNoSlot;) {
    const std::uint32_t n = next[b];
    blocks.Push(b);
    b = n;
  }
  ch = ParamChain{};
}

// Slot of the k-th row of an owner's chain (allocating an overflow block when needed).
SlotIndex ChainSlot(Impl& im, ParamChain& ch, SlotIndex owner, std::uint32_t k, bool playbackScope) noexcept {
  if (k < kParamInlineSlots) return owner * kParamInlineSlots + k;
  const std::uint32_t inlineRows = (playbackScope ? im.cfg.maxPlaybacks : im.cfg.maxEmitters) * kParamInlineSlots;
  const std::uint32_t kb = (k - kParamInlineSlots) / kParamOverflowBlockEntries;
  const std::uint32_t ki = (k - kParamInlineSlots) % kParamOverflowBlockEntries;
  std::uint32_t* next = playbackScope ? im.ap.playbackBlockNext : im.ap.emitterBlockNext;
  std::uint32_t b = ch.firstBlock;
  for (std::uint32_t i = 0; i < kb && b != kNoSlot; ++i) b = next[b];
  if (b == kNoSlot && ki == 0u) {
    base::IndexFreeList& blocks = playbackScope ? im.ap.playbackBlocks : im.ap.emitterBlocks;
    b = blocks.Capacity() == 0u ? kNoSlot : blocks.Pop();
    if (b == base::kInvalidIndex) return kNoSlot;
    next[b] = kNoSlot;
    if (ch.firstBlock == kNoSlot) {
      ch.firstBlock = b;
    } else {
      next[ch.lastBlock] = b;
    }
    ch.lastBlock = b;
  }
  if (b == kNoSlot) return kNoSlot;
  return inlineRows + b * kParamOverflowBlockEntries + ki;
}

template <class TableT>
void SetChainParam(Impl& im, TableT& tb, ParamChain& ch, SlotIndex owner, bool playbackScope, Id id, ParamKind kind,
                   float target, float rampRate, Id*, SlotIndex* ownerCol, ParamKind* kindCol, float* targetCol,
                   float* rampCol, SlotIndex* nextCol, Id* idCol) noexcept {
  // Existing row of the chain?
  SlotIndex s = ch.count != 0u ? owner * kParamInlineSlots : kNoSlot;
  for (std::uint32_t k = 0; k < ch.count && s != kNoSlot; ++k) {
    const std::uint32_t d = base::rows::DenseOf(tb.rows, s);
    if (d == kNoSlot) break;
    if (idCol[d] == id) {
      targetCol[d] = target;
      kindCol[d] = kind;
      rampCol[d] = rampRate;
      return;
    }
    s = nextCol[d];
  }
  const SlotIndex slot = ChainSlot(im, ch, owner, ch.count, playbackScope);
  if (slot == kNoSlot) {
    PostWarning(im, SND_ERR_LIMIT_REACHED, 0u, id);
    return;
  }
  const std::uint32_t d = base::rows::InsertNext(tb.rows, slot);
  if (d == kNoSlot) return;
  idCol[d] = id;
  ownerCol[d] = owner;
  kindCol[d] = kind;
  targetCol[d] = target;
  rampCol[d] = rampRate;
  nextCol[d] = kNoSlot;
  if (ch.tail != kNoSlot) nextCol[base::rows::DenseOf(tb.rows, ch.tail)] = slot;
  ch.tail = slot;
  ++ch.count;
}

float RampRateOf(const Impl& im, Id id) noexcept {
  const std::uint32_t d = im.paramDefMap.Find(id);
  return d == IdMap::kNotFound ? 0.0f : im.paramDefs[d].rampRate;
}
float ClampParam(const Impl& im, Id id, float v) noexcept {
  const std::uint32_t d = im.paramDefMap.Find(id);
  if (d == IdMap::kNotFound) return v;
  const ParamDef& pd = im.paramDefs[d];
  if (pd.kind != 0u || !(pd.maxValue > pd.minValue)) return v;
  return v < pd.minValue ? pd.minValue : (v > pd.maxValue ? pd.maxValue : v);
}

void SetParamValue(Impl& im, ParamScope scope, HandleRef target, Id id, ParamKind kind, float value) noexcept {
  schema::Tables& t = im.tables;
  const float rate = kind == ParamKind::Switch ? 0.0f : RampRateOf(im, id);
  if (kind == ParamKind::Continuous) value = ClampParam(im, id, value);
  switch (scope) {
    case ParamScope::Global: {
      schema::TableRows& rows = SND_ROWS(ParamGlobal);
      std::uint32_t slot = im.ap.globalParams.Find(id);
      std::uint32_t d = slot == IdMap::kNotFound ? kNoSlot : base::rows::DenseOf(rows, slot);
      if (d == kNoSlot) {
        slot = im.ap.globalFree.Pop();
        if (slot == base::kInvalidIndex) {
          PostWarning(im, SND_ERR_LIMIT_REACHED, 0u, id);
          return;
        }
        d = base::rows::InsertNext(rows, slot);
        im.ap.globalParams.Set(id, slot);
        SND_W(ParamGlobal_paramId)[d] = id;
        SND_W(ParamGlobal_ownerSlot)[d] = kNoSlot;
        SND_W(ParamGlobal_nextSlot)[d] = kNoSlot;
      }
      SND_W(ParamGlobal_kind)[d] = kind;
      SND_W(ParamGlobal_target)[d] = value;
      SND_W(ParamGlobal_rampRate)[d] = rate;
      return;
    }
    case ParamScope::Emitter: {
      if (!base::rows::IsCurrent(t.Emitter.rows, target)) return;
      SND_ROWS(ParamEmitter);
      SetChainParam(im, t.ParamEmitter, im.ap.emitterChain[target.slot], target.slot, false, id, kind, value, rate,
                    nullptr, SND_W(ParamEmitter_ownerSlot), SND_W(ParamEmitter_kind), SND_W(ParamEmitter_target),
                    SND_W(ParamEmitter_rampRate), SND_W(ParamEmitter_nextSlot), SND_W(ParamEmitter_paramId));
      return;
    }
    case ParamScope::Playback: {
      if (!base::rows::IsCurrent(t.Playback.rows, target)) return;
      SND_ROWS(ParamPlayback);
      SetChainParam(im, t.ParamPlayback, im.ap.playbackChain[target.slot], target.slot, true, id, kind, value, rate,
                    nullptr, SND_W(ParamPlayback_ownerSlot), SND_W(ParamPlayback_kind), SND_W(ParamPlayback_target),
                    SND_W(ParamPlayback_rampRate), SND_W(ParamPlayback_nextSlot), SND_W(ParamPlayback_paramId));
      return;
    }
  }
}

void DestroyEmitterParams(Impl& im, SlotIndex emitter) noexcept {
  schema::Tables& t = im.tables;
  ParamChain& ch = im.ap.emitterChain[emitter];
  SND_ROWS(ParamEmitter);
  SlotIndex s = ch.count != 0u ? emitter * kParamInlineSlots : kNoSlot;
  for (std::uint32_t k = 0; k < ch.count && s != kNoSlot; ++k) {
    const std::uint32_t d = base::rows::DenseOf(t.ParamEmitter.rows, s);
    if (d == kNoSlot) break;
    const SlotIndex next = SND_W(ParamEmitter_nextSlot)[d];
    RemoveParamEmitterRow(t, s);
    s = next;
  }
  ResetChain(im, ch, false);
}

void DestroyEmitter(Impl& im, SlotIndex slot) noexcept {
  schema::Tables& t = im.tables;
  if (!base::rows::IsLive(SND_ROWS(Emitter), slot)) return;
  const std::uint32_t gen = t.Emitter.rows.generation[slot];
  DestroyEmitterParams(im, slot);
  RemoveEmitterRow(t, slot);
  im.ap.implicitOwnerGen[slot] = 0u;
  if (im.releasedEmitterCount < im.cfg.maxEmitters) im.releasedEmitters[im.releasedEmitterCount++] = Released{slot, gen};
}

SlotIndex CreateEmitterRow(Impl& im, HandleRef h) noexcept {
  schema::Tables& t = im.tables;
  const std::uint32_t d = base::rows::Insert(SND_ROWS(Emitter), h.slot, h.generation);
  if (d == kNoSlot) return kNoSlot;
  SND_W(Emitter_position)[d] = Vec3{0, 0, 0};
  SND_W(Emitter_velocity)[d] = Vec3{0, 0, 0};
  SND_W(Emitter_forward)[d] = Vec3{0, 0, 1};
  SND_W(Emitter_lastUpdateBlock)[d] = im.block;
  SND_W(Emitter_createdBlock)[d] = im.block;
  if (im.ap.emitterChain[h.slot].count != 0u || im.ap.emitterChain[h.slot].firstBlock != kNoSlot) {
    ResetChain(im, im.ap.emitterChain[h.slot], false);
  }
  return h.slot;
}

// Creates the Playback row of a play-family command (4 step 1-2).
void CreatePlayback(Impl& im, HandleRef h, HandleRef emitter, Id event, EventRef ref, const snd_when& whenIn,
                    const Vec3* position, std::uint32_t flags, DynamicSourceRef dyn) noexcept {
  schema::Tables& t = im.tables;
  if (h.slot >= im.cfg.maxPlaybacks) return;
  schema::TableRows& rows = SND_ROWS(Playback);
  const std::uint32_t d = base::rows::Insert(rows, h.slot, h.generation);
  if (d == kNoSlot) return;  // slot already live: the game side never issues that
  if ((flags & cmd::kPlayOneShot) != 0u) ref.flags |= kEventRefOneShot;
  // Emitter: the given one if current, an implicit one for PlayAt (released with the playback).
  SlotIndex es = kNoSlot;
  if ((flags & cmd::kPlayHasPosition) != 0u && position != nullptr) {
    const HandleRef eh = im.emitterSlots.Acquire();
    if (eh.generation != 0u && CreateEmitterRow(im, eh) != kNoSlot) {
      const std::uint32_t ed = base::rows::DenseOf(t.Emitter.rows, eh.slot);
      SND_W(Emitter_position)[ed] = *position;
      es = eh.slot;
      im.ap.implicitEmitter[h.slot] = es;
      im.ap.implicitOwnerGen[es] = h.generation;
    } else if (eh.generation != 0u) {
      im.emitterSlots.Release(eh);
    }
  } else if (emitter.generation != 0u && base::rows::IsCurrent(t.Emitter.rows, emitter)) {
    es = emitter.slot;
  }
  // Music events own a transport (8.1, D-05).
  SlotIndex ts = kNoSlot;
  if ((ref.flags & kEventRefMusic) != 0u) {
    const HandleRef th = im.transportSlots.Acquire();
    if (th.generation != 0u) {
      const std::uint32_t td = base::rows::Insert(SND_ROWS(Transport), th.slot, th.generation);
      if (td != kNoSlot) {
        SND_W(Transport_ownerPlayback)[td] = h.slot;
        SND_W(Transport_lifetime)[td] = TransportLifetime{0u, 1u};
        SND_W(Transport_initial)[td] = TransportInit{120.0f, TimeSignature{4, 4}, 1u, 0u};
        ts = th.slot;
      } else {
        im.transportSlots.Release(th);
      }
    }
  }
  SND_W(Playback_eventRef)[d] = ref;
  im.ap.playCounter = im.ap.playCounter + 1u;
  SND_W(Playback_seed)[d] = base::MixU32(im.cfg.seed ^ (im.ap.playCounter * 0x9E3779B9u));
  SND_W(Playback_emitterSlot)[d] = es;
  SND_W(Playback_transportSlot)[d] = ts;
  SND_W(Playback_eventId)[d] = event;
  SND_W(Playback_dynamicSource)[d] = dyn;
  PlaybackRequest req{0u, 0u, -1.0f, 0.0f, kNoSlot, kNoSlot};
  snd_when w = whenIn;
  SlotIndex transport = kNoSlot;
  if (ScheduledWhen(im, w, &transport, h.Value())) {
    const SlotIndex rs = NewReservation(im, transport, w, ReservationAction::Play, h, ReservationPayload{});
    if (rs == kNoSlot) {
      req.flags |= kRequestRejectReservationFull;
    } else {
      req.startReservation = rs;
    }
  }
  SND_W(Playback_request)[d] = req;
  ResetChain(im, im.ap.playbackChain[h.slot], true);
}

PlaybackRequest* RequestOf(Impl& im, HandleRef h) noexcept {
  schema::Tables& t = im.tables;
  const std::uint32_t d = base::rows::DenseOf(t.Playback.rows, h);
  return d == kNoSlot ? nullptr : &SND_W(Playback_request)[d];
}

void RequestStop(Impl& im, HandleRef h, float fade) noexcept {
  PlaybackRequest* r = RequestOf(im, h);
  if (r == nullptr) return;
  r->flags |= kRequestStop;
  r->stopFadeSeconds = fade;
  ++r->seq;
}

void ApplyTimed(Impl& im, HandleRef transportHandle, const snd_when& when, ReservationAction action,
                const ReservationPayload& payload) noexcept {
  const SlotIndex ts = TransportSlotOf(im, transportHandle);
  if (ts == kNoSlot) return;  // stale handle: nothing happens (D-29)
  // Transport changes always go through the reservation table, "now" included (D-18).
  if (NewReservation(im, ts, when, action, transportHandle, payload) == kNoSlot) {
    PostWarning(im, SND_ERR_LIMIT_REACHED, SND_END_DETAIL_RESERVATION_FULL, transportHandle.Value());
  }
}

void AddMusicInput(Impl& im, music::MusicInputKind kind, HandleRef playback, Id state, std::uint32_t whenOverride) noexcept {
  schema::Tables& t = im.tables;
  const std::uint32_t d = base::rows::DenseOf(t.Playback.rows, playback);
  if (d == kNoSlot) return;
  EventPriv& ev = im.ev;
  if (ev.musicInputCount >= ev.musicInputCap) return;
  music::MusicInput& in = ev.musicInputs[ev.musicInputCount++];
  in = music::MusicInput{};
  in.kind = kind;
  in.playbackSlot = playback.slot;
  in.transportSlot = SND_W(Playback_transportSlot)[d];
  in.event = SND_W(Playback_eventRef)[d];
  in.stateId = state;
  in.voiceSlot = kNoSlot;
  in.clock = im.clock;
  in.whenOverride = whenOverride;
}

void SetBankUnloading(Impl& im, HandleRef bank, std::uint32_t mode) noexcept {
  if (bank.slot >= im.cfg.maxBanks) return;
  BankAudio& b = im.ap.banks[bank.slot];
  if (mode == kCancelUnloadWhenUnused) {  // the language came back before the bank was unused
    if (b.state == BankAudioState::Unloading && b.unloadMode == kUnloadWhenUnused && b.generation == bank.generation) {
      b.state = BankAudioState::Registered;
    }
    if (b.pendingUnload != 0u && b.pendingGeneration == bank.generation) b.pendingUnload = 0u;
    return;
  }
  if (b.state == BankAudioState::Unloading && b.unloadMode == kUnloadWhenUnused && b.generation == bank.generation) {
    b.unloadMode = mode;  // an explicit unload replaces the language drain
    b.discardSent = 0;
    return;
  }
  if (b.state == BankAudioState::Registered && b.generation == bank.generation) {
    b.state = BankAudioState::Unloading;
    b.unloadMode = mode;
    b.discardSent = 0;
  } else if (b.state == BankAudioState::Free) {
    b.pendingUnload = 1u;
    b.pendingMode = mode;
    b.pendingGeneration = bank.generation;
  }
}

template <class P>
P Load(const std::uint8_t* p) noexcept {
  P v;
  std::memcpy(&v, p, sizeof(P));
  return v;
}

void ApplyOne(Impl& im, const cmd::CommandHeader& h, const std::uint8_t* p) noexcept {
  schema::Tables& t = im.tables;
  using CT = cmd::CommandType;
  switch (static_cast<CT>(h.type)) {
    case CT::Suspend:
      im.ap.suspended = 1u;
      break;
    case CT::Resume:
      im.ap.suspended = 0u;
      break;
    case CT::BankRegister:
      ApplyBankRegister(im, Load<cmd::BankRegister>(p));
      break;
    case CT::BankUnload: {
      const auto c = Load<cmd::BankUnload>(p);
      SetBankUnloading(im, c.bank, c.mode);
      break;
    }
    case CT::SetLanguage:
      im.ap.language = Load<cmd::SetLanguage>(p).language;
      break;
    case CT::DeclareLanguage:
      break;  // the name table lives on the game side (D-50)
    case CT::Play: {
      const auto c = Load<cmd::Play>(p);
      CreatePlayback(im, c.playback, c.emitter, c.event, ResolveEvent(im, c.event), c.when, &c.position, c.flags,
                     DynamicSourceRef{SourceKind::Count, {0, 0, 0}, 0u});
      break;
    }
    case CT::LiveAudition: {
      const auto c = Load<cmd::LiveAudition>(p);
      CreatePlayback(im, c.playback, HandleRef{}, c.event, ResolveEvent(im, c.event), snd_when{}, &c.position,
                     c.flags | cmd::kPlayOneShot, DynamicSourceRef{SourceKind::Count, {0, 0, 0}, 0u});
      break;
    }
    case CT::PlaySynth: {
      const auto c = Load<cmd::PlaySynth>(p);
      if (c.playback.slot >= im.cfg.maxPlaybacks) break;
      DynamicDesc& dd = im.ap.dynamic[c.playback.slot];
      dd = DynamicDesc{};
      dd.synth = c.desc;
      dd.bus = c.desc.bus;
      dd.kind = static_cast<std::uint32_t>(SourceKind::Callback);
      CreatePlayback(im, c.playback, c.emitter, c.desc.event_template, EventRef{kNoBank, kEventRefDynamic, 0u}, c.when,
                     nullptr, 0u, DynamicSourceRef{SourceKind::Callback, {0, 0, 0}, c.playback.slot});
      break;
    }
    case CT::PlayUserBuffer: {
      const auto c = Load<cmd::PlayUserBuffer>(p);
      if (c.playback.slot >= im.cfg.maxPlaybacks) break;
      DynamicDesc& dd = im.ap.dynamic[c.playback.slot];
      dd = DynamicDesc{};
      dd.userBuffer = c.buffer;
      dd.bus = c.bus;
      dd.kind = static_cast<std::uint32_t>(SourceKind::UserBuffer);
      if (c.buffer.slot < im.cfg.maxUserBuffers) ++im.ap.userBufferRefs[c.buffer.slot];
      CreatePlayback(im, c.playback, c.emitter, 0u, EventRef{kNoBank, kEventRefDynamic, 0u}, c.when, nullptr, 0u,
                     DynamicSourceRef{SourceKind::UserBuffer, {0, 0, 0}, c.playback.slot});
      break;
    }
    case CT::PlayInput: {
      const auto c = Load<cmd::PlayInput>(p);
      if (c.playback.slot >= im.cfg.maxPlaybacks) break;
      DynamicDesc& dd = im.ap.dynamic[c.playback.slot];
      dd = DynamicDesc{};
      dd.input = c.desc;
      dd.bus = c.desc.bus;
      dd.kind = static_cast<std::uint32_t>(SourceKind::Input);
      CreatePlayback(im, c.playback, c.emitter, 0u, EventRef{kNoBank, kEventRefDynamic, 0u}, c.when, nullptr, 0u,
                     DynamicSourceRef{SourceKind::Input, {0, 0, 0}, c.playback.slot});
      break;
    }
    case CT::Stop: {
      const auto c = Load<cmd::Stop>(p);
      if (!base::rows::IsCurrent(t.Playback.rows, c.playback)) break;
      snd_when w = c.when;
      SlotIndex ts = kNoSlot;
      if (ScheduledWhen(im, w, &ts, c.playback.Value())) {
        if (NewReservation(im, ts, w, ReservationAction::Stop, c.playback, PayloadOf(&c.fadeSeconds, 4)) == kNoSlot) {
          RequestStop(im, c.playback, c.fadeSeconds);  // table full: stop now rather than never
        }
      } else {
        RequestStop(im, c.playback, c.fadeSeconds);
      }
      break;
    }
    case CT::PauseResume: {
      const auto c = Load<cmd::PauseResume>(p);
      PlaybackRequest* r = RequestOf(im, c.playback);
      if (r == nullptr) break;
      if (c.pause != 0u) {
        r->flags = (r->flags & ~kRequestResume) | kRequestPause;
      } else {
        r->flags = (r->flags & ~kRequestPause) | kRequestResume;
      }
      r->pauseFadeSeconds = c.fadeSeconds;
      ++r->seq;
      break;
    }
    case CT::SetScheduledStop: {
      const auto c = Load<cmd::SetScheduledStop>(p);
      PlaybackRequest* r = RequestOf(im, c.playback);
      if (r == nullptr) break;
      snd_when w = c.when;
      SlotIndex ts = kNoSlot;
      if (!ScheduledWhen(im, w, &ts, c.playback.Value())) {
        RequestStop(im, c.playback, c.fadeSeconds);
        break;
      }
      const SlotIndex rs =
          NewReservation(im, ts, w, ReservationAction::ScheduledStop, c.playback, PayloadOf(&c.fadeSeconds, 4));
      if (rs == kNoSlot) {
        PostWarning(im, SND_ERR_LIMIT_REACHED, SND_END_DETAIL_RESERVATION_FULL, c.playback.Value());
        break;
      }
      r = RequestOf(im, c.playback);
      r->flags |= kRequestScheduledStop;
      r->stopReservation = rs;
      r->stopFadeSeconds = c.fadeSeconds;
      ++r->seq;
      break;
    }
    case CT::CreateEmitter:
      CreateEmitterRow(im, Load<cmd::CreateEmitter>(p).emitter);
      break;
    case CT::DestroyEmitter: {
      const auto c = Load<cmd::DestroyEmitter>(p);
      if (base::rows::IsCurrent(t.Emitter.rows, c.emitter)) DestroyEmitter(im, c.emitter.slot);
      break;
    }
    case CT::SetTransform: {
      const auto c = Load<cmd::SetTransform>(p);
      const std::uint32_t d = base::rows::DenseOf(t.Emitter.rows, c.emitter);
      if (d == kNoSlot) break;
      if ((c.mask & cmd::kTransformPosition) != 0u) SND_W(Emitter_position)[d] = c.position;
      if ((c.mask & cmd::kTransformForward) != 0u) SND_W(Emitter_forward)[d] = c.forward;
      if ((c.mask & cmd::kTransformVelocity) != 0u) SND_W(Emitter_velocity)[d] = c.velocity;
      SND_W(Emitter_lastUpdateBlock)[d] = im.block;
      break;
    }
    case CT::SetListener: {
      const auto c = Load<cmd::SetListener>(p);
      if (c.index >= im.cfg.maxListeners) break;
      schema::TableRows& rows = SND_ROWS(Listener);
      if (!base::rows::IsLive(rows, c.index)) base::rows::InsertNext(rows, c.index);
      SND_W(Listener_position)[c.index] = c.listener.position;
      SND_W(Listener_velocity)[c.index] = c.listener.velocity;
      SND_W(Listener_forward)[c.index] = c.listener.forward;
      SND_W(Listener_up)[c.index] = c.listener.up;
      SND_W(Listener_weight)[c.index] = c.listener.weight;
      SND_W(Listener_enabled)[c.index] = c.listener.enabled;
      break;
    }
    case CT::SetParam: {
      const auto c = Load<cmd::SetParam>(p);
      SetParamValue(im, static_cast<ParamScope>(c.scope), c.target, c.param, ParamKind::Continuous, c.value);
      break;
    }
    case CT::SetSwitch: {
      const auto c = Load<cmd::SetSwitch>(p);
      SetParamValue(im, static_cast<ParamScope>(c.scope), c.target, c.group, ParamKind::Switch,
                    std::bit_cast<float>(c.value));
      break;
    }
    case CT::SetBusVolume: {
      const auto c = Load<cmd::SetBusVolume>(p);
      const std::uint32_t slot = im.busMap.Find(c.bus == 0u && im.rd.masterSlot != kNoSlot ? im.busDefs[im.rd.masterSlot].id : c.bus);
      const std::uint32_t d = slot == IdMap::kNotFound ? kNoSlot : base::rows::DenseOf(t.Bus.rows, slot);
      if (d == kNoSlot) {
        PostWarning(im, SND_ERR_NOT_FOUND, 0u, c.bus);
        break;
      }
      TimedRamp& r = SND_W(Bus_volume)[d];
      const float from = RampValue(r, im.clock);
      r = TimedRamp{from, c.volume, im.clock, static_cast<std::uint32_t>(c.fadeSeconds * im.cfg.sampleRate), 0u};
      break;
    }
    case CT::ApplySnapshot: {
      const auto c = Load<cmd::ApplySnapshot>(p);
      const std::uint32_t slot = im.snapMap.Find(c.snapshot);
      const std::uint32_t d = slot == IdMap::kNotFound ? kNoSlot : base::rows::DenseOf(t.Snapshot.rows, slot);
      if (d == kNoSlot) {
        PostWarning(im, SND_ERR_NOT_FOUND, 0u, c.snapshot);
        break;
      }
      TimedRamp& r = SND_W(Snapshot_request)[d].weight;
      const float from = RampValue(r, im.clock);
      r = TimedRamp{from, c.weight, im.clock, static_cast<std::uint32_t>(c.timeSeconds * im.cfg.sampleRate), 0u};
      break;
    }
    case CT::SetCategoryVolume:
    case CT::PauseCategory:
    case CT::MuteCategory: {
      Id id = 0;
      if (static_cast<CT>(h.type) == CT::SetCategoryVolume) id = Load<cmd::SetCategoryVolume>(p).category;
      if (static_cast<CT>(h.type) == CT::PauseCategory) id = Load<cmd::PauseCategory>(p).category;
      if (static_cast<CT>(h.type) == CT::MuteCategory) id = Load<cmd::MuteCategory>(p).category;
      const std::uint32_t slot = id == 0u ? im.globalCategory : im.catMap.Find(id);
      const std::uint32_t d = slot == IdMap::kNotFound ? kNoSlot : base::rows::DenseOf(t.Category.rows, slot);
      if (d == kNoSlot) {
        PostWarning(im, SND_ERR_NOT_FOUND, 0u, id);
        break;
      }
      CategoryRequest& r = SND_W(Category_request)[d];
      if (static_cast<CT>(h.type) == CT::SetCategoryVolume) {
        const auto c = Load<cmd::SetCategoryVolume>(p);
        r.volume = TimedRamp{RampValue(r.volume, im.clock), c.volume, im.clock,
                             static_cast<std::uint32_t>(c.fadeSeconds * im.cfg.sampleRate), 0u};
      } else if (static_cast<CT>(h.type) == CT::PauseCategory) {
        r.paused = Load<cmd::PauseCategory>(p).paused;
      } else {
        r.muted = Load<cmd::MuteCategory>(p).muted;
      }
      break;
    }
    case CT::SetMusicState: {
      const auto c = Load<cmd::SetMusicState>(p);
      if (!base::rows::IsCurrent(t.Playback.rows, c.playback)) break;
      snd_when w = c.when;
      SlotIndex ts = kNoSlot;
      if (ScheduledWhen(im, w, &ts, c.playback.Value())) {
        if (NewReservation(im, ts, w, ReservationAction::SetMusicState, c.playback, PayloadOf(&c.state, 4)) == kNoSlot) {
          PostWarning(im, SND_ERR_LIMIT_REACHED, SND_END_DETAIL_RESERVATION_FULL, c.playback.Value());
        }
      } else {
        AddMusicInput(im, music::MusicInputKind::SetState, c.playback, c.state, 0u);
      }
      break;
    }
    case CT::PrepareMusicState: {
      const auto c = Load<cmd::PrepareMusicState>(p);
      AddMusicInput(im, music::MusicInputKind::PrepareState, c.playback, c.state, 0u);
      break;
    }
    case CT::CreateTransport: {
      const auto c = Load<cmd::CreateTransport>(p);
      const std::uint32_t d = base::rows::Insert(SND_ROWS(Transport), c.transport.slot, c.transport.generation);
      if (d == kNoSlot) break;
      SND_W(Transport_ownerPlayback)[d] = kNoSlot;
      SND_W(Transport_lifetime)[d] = TransportLifetime{0u, 0u};
      SND_W(Transport_initial)[d] =
          TransportInit{c.desc.bpm, TimeSignature{c.desc.beats_per_bar, c.desc.beat_unit}, c.desc.start_playing, 0u};
      break;
    }
    case CT::DestroyTransport: {
      const auto c = Load<cmd::DestroyTransport>(p);
      const std::uint32_t d = base::rows::DenseOf(t.Transport.rows, c.transport);
      if (d != kNoSlot) SND_W(Transport_lifetime)[d].destroyRequested = 1u;
      break;
    }
    case CT::SetTransportState: {
      const auto c = Load<cmd::SetTransportState>(p);
      ApplyTimed(im, c.transport, c.when, ReservationAction::SetTransportState, PayloadOf(&c.state, 4));
      break;
    }
    case CT::SetTempo: {
      const auto c = Load<cmd::SetTempo>(p);
      ApplyTimed(im, c.transport, c.when, ReservationAction::SetTempo, PayloadOf(&c.bpm, 4));
      break;
    }
    case CT::SeekTransport: {
      const auto c = Load<cmd::SeekTransport>(p);
      ApplyTimed(im, c.transport, c.when, ReservationAction::Seek, PayloadOf(&c.position, sizeof(c.position)));
      break;
    }
    case CT::CreateUserBuffer: {
      const auto c = Load<cmd::CreateUserBuffer>(p);
      if (c.buffer.slot < im.cfg.maxUserBuffers) im.ap.userBufferDestroy[c.buffer.slot] = 0u;
      break;
    }
    case CT::DestroyUserBuffer: {
      const auto c = Load<cmd::DestroyUserBuffer>(p);
      if (c.buffer.slot < im.cfg.maxUserBuffers &&
          im.userBuffers[c.buffer.slot].generation.load(std::memory_order_acquire) == c.buffer.generation) {
        im.ap.userBufferDestroy[c.buffer.slot] = c.buffer.generation;
      }
      break;
    }
    case CT::LiveSetValue:
    case CT::LiveReplaceChunk:
      if (im.desc.liveCommand != nullptr) im.desc.liveCommand(im.desc.liveUser, static_cast<CT>(h.type), p);
      break;
    case CT::LiveOverride: {
      const auto c = Load<cmd::LiveOverride>(p);
      ApplyPriv& ap = im.ap;
      std::uint32_t at = kNoSlot;
      for (std::uint32_t i = 0; i < ap.overrideCount; ++i) {
        if (ap.overrides[i].used != 0u && ap.overrides[i].kind == c.kind && ap.overrides[i].id == c.id) at = i;
      }
      if (c.enable == 0u) {
        if (at != kNoSlot) ap.overrides[at].used = 0u;
      } else {
        if (at == kNoSlot) {
          for (std::uint32_t i = 0; i < im.desc.limits.maxLiveOverrides; ++i) {
            if (ap.overrides[i].used == 0u) {
              at = i;
              break;
            }
          }
          if (at == kNoSlot) break;
          if (at >= ap.overrideCount) ap.overrideCount = at + 1u;
        }
        ap.overrides[at] = LiveOverride{c.kind, c.id, c.value, 1u};
        if (c.kind == static_cast<std::uint32_t>(cmd::LiveOverrideKind::MusicState)) {
          AddMusicInput(im, music::MusicInputKind::SetState, c.target, c.value, 0u);
        }
      }
      break;
    }
    case CT::LiveClearOverrides:
      for (std::uint32_t i = 0; i < im.ap.overrideCount; ++i) im.ap.overrides[i].used = 0u;
      im.ap.overrideCount = 0;
      break;
    default:
      break;
  }
}

void ApplyFallback(Impl& im, const cmd::OverflowFallback& fb) noexcept {
  schema::Tables& t = im.tables;
  auto forBits = [](const std::uint64_t* words, std::uint32_t capacity, auto&& fn) {
    for (std::uint32_t w = 0, n = base::notify::BitWords(capacity); w < n; ++w) {
      std::uint64_t bits = words[w];
      while (bits != 0u) {
        const std::uint32_t s = w * 64u + static_cast<std::uint32_t>(base::CountTrailingZeros64(bits));
        bits &= bits - 1u;
        if (s < capacity) fn(s);
      }
    }
  };
  forBits(fb.stopPlayback, im.cfg.maxPlaybacks, [&](std::uint32_t s) {
    if (base::rows::IsLive(t.Playback.rows, s)) RequestStop(im, HandleRef{s, t.Playback.rows.generation[s]}, -1.0f);
  });
  forBits(fb.destroyEmitter, im.cfg.maxEmitters, [&](std::uint32_t s) { DestroyEmitter(im, s); });
  forBits(fb.destroyTransport, im.cfg.maxTransports, [&](std::uint32_t s) {
    const std::uint32_t d = base::rows::DenseOf(t.Transport.rows, s);
    if (d != kNoSlot && SND_W(Transport_lifetime)[d].createdFromPlayback == 0u) {
      SND_W(Transport_lifetime)[d].destroyRequested = 1u;
    }
  });
  forBits(fb.destroyUserBuffer, im.cfg.maxUserBuffers, [&](std::uint32_t s) {
    const std::uint32_t g = im.userBuffers[s].generation.load(std::memory_order_acquire);
    if (g != 0u) im.ap.userBufferDestroy[s] = g;
  });
  forBits(fb.unloadBank, im.cfg.maxBanks, [&](std::uint32_t s) {
    if (im.ap.banks[s].state == BankAudioState::Registered) {
      SetBankUnloading(im, HandleRef{s, im.ap.banks[s].generation}, SND_UNLOAD_STOP_AND_UNLOAD);
    }
  });
  for (std::uint32_t i = 0; i < fb.paramCapacity; ++i) {
    const cmd::OverflowParam& op = fb.params[i];
    if (op.used == 0u) continue;
    SetParamValue(im, static_cast<ParamScope>(op.scope), op.target, op.id,
                  op.isSwitch != 0u ? ParamKind::Switch : ParamKind::Continuous, std::bit_cast<float>(op.value));
  }
}

// Unloading banks (10.3): wait for the referencing playbacks, unregister, then the epoch.
void ProcessBanks(Impl& im) noexcept {
  schema::Tables& t = im.tables;
  for (std::uint32_t s = 0; s < im.cfg.maxBanks; ++s) {
    BankAudio& b = im.ap.banks[s];
    if (b.state == BankAudioState::Unloading) {
      const schema::TableRows& rows = t.Playback.rows;
      const EventRef* refs = SND_W(Playback_eventRef);
      bool referenced = false;
      for (std::uint32_t d = 0; d < rows.count; ++d) {
        if (refs[d].bankSlot == s) {
          referenced = true;
          break;
        }
      }
      if (b.unloadMode == SND_UNLOAD_IF_UNUSED && referenced) {
        b.state = BankAudioState::Registered;  // refused: back to LOADED on the game side
        if (!im.bankEvents.TryPush(BankEvent{BankEventKind::UnloadRefused, s, b.generation, 0u})) b.state = BankAudioState::Unloading;
        continue;
      }
      if (referenced) {
        if (b.unloadMode == SND_UNLOAD_STOP_AND_UNLOAD && b.discardSent == 0u) {
          PlaybackRequest* req = SND_W(Playback_request);
          for (std::uint32_t d = 0; d < rows.count; ++d) {
            if (refs[d].bankSlot == s) {
              req[d].flags |= kRequestDiscard;
              ++req[d].seq;
            }
          }
          b.discardSent = 1u;
        }
        continue;
      }
      if (b.unloadMode == kUnloadWhenUnused) {
        // Tell the game side first (bank manager: UNLOADING); retry next block if the ring is full.
        if (!im.bankEvents.TryPush(BankEvent{BankEventKind::Unregistered, s, b.generation, 0u})) continue;
      }
      UnregisterBank(im, s);
    } else if (b.state == BankAudioState::AwaitEpoch) {
      // Epoch (3.3): a block boundary has passed and no RT worker runs a chunk of
      // a block that could still reference the bank (D-43 SAFE POINT).
      if (im.block > b.unregisterBlock && im.blockQuiescent) {
        if (im.bankEvents.TryPush(BankEvent{BankEventKind::Release, s, b.generation, 0u})) b = BankAudio{};
      }
    }
  }
}

// Housekeeping of rows whose owner went away last block.
void ReleaseOwnedRows(Impl& im) noexcept {
  schema::Tables& t = im.tables;
  // Playbacks that ended in the previous block (RunEventLogic's list).
  for (std::uint32_t i = 0; i < im.endedCount; ++i) {
    const EndedPlayback& e = im.ended[i];
    const SlotIndex es = im.ap.implicitEmitter[e.slot];
    if (es != kNoSlot) {
      im.ap.implicitEmitter[e.slot] = kNoSlot;
      if (im.ap.implicitOwnerGen[es] == e.generation) DestroyEmitter(im, es);
    }
    const DynamicDesc& dd = im.ap.dynamic[e.slot];
    if (dd.kind == static_cast<std::uint32_t>(SourceKind::UserBuffer) && dd.userBuffer.slot < im.cfg.maxUserBuffers) {
      std::uint32_t& refs = im.ap.userBufferRefs[dd.userBuffer.slot];
      if (refs > 0u) --refs;
      im.ap.dynamic[e.slot].kind = static_cast<std::uint32_t>(SourceKind::Count);
    }
  }
  (void)t;
  // User buffers whose destroy was requested and that no playback uses (5.1).
  for (std::uint32_t s = 0; s < im.cfg.maxUserBuffers; ++s) {
    const std::uint32_t g = im.ap.userBufferDestroy[s];
    if (g == 0u || im.ap.userBufferRefs[s] != 0u) continue;
    im.ap.userBufferDestroy[s] = 0u;
    if (im.releasedUserBufferCount < im.cfg.maxUserBuffers) {
      im.releasedUserBuffers[im.releasedUserBufferCount++] = Released{s, g};
    }
  }
}

// Work RunEventLogic deferred to this block (action tracks, music PlayEvent).
void ApplyDeferred(Impl& im) noexcept {
  schema::Tables& t = im.tables;
  for (std::uint32_t i = 0; i < im.ev.deferredCount; ++i) {
    const Deferred& df = im.ev.deferred[i];
    switch (df.kind) {
      case DeferredKind::Play: {
        HandleRef em{};
        if (df.emitter != kNoSlot && base::rows::IsLive(t.Emitter.rows, df.emitter) &&
            t.Emitter.rows.generation[df.emitter] == df.emitterGen) {
          em = HandleRef{df.emitter, df.emitterGen};
        }
        snd_when now{};
        if (df.startClock > im.clock) {  // music PlayEvent (stinger): sample accurate start
          now.kind = static_cast<std::uint8_t>(SND_WHEN_CLOCK_SAMPLE);
          now.value = df.startClock;
        }
        CreatePlayback(im, df.handle, em, df.id, df.ref, now, nullptr, cmd::kPlayOneShot,
                       DynamicSourceRef{SourceKind::Count, {0, 0, 0}, 0u});
        break;
      }
      case DeferredKind::StopEvent: {
        const schema::TableRows& rows = t.Playback.rows;
        for (std::uint32_t d = 0; d < rows.count; ++d) {
          if (SND_W(Playback_eventId)[d] == df.id) {
            const SlotIndex s = base::rows::SlotAt(rows, d);
            RequestStop(im, HandleRef{s, rows.generation[s]}, -1.0f);
          }
        }
        break;
      }
      case DeferredKind::SetParam:
        SetParamValue(im, ParamScope::Global, HandleRef{}, df.id, ParamKind::Continuous, df.value);
        break;
      case DeferredKind::ApplySnapshot: {
        const std::uint32_t slot = im.snapMap.Find(df.id);
        const std::uint32_t d = slot == IdMap::kNotFound ? kNoSlot : base::rows::DenseOf(t.Snapshot.rows, slot);
        if (d != kNoSlot) SND_W(Snapshot_request)[d].weight = TimedRamp{df.value, df.value, im.clock, 0u, 0u};
        break;
      }
    }
  }
  im.ev.deferredCount = 0;
}

}  // namespace

void ApplyCommands(Impl& im) noexcept {
  im.releasedEmitterCount = 0;
  im.releasedUserBufferCount = 0;
  // Music inputs are reset after RunMusicLogic consumed them (sys_event.cpp RunMusic).
  ReleaseOwnedRows(im);
  ApplyDeferred(im);
  for (std::uint32_t pi = 0; pi < im.cfg.maxThreads; ++pi) {
    Producer& pr = im.producers[pi];
    std::uint32_t idx = 0;
    while (pr.cycle.Take(idx)) {
      ArenaView& av = pr.arenas[idx];
      cmd::ArenaHeader& h = *av.header;
      h.appliedBlock = im.block;  // D-12
      auto apply = [&](const cmd::CommandHeader& ch, const std::uint8_t* payload) { ApplyOne(im, ch, payload); };
      cmd::ForEachCommand(av.normal, av.normal + h.normalBytes, apply);
      cmd::ForEachCommand(av.reserve, av.reserve + h.reserveBytes, apply);
      if ((h.flags & cmd::kArenaHasFallback) != 0u) ApplyFallback(im, av.fallback);
      pr.cycle.Return(idx);
    }
  }
  ProcessBanks(im);
}

}  // namespace snd::core
