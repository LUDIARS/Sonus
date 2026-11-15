// Internal value types shared by the table schema, commands and the
// core <-> source / music interfaces. FROZEN CONTRACT.
//
// All types are trivially copyable PODs with fixed sizes (static_assert'ed),
// so they can live in SoA columns, command payloads and replay records.
#ifndef SND_INTERNAL_COMMON_HPP
#define SND_INTERNAL_COMMON_HPP

#include <cstddef>
#include <cstdint>
#include <type_traits>

#include "snd/ext/types.hpp"
#include "snd/id.hpp"
#include "snd/types.h"

namespace snd {

// ---------------------------------------------------------------------------
// Indices, clocks
// ---------------------------------------------------------------------------
using SlotIndex = std::uint32_t;  // stable index (3.1); never a dense position
inline constexpr SlotIndex kInvalidSlot = 0xFFFFFFFFu;

using ClockSample = std::uint64_t;  // common clock, internal sample rate (8.1)
using BlockIndex = std::uint64_t;   // number of blocks rendered since Create
using MusicalTicks = std::int64_t;  // absolute ticks from transport origin, 960 per beat (8.1)

inline constexpr std::uint32_t kParamInlineSlots = 8;          // 3.1 fixed-width per owner
inline constexpr std::uint32_t kParamOverflowBlockEntries = 16;  // 3.1 overflow block size
inline constexpr std::size_t kBufferAlignment = 64;  // 3.4

// Generation/slot pair as stored in commands and tables (= handle value split).
struct HandleRef {
  std::uint32_t slot;
  std::uint32_t generation;  // 0 = none
  static constexpr HandleRef FromValue(std::uint64_t v) noexcept {
    return HandleRef{static_cast<std::uint32_t>(v), static_cast<std::uint32_t>(v >> 32)};
  }
  constexpr std::uint64_t Value() const noexcept {
    return (static_cast<std::uint64_t>(generation) << 32) | slot;
  }
  constexpr bool IsNone() const noexcept { return generation == 0u; }
};
static_assert(sizeof(HandleRef) == 8);

struct RangeU32 {
  std::uint32_t first;
  std::uint32_t count;
};
static_assert(sizeof(RangeU32) == 8);

using Vec3 = snd_vec3;  // same alias as the public snd::Vec3
static_assert(sizeof(Vec3) == 12);

// ---------------------------------------------------------------------------
// Enumerations used in columns (stored as fixed-size integers)
// ---------------------------------------------------------------------------
enum class PlaybackStateId : std::uint8_t {  // 4.1, values = SND_PLAYBACK_STATE_*
  None = SND_PLAYBACK_STATE_NONE,
  Pending = SND_PLAYBACK_STATE_PENDING,
  Priming = SND_PLAYBACK_STATE_PRIMING,
  Playing = SND_PLAYBACK_STATE_PLAYING,
  Paused = SND_PLAYBACK_STATE_PAUSED,
  Stopping = SND_PLAYBACK_STATE_STOPPING,
  Ended = SND_PLAYBACK_STATE_ENDED,
};

enum class SourceKind : std::uint8_t {  // 5
  Sample = 0,
  Stream = 1,
  Oscillator = 2,
  Noise = 3,
  Granular = 4,
  Patch = 5,
  Callback = 6,
  UserBuffer = 7,
  Input = 8,
  Count = 9,
};

enum class ParamKind : std::uint8_t { Continuous = 0, Switch = 1 };

enum class ParamScope : std::uint8_t { Global = 0, Emitter = 1, Playback = 2 };

// ---------------------------------------------------------------------------
// References into bank data
// ---------------------------------------------------------------------------
// Event resolved at apply time: bank slot + row in that bank's EVNT table.
struct EventRef {
  std::uint16_t bankSlot;   // 0xFFFF = unresolved (not loaded)
  std::uint16_t flags;      // EventRefFlags
  std::uint32_t eventIndex;
};
static_assert(sizeof(EventRef) == 8);
enum EventRefFlags : std::uint16_t {
  kEventRefMusic = 1u << 0,    // event is a music event (owns a transport)
  kEventRefDynamic = 1u << 1,  // PlaySynth / PlayUserBuffer / PlayInput: no bank event
  kEventRefOneShot = 1u << 2,  // no handle was given out
};
inline constexpr std::uint16_t kUnresolvedBank = 0xFFFFu;

struct ClipRef {
  std::uint16_t bankSlot;
  std::uint16_t trackIndex;  // within the event
  std::uint32_t clipIndex;   // row in the bank's CLIP table
};
static_assert(sizeof(ClipRef) == 8);

// ---------------------------------------------------------------------------
// Column payload types
// ---------------------------------------------------------------------------
// Fractional read position of a voice in source frames.
struct Playhead {
  std::uint64_t frame;      // integer source frame
  std::uint32_t fraction;   // 2^-32 frame units
  std::uint32_t loopCount;  // completed loops
};
static_assert(sizeof(Playhead) == 16);

// Pending requests written by ApplyCommands, consumed by RunEventLogic.
// RunEventLogic never clears them: it remembers the last handled sequence in
// PlaybackLogic::handledRequestSeq (single-writer rule, 3.2).
struct PlaybackRequest {
  std::uint32_t seq;           // incremented on every new request
  std::uint32_t flags;         // PlaybackRequestFlags (latest wins per flag)
  float stopFadeSeconds;       // < 0 = event default
  float pauseFadeSeconds;
  SlotIndex stopReservation;   // reservation slot for a scheduled stop, kInvalidSlot if none
  SlotIndex startReservation;  // reservation slot of a quantised start (Pending), kInvalidSlot if none
};
static_assert(sizeof(PlaybackRequest) == 24);
enum PlaybackRequestFlags : std::uint32_t {
  kRequestStop = 1u << 0,
  kRequestPause = 1u << 1,
  kRequestResume = 1u << 2,
  kRequestScheduledStop = 1u << 3,
  kRequestDiscard = 1u << 4,  // bank unload (StopAndUnload) / system shutdown
};

struct PlaybackSchedule {
  ClockSample startClock;  // first audible sample (sample accurate, 4.4), 0 = not yet scheduled
  ClockSample stopClock;   // scheduled stop, 0 = none
};
static_assert(sizeof(PlaybackSchedule) == 16);

// Private per-playback logic state of RunEventLogic (sequence position etc.).
struct PlaybackLogic {
  std::uint32_t handledRequestSeq;
  std::uint32_t voiceCount;
  std::uint32_t sequenceCursor;  // 4.2 sequential / shuffle / combo state
  std::uint32_t lastChoice;      // random-no-repeat
  std::uint64_t playedFrames;    // frames played (GetPosition)
  float fadeGain;                // current stop/pause fade
  float fadeStep;                // per-block change
};
static_assert(sizeof(PlaybackLogic) == 32);

struct PlaybackEnd {
  std::uint32_t reason;  // snd_end_reason, 0 while alive
  std::uint32_t detail;  // snd_end_detail
};
static_assert(sizeof(PlaybackEnd) == 8);

// What a dynamic (non-bank) playback plays: index into the per-kind
// descriptor table owned by ApplyCommands (synth descs, user buffers, inputs).
struct DynamicSourceRef {
  SourceKind kind;  // Callback / UserBuffer / Input; Count = bank event
  std::uint8_t reserved[3];
  std::uint32_t descIndex;
};
static_assert(sizeof(DynamicSourceRef) == 8);

struct VoiceLifecycle {
  ClockSample startClock;  // sample-accurate start (offset inside block = startClock - blockStart)
  ClockSample stopClock;   // 0 = none
  std::uint8_t state;      // VoiceStateId
  std::uint8_t flags;      // VoiceFlags
  std::uint16_t reserved;
  float releaseSeconds;    // fade on stop
};
static_assert(sizeof(VoiceLifecycle) == 24);
enum class VoiceStateId : std::uint8_t { Starting = 0, Playing = 1, Releasing = 2, Ending = 3 };
enum VoiceFlags : std::uint8_t {
  kVoiceLoop = 1u << 0,
  kVoiceIgnorePause = 1u << 1,  // category ignores pause (4.6)
  kVoiceVirtualRestart = 1u << 2,  // return from virtual: restart from the beginning (4.4)
  kVoiceVirtualKill = 1u << 3,     // return from virtual: never come back
};

// Values at block start and block end (linear ramp across the block, 5.1).
struct Ramp {
  float start;
  float end;
};
static_assert(sizeof(Ramp) == 8);

struct FilterTarget {
  float lowpassHz;   // composed by min (4.3)
  float highpassHz;  // composed by max
};
static_assert(sizeof(FilterTarget) == 8);

struct VoiceRenderStatus {
  std::uint8_t ended;    // source reached its end (non-looping)
  std::uint8_t starved;  // stream / callback ring ran dry this block
  std::uint8_t late;     // chunk missed the deadline (2.3)
  std::uint8_t reserved;
  std::uint32_t framesRendered;
};
static_assert(sizeof(VoiceRenderStatus) == 8);

// 1.1 (D-43): the voice belonged to a chunk that missed the deadline and whose
// worker may still be running. Written by Render (serial part); SelectVoices,
// BuildRenderList (and RunEventLogic, which destroys voices) read the previous
// block's value. While held != 0 no system reads or writes the voice's render
// state (its RealVoice row and the real-slot pools), the voice is not put
// into the render list, and its real slot is neither reassigned (stolen) nor
// freed; its output was faded to silence from the end of the
// previous block. Render clears it once RtWorkerPool::IsQuiescent() is true
// (StragglersInFlight() is for statistics only); the render state is then the
// value the late chunk wrote.
struct VoiceRenderHold {
  std::uint8_t held;            // 1 = held (do not touch the render state)
  std::uint8_t reserved[3];
  std::uint32_t lateGeneration; // RtWorkerPool batch generation of the late chunk
};
static_assert(sizeof(VoiceRenderHold) == 8);

struct RealVoiceAssign {
  std::uint32_t assignSeq;    // bumps on (re)assignment; late chunk results compare it (2.3)
  std::uint8_t fadeReserve;   // slot is one of the fade reserve slots (4.4)
  std::uint8_t fadingOut;     // being stolen: fade to 0 then free
  std::uint16_t reserved;
  float fadeSeconds;
};
static_assert(sizeof(RealVoiceAssign) == 12);

// 1.1 (D-44): the former 64-byte RealVoice.resamplerState column is gone. The
// resampler state of real voice slot r (RealVoice is a Direct table, D-43)
// lives in core's resampler pool at index r: (maxRealVoices + fadeReserveVoices) slots of
// dsp::ResamplerStateBytes(Sinc, Config.maxVoiceChannels, Config.maxPitchRatio)
// bytes, 64-byte aligned (MemTag Tables). Pitch ratios are clamped to
// Config.maxPitchRatio.

struct alignas(16) FilterState {
  float data[64];  // 2 filters x 4 state x kMaxChannels
};
static_assert(sizeof(FilterState) == 256);

struct alignas(16) PanMatrix {
  float gain[kMaxChannels][kMaxChannels];  // [input channel][output channel]
};
static_assert(sizeof(PanMatrix) == 256);

struct BusPeak {
  float peak[kMaxChannels];
};
static_assert(sizeof(BusPeak) == 32);

// Time-based ramp request (set once by a command, evaluated by readers).
struct TimedRamp {
  float from;
  float to;
  ClockSample startClock;
  std::uint32_t durationSamples;
  std::uint32_t reserved;
};
static_assert(sizeof(TimedRamp) == 24);

struct Occlusion {
  float occlusion;    // 0..1
  float obstruction;  // 0..1
};
static_assert(sizeof(Occlusion) == 8);

struct TimeSignature {
  std::uint16_t beatsPerBar;
  std::uint16_t beatUnit;
};
static_assert(sizeof(TimeSignature) == 4);

struct TempoMapRef {
  std::uint16_t bankSlot;      // kUnresolvedBank for standalone transports
  std::uint16_t overrideCount; // runtime SetTempo overrides held by the music module
  std::uint32_t firstPoint;    // row in the bank's tempo point table
  std::uint32_t pointCount;
  float baseBpm;               // standalone transports / last override
};
static_assert(sizeof(TempoMapRef) == 16);

struct TransportPosition {
  MusicalTicks tick;          // integer ticks at anchorClock
  double tickFraction;        // [0,1)
  ClockSample anchorClock;    // clock of the block start this position refers to
  double ticksPerSample;      // current rate incl. pitch
};
static_assert(sizeof(TransportPosition) == 32);

struct MusicalRange {
  MusicalTicks begin;
  MusicalTicks end;  // end <= begin: no loop
};
static_assert(sizeof(MusicalRange) == 16);

struct TransportLifetime {
  std::uint32_t destroyRequested;
  std::uint32_t createdFromPlayback;  // 1 = owned by a music playback
};
static_assert(sizeof(TransportLifetime) == 8);

struct TransportInit {
  float bpm;
  TimeSignature signature;
  std::uint32_t startPlaying;
  std::uint32_t reserved;
};
static_assert(sizeof(TransportInit) == 16);

// When as stored in the Reservation table (same fields as snd_when minus the handle).
struct ReservationWhen {
  std::uint64_t value;
  std::int32_t offsetTicks;
  std::uint32_t count;
  std::uint8_t kind;  // snd_when_kind
  std::uint8_t flags;
  std::uint8_t lateRule;  // ReservationLateRule (8.5 "間に合わない時"), filled from the rule table
  std::uint8_t reserved[5];
};
static_assert(sizeof(ReservationWhen) == 24);

enum class ReservationAction : std::uint8_t {
  Play = 0,
  Stop = 1,
  ScheduledStop = 2,
  SetMusicState = 3,
  SetTempo = 4,
  Seek = 5,
  SetTransportState = 6,
  Stinger = 7,
};

enum class ReservationLateRule : std::uint8_t {
  NextGrid = 0,   // default: move to the next grid point of the same kind
  StartLate = 1,  // fire now, offset into the material ("遅れて開始")
  Cancel = 2,     // end with SND_END_DETAIL_RESERVATION_MISSED
};

struct ReservationPayload {
  std::uint8_t bytes[16];  // action-specific (commands.hpp documents each layout)
};
static_assert(sizeof(ReservationPayload) == 16);

struct ResolvedTime {
  MusicalTicks tick;         // target after quantisation (and deferral)
  std::uint32_t deferrals;   // times moved by the late rule
  std::uint32_t resolved;    // 0 = not yet converted from the When
};
static_assert(sizeof(ResolvedTime) == 16);

struct StreamControl {
  std::uint8_t state;        // StreamStateId
  std::uint8_t priority;     // 6.1 music > voice > ambience
  std::uint8_t stallPolicy;  // 0 = time priority, 1 = sound priority (6.3, 8.4)
  std::uint8_t flags;
  std::uint32_t chunkBytes;
  std::uint64_t loopStartFrame;
  std::uint64_t loopEndFrame;  // 0 = no loop
};
static_assert(sizeof(StreamControl) == 24);
enum class StreamStateId : std::uint8_t { Priming = 0, Running = 1, Stalled = 2, Paused = 3, Closing = 4 };

struct CategoryRequest {
  TimedRamp volume;
  std::uint32_t paused;
  std::uint32_t muted;
};
static_assert(sizeof(CategoryRequest) == 32);

struct SnapshotRequest {
  TimedRamp weight;  // 4.6 weighted, time-based transition
};
static_assert(sizeof(SnapshotRequest) == 24);

}  // namespace snd

#endif  // SND_INTERNAL_COMMON_HPP
