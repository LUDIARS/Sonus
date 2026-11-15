// core <-> music internal interface (design.md 8, 12.2-2). FROZEN CONTRACT.
//
// The music module implements IMusicSystem. Core calls it at fixed points of
// the block (all on the audio thread, principle 1 applies):
//
//   1 ApplyCommands     core: creates Transport rows (CreateTransport, music Play)
//                       and Reservation rows (any request with a non-immediate
//                       When, and immediate transport changes with "now").
//   2 UpdateTransport   music: IMusicSystem::UpdateTransport. Advances every
//                       Transport, applies tempo/seek/state reservations itself
//                       (it owns those columns), converts the remaining due
//                       reservations into the FireList (sample-accurate) and
//                       destroys their rows. Beat/bar notifications are queued
//                       internally for Publish.
//   4 RunEventLogic     core: consumes the FireList for Play / Stop /
//                       ScheduledStop. For music playbacks it forwards
//                       lifecycle and SetMusicState/Prepare/Stinger entries to
//                       IMusicSystem::RunMusicLogic, executes the returned
//                       MusicVoiceRequests (creating/stopping voices through the
//                       normal voice path, so voice management applies, 8.3) and
//                       reports the outcome via OnVoiceResults in the same block.
//  10 Publish           core: IMusicSystem::CollectNotifications into the event ring.
//
// Transport time base: absolute ticks (960 per beat) since the transport
// origin; bars/beats are derived through the tempo map and time signatures.
#ifndef SND_INTERNAL_MUSIC_IF_HPP
#define SND_INTERNAL_MUSIC_IF_HPP

#include <cstddef>
#include <cstdint>

#include "snd/ext/allocator.hpp"
#include "snd/internal/common.hpp"
#include "snd/internal/notify.hpp"
#include "snd/internal/schema.hpp"
#include "snd/internal/source_if.hpp"

namespace snd::music {

struct MusicFrame {
  source::BlockInfo block;
  std::uint32_t lookaheadFrames;  // block + render ahead: reservations due within fire now (8.2)
  std::uint32_t reserved;
  double outputLatencySeconds;    // for the audible-time estimate of notifications (8.1)
};

// --- FireList (UpdateTransport -> RunEventLogic) --------------------------------
struct FireEntry {
  SlotIndex reservation;         // already destroyed; for logging only
  ReservationAction action;      // Play, Stop, ScheduledStop, SetMusicState, Stinger
  std::uint8_t lateRuleApplied;  // ReservationLateRule if it was late, 0xFF otherwise
  std::uint16_t reserved;
  HandleRef target;              // playback (or transport for transport actions)
  ReservationPayload payload;    // copied from the reservation
  ClockSample fireClock;         // exact sample (may be inside a later block within lookahead)
  std::uint64_t sequence;        // Reservation.sequence; ties are fired in sequence order (2.5)
};
static_assert(sizeof(FireEntry) == 48);

struct FireList {
  FireEntry* entries;
  std::uint32_t count;
  std::uint32_t capacity;  // Config.maxReservations
};

// --- Music logic (inside RunEventLogic) ------------------------------------------
enum class MusicInputKind : std::uint8_t {
  PlaybackStarted = 0,  // a music playback became Playing (transportSlot valid)
  PlaybackStopping = 1, // stop requested: fade per rules, then report done
  PlaybackEnded = 2,    // row destroyed: drop private state
  SetState = 3,         // SetMusicState (from command or fired reservation)
  PrepareState = 4,     // PrepareMusicState (8.3 pre-prime)
  Stinger = 5,          // fired stinger reservation
  VoiceEnded = 6,       // a voice created for this playback ended (render status)
};

struct MusicInput {
  MusicInputKind kind;
  std::uint8_t reserved[3];
  SlotIndex playbackSlot;
  SlotIndex transportSlot;
  EventRef event;
  Id stateId;           // SetState / PrepareState
  SlotIndex voiceSlot;  // VoiceEnded
  ClockSample clock;    // when it takes effect (fire clock or block start)
  std::uint32_t whenOverride;  // 1: caller's When overrides the rule timing (snd_set_music_state)
  std::uint32_t reserved2;
};

enum class MusicVoiceOp : std::uint8_t {
  StartClip = 0,  // start a segment / stem / bridge clip
  StopVoice = 1,  // stop with fade
  SetGain = 2,    // stem volume, transition fades (linear gain ramp target)
  PlayEvent = 3,  // stinger or nested event: core creates a child playback
};

struct MusicVoiceRequest {
  MusicVoiceOp op;
  std::uint8_t reserved[3];
  SlotIndex playbackSlot;
  SlotIndex voiceSlot;     // StopVoice / SetGain
  std::uint32_t tag;       // music-defined id echoed in MusicVoiceResult
  ClipRef clip;            // StartClip
  EventRef event;          // PlayEvent
  ClockSample startClock;  // sample-accurate start
  std::uint64_t startFrame;  // offset into the material (same-bar entry, last position, pickup)
  float gain;              // StartClip initial / SetGain target
  float fadeSeconds;       // StopVoice / SetGain ramp
  std::uint32_t syncGroup; // streams of one music playback share a group (8.4)
  std::uint32_t reserved2;
};

struct MusicVoiceRequestList {
  MusicVoiceRequest* requests;
  std::uint32_t count;
  std::uint32_t capacity;  // when full, music must defer to the next block
};

struct MusicVoiceResult {
  std::uint32_t tag;
  SlotIndex voiceSlot;     // kInvalidSlot if rejected
  std::uint32_t endDetail; // snd_end_detail when rejected
  std::uint32_t reserved;
};

// --- Services provided by core -------------------------------------------------------
struct MusicServices {
  IAllocator* allocator;  // Init only
  // Bank music tables (MSEG, MPLS, MPLE, MSTM, MSTA, MTRN, MSTG, MMRK, MTMP) of a
  // loaded bank: returns the chunk start or nullptr. RT-safe (table lookup).
  const std::uint8_t* (*bankChunk)(void* ctx, std::uint16_t bankSlot, std::uint32_t fourcc,
                                   std::uint64_t* bytes) noexcept;
  void* bankCtx;
  // Lead time before a clip can start at startFrame (stream priming, 8.2). RT-safe.
  std::uint32_t (*primeFrames)(void* ctx, ClipRef clip, std::uint64_t startFrame) noexcept;
  void* primeCtx;
};

struct MusicLimits {
  std::uint32_t maxTransports;
  std::uint32_t maxReservations;
  std::uint32_t maxPlaybacks;   // music state is kept per playback slot
  std::uint32_t maxVoiceRequestsPerBlock;
  std::uint32_t sampleRate;
  std::uint32_t blockFrames;
};

class IMusicSystem {
 public:
  virtual ~IMusicSystem() = default;

  // Owner thread (Create / Destroy).
  virtual std::size_t PoolBytes(const MusicLimits& limits) const noexcept = 0;
  virtual bool Init(void* pool, std::size_t bytes, const MusicLimits& limits,
                    const MusicServices& services) noexcept = 0;
  virtual void Shutdown() noexcept = 0;

  // System 2. Writes only the columns schema.hpp assigns to UpdateTransport,
  // destroys Transport/Reservation rows, fills `fire` (count reset by callee).
  virtual void UpdateTransport(const MusicFrame& frame, schema::Tables& tables,
                               FireList& fire) noexcept = 0;

  // Inside system 4. Reads tables (declared RunEventLogic reads only), appends
  // to `out` (count not reset by callee).
  virtual void RunMusicLogic(const MusicFrame& frame, const schema::Tables& tables,
                             const MusicInput* inputs, std::uint32_t count,
                             MusicVoiceRequestList& out) noexcept = 0;
  virtual void OnVoiceResults(const MusicVoiceResult* results, std::uint32_t count) noexcept = 0;

  // System 10. Beats, bars, markers, late reservations. Returns records written.
  virtual std::uint32_t CollectNotifications(notify::EventRecord* out,
                                             std::uint32_t capacity) noexcept = 0;

  // Any audio-thread system: musical <-> clock conversion on the current
  // timeline of a transport (as of the last UpdateTransport). false if invalid.
  virtual bool TicksToClock(SlotIndex transport, MusicalTicks ticks,
                            ClockSample* out) const noexcept = 0;
  virtual bool ClockToTicks(SlotIndex transport, ClockSample clock,
                            MusicalTicks* out) const noexcept = 0;
};

}  // namespace snd::music

#endif  // SND_INTERNAL_MUSIC_IF_HPP
