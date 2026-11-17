// Private state of the music system (everything lives in the pool core hands
// to Init; nothing is allocated afterwards) and the helpers shared by the
// UpdateTransport and music-logic halves.
#ifndef SND_MUSIC_SRC_MUSIC_STATE_HPP
#define SND_MUSIC_SRC_MUSIC_STATE_HPP

#include <cstdint>

#include "bank_tables.hpp"
#include "playlist.hpp"
#include "rules.hpp"
#include "segment.hpp"
#include "snd/internal/music_if.hpp"
#include "snd/music/music_system.hpp"
#include "timeline.hpp"

namespace snd::music::detail {

inline constexpr std::uint32_t kMaxInstances = 6;
inline constexpr std::uint32_t kMaxInstanceVoices = 5;  // main material + 4 separate-file stems
inline constexpr std::uint32_t kMaxLocalVoices = 20;
inline constexpr std::uint32_t kMaxLastPositions = 8;
inline constexpr std::uint32_t kMaxPendingStingers = 4;
inline constexpr std::uint32_t kNotifyCapacity = 512;
inline constexpr std::uint32_t kLogCapacity = 64;
inline constexpr std::uint16_t kNoPb = 0xFFFFu;
inline constexpr std::uint8_t kNoLate = 0xFFu;
// core's FireEntry::lateRuleApplied for "the transport is gone" (core_impl.hpp
// kFireTransportGone; snd/core/core.hpp kFireTransportGone).
inline constexpr std::uint8_t kLateTransportGone = 0xFEu;
// How far ahead of a segment end the playlist picks the next segment.
inline constexpr double kPlanAheadSeconds = 0.5;

enum class Role : std::uint8_t { Free = 0, Main = 1, Bridge = 2 };
enum class InstPhase : std::uint8_t { Planned = 0, Issued = 1 };

// A scheduled or sounding segment (main material + separate-file stems).
struct Instance {
  Role role;
  InstPhase phase;
  std::uint8_t successor;    // the next segment of the playlist is planned
  std::uint8_t stopPending;  // stop at stopClock (fade stopFade)
  std::uint8_t voiceCount;
  std::uint8_t voices[kMaxInstanceVoices];
  std::uint32_t seg;         // MSEG row
  Id segId;                  // MSEG id
  std::uint32_t stateRow;    // MSTA row it belongs to (kNone: bridge)
  std::uint32_t serial;
  MusicalTicks startTick;    // transport tick of its bar 0
  MusicalTicks lenTicks;
  MusicalTicks anchorTick;   // first transport tick at which its audio must match
  float fadeIn;              // seconds
  float stopFade;            // seconds
  ClockSample stopClock;
  ClockSample voiceClock;    // first audible sample of the main voice (issued)
  std::uint64_t voiceFrame;  // material frame at voiceClock
  ClockSample markerCursor;  // markers before this clock were notified
  double ratio;              // output samples per material frame
  MusicalTicks EndTick() const noexcept { return startTick + lenTicks; }
};

enum class VoiceStatus : std::uint8_t { Free = 0, Requested = 1, Live = 2 };

struct LocalVoice {
  VoiceStatus status;
  std::uint8_t inst;
  std::uint8_t stopSent;
  std::uint8_t reserved;
  std::uint32_t tag;
  SlotIndex slot;
  std::uint32_t stem;   // MSTM row of a separate-file stem, kNone for the main material
  std::uint32_t asset;  // ASST row
  BlockIndex createdBlock;
  float gain;           // last gain sent (StartClip / SetGain)
  float reserved2;
};

struct LastPosition {
  Id state;
  std::uint32_t seg;
  MusicalTicks offset;  // ticks into `seg` when the state was left
  Cursor cursor;
};

enum class Phase : std::uint8_t { Idle = 0, Pending = 1, Transitioning = 2 };

struct MusicPb {
  std::uint32_t used;
  SlotIndex playback;
  std::uint32_t generation;  // Playback row generation
  std::uint16_t bank;
  std::uint8_t started;
  std::uint8_t stopping;
  std::uint32_t eventRow;
  SlotIndex transport;
  std::uint32_t rng;
  std::uint8_t paused;
  std::uint8_t soundPriority;  // sync group stall policy (8.4)
  std::uint8_t hasQueued;
  std::uint8_t reserved;
  std::uint32_t syncGroup;
  // State and playlist.
  std::uint32_t stateRow;
  Id stateId;
  Cursor cursor;
  std::int32_t head;  // instance the playlist continues from, -1 none
  Instance inst[kMaxInstances];
  LocalVoice voice[kMaxLocalVoices];
  // Transition state machine (8.6).
  Phase phase;
  Rule pendRule;
  Id pendTarget;
  std::uint32_t pendTargetRow;
  double pendTick;
  std::uint32_t pendPrime;  // prime frames of the target's first material
  std::uint8_t pendLate;  // late rule applied to the pending transition (kNoLate none)
  std::uint32_t pendDeferrals;
  ClockSample transEnd;
  Interrupt transInterrupt;
  Id queuedTarget;
  Id desired;   // SetMusicState before the playback started
  Id prepared;  // PrepareMusicState (no priming interface yet, see proposals)
  LastPosition last[kMaxLastPositions];
  std::uint32_t lastNext;
  Id stingers[kMaxPendingStingers];
  std::uint32_t stingerCount;
  std::uint8_t seekPending;  // the owned transport was seeked (UpdateTransport -> music logic)
  ClockSample seekClock;
  std::uint32_t serial;
  std::uint32_t transitions;
  std::uint32_t deferrals;
  std::uint32_t cancels;
};

// Per Reservation slot: the resolved musical target (8.2: kept in music time).
struct ResState {
  std::uint64_t seq;       // Reservation.sequence this state belongs to (0 = none)
  double tick;             // musical target (musical kinds)
  ClockSample clock;       // clock target (clock kinds)
  ClockSample pickup;      // PICKUP_ALIGN lead in samples
  std::uint32_t deferrals;
  std::uint8_t isClock;
  std::uint8_t lateRule;
  std::uint8_t late;       // late rule applied (kNoLate none)
  std::uint8_t cancel;     // fire as "cancel"
};

struct State {
  MusicLimits limits{};
  MusicServices services{};
  double rate = 48000.0;
  Timeline* timelines = nullptr;   // [maxTransports] by transport slot
  MusicPb* pbs = nullptr;          // [pbCount]
  std::uint32_t pbCount = 0;
  std::uint16_t* pbOfSlot = nullptr;  // [maxPlaybacks]
  ResState* res = nullptr;         // [maxReservations]
  std::uint32_t* due = nullptr;    // [maxReservations] scratch
  notify::EventRecord* notes = nullptr;  // [kNotifyCapacity]
  std::uint32_t noteCount = 0;
  std::uint32_t dropped = 0;
  MusicTransitionRecord log[kLogCapacity];
  std::uint32_t logCount = 0;
  // Current block.
  BlockIndex block = 0;
  ClockSample blockStart = 0;
  std::uint32_t blockFrames = 256;
  ClockSample BlockEnd() const noexcept { return blockStart + blockFrames; }
  BankTables tb;   // scratch, bound per playback / reservation
  BankTables tb2;  // second scratch (reservation targets)
};

// --- shared helpers (music_common.cpp) ---
void PushNote(State& s, notify::EventType type, std::uint32_t a, std::uint64_t object, ClockSample clock,
              std::uint32_t b = 0, std::uint16_t detail = 0) noexcept;
MusicPb* PbOf(State& s, SlotIndex playback) noexcept;
const MusicPb* PbOf(const State& s, SlotIndex playback) noexcept;
Timeline* TimelineOf(State& s, SlotIndex transport) noexcept;
ClockSample RoundClock(double c) noexcept;
// Prime frames before material `asset` can start at `frame` (MusicServices::primeFrames).
std::uint32_t PrimeOf(const State& s, std::uint16_t bank, std::uint32_t asset, std::uint64_t frame) noexcept;
// ClipRef naming an ASST row (core: snd/core/core.hpp kClipRefAssetTrack).
ClipRef AssetClip(std::uint16_t bank, std::uint32_t asset) noexcept;
// Pickup lead (output samples) of the first segment of `stateRow`'s playlist.
double StatePickupSamples(const State& s, const BankTables& tb, std::uint32_t stateRow) noexcept;

// The music playback that owns transport `slot`, or nullptr.
MusicPb* PbOfTransport(State& s, SlotIndex transport) noexcept;
// Prime frames of an event's asset clips (max), for reservation lead times (8.2).
std::uint32_t EventPrimeFrames(const State& s, const BankTables& tb, std::uint32_t eventRow) noexcept;
// Pickup lead (output samples) of an event: a music event's first segment, or the
// segment that uses the event's first clip asset (PICKUP_ALIGN, D-21).
double EventPickupSamples(const State& s, const BankTables& tb, std::uint32_t eventRow) noexcept;

// Segment tick queries of a music playback (for NEXT_MARKER / SEGMENT_END When kinds).
// The instance sounding at `tick` (main or bridge), or -1.
std::int32_t InstanceAt(const MusicPb& pb, double tick) noexcept;
bool SegmentEndAfter(const MusicPb& pb, double tick, double* out) noexcept;
bool NextMarkerAfter(const State& s, const BankTables& tb, const MusicPb& pb, double tick, Id marker,
                     double* out) noexcept;

// --- music logic (music_logic.cpp / transition.cpp) ---
void HandleSetState(State& s, const BankTables& tb, MusicPb& pb, Id target, ClockSample clock,
                    bool whenOverride) noexcept;
void UpdatePlayback(State& s, const schema::Tables& t, MusicPb& pb, MusicVoiceRequestList& out) noexcept;
void ExecuteTransition(State& s, const BankTables& tb, MusicPb& pb, Timeline& tl) noexcept;
// Re-places the current segment after a seek of the owned transport.
void Relocate(State& s, const BankTables& tb, MusicPb& pb, Timeline& tl) noexcept;
void ReleasePb(State& s, MusicPb& pb) noexcept;
void StartMusic(State& s, const BankTables& tb, MusicPb& pb, ClockSample clock) noexcept;
// Plans a segment instance at startTick (tempo points added from max(start, anchor)).
std::int32_t PlanInstance(State& s, const BankTables& tb, MusicPb& pb, Timeline& tl, Role role, std::uint32_t seg,
                          std::uint32_t stateRow, MusicalTicks startTick, MusicalTicks anchorTick,
                          float fadeIn) noexcept;
void LogTransition(State& s, const MusicPb& pb, Id from, Id to, double tick, double clock, std::uint8_t late,
                   std::uint32_t deferrals) noexcept;

}  // namespace snd::music::detail

#endif  // SND_MUSIC_SRC_MUSIC_STATE_HPP
