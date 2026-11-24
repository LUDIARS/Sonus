// core internals: the state of one System (tables, queues, system-private
// state) and the declarations shared by the system implementations.
//
// Ownership inside core follows the schema (3.2): every table column is
// written by one system only, through schema::WriteColumn<System, Column> /
// ReadColumn (see access.hpp). Data that is not a table column is grouped by
// the system that writes it (the "*Priv" structs below); a few of those are
// read by later systems of the same block, which is noted at each field
// (proposed as schema columns in the stage-2 report).
#ifndef SND_CORE_SRC_CORE_IMPL_HPP
#define SND_CORE_SRC_CORE_IMPL_HPP

#include <atomic>
#include <cstddef>
#include <cstdint>

#include "id_map.hpp"
#include "snd/bank/bank_manager.hpp"
#include "snd/bank/prepared.hpp"
#include "snd/base/arena.hpp"
#include "snd/base/handle.hpp"
#include "snd/base/mpmc.hpp"
#include "snd/base/mutex.hpp"
#include "snd/base/notify.hpp"
#include "snd/base/pool.hpp"
#include "snd/base/sparse_set.hpp"
#include "snd/base/spsc.hpp"
#include "snd/core/core.hpp"
#include "snd/dsp/effects.hpp"
#include "snd/dsp/filter.hpp"
#include "snd/dsp/meter.hpp"
#include "snd/dsp/pan.hpp"
#include "snd/dsp/resampler.hpp"
#include "snd/internal/commands.hpp"
#include "snd/internal/notify.hpp"
#include "snd/internal/schema.hpp"
#include "snd/jobs/rt_workers.hpp"
#include "fx_control.hpp"

namespace snd::core {

namespace bf = ::snd::bank_format;
namespace bk = ::snd::bank;

inline constexpr std::uint32_t kNoSlot = kInvalidSlot;
inline constexpr std::uint16_t kNoBank = kUnresolvedBank;
inline constexpr std::uint32_t kMaxBusEffects = 16;       // effects processed per bus
inline constexpr std::uint32_t kMaxNestedDepth = 4;       // nested events (4.2)
// Core-internal BankUnload modes (language switch, 10.4). The bank stays LOADED for
// the game while its last voices play; the bank manager learns about the unload only
// when core unregisters it (then UNLOADING, then released after the epoch).
inline constexpr std::uint32_t kUnloadWhenUnused = 0x100u;
inline constexpr std::uint32_t kCancelUnloadWhenUnused = 0x101u;  // switched back before it was unused
inline constexpr std::uint32_t kMaxLanguageSwitches = 64;

// ---------------------------------------------------------------------------
// Configuration derived once at Create
// ---------------------------------------------------------------------------
struct Cfg {
  std::uint32_t sampleRate, blockFrames, outChannels;
  std::uint32_t maxReal, fadeReserve, realTotal, maxVirtual;
  std::uint32_t maxPlaybacks, maxEmitters, maxListeners, maxTransports, maxReservations, maxStreams;
  std::uint32_t maxBanks, maxUserBuffers, maxGlobalParams, paramOverflowBlocks;
  std::uint32_t emitterParamRows, playbackParamRows;
  std::uint32_t voicesPerChunk, maxChunks;
  std::uint32_t busCap, catCap, snapCap, groupCap, paramDefCap, duckCap;
  std::uint32_t arenaBytes, reserveBytes, arenasPerThread, maxThreads;
  std::uint32_t criticalPriority, overflowParamSlots, eventRingCapacity;
  std::uint32_t shortFadeFrames, maxVoiceChannels, maxVoicesPerPlay;
  std::uint32_t extrapolateFrames, loadTargetPercent, lodDownBlocks, lodUpBlocks;
  float maxPitchRatio, stealHysteresisDb, shortFadeSeconds;
  std::uint32_t listenerMode, seed;
  std::size_t resamplerSlotBytes;
};

// ---------------------------------------------------------------------------
// Game side: command producers (2.1, commands.hpp)
// ---------------------------------------------------------------------------
struct ArenaView {
  cmd::ArenaHeader* header = nullptr;
  std::uint8_t* normal = nullptr;
  std::uint8_t* reserve = nullptr;
  cmd::OverflowFallback fallback{};
};

struct Producer {
  // 0 free, 1 owned by a registered thread, 2 retiring: unregistered with an arena
  // that could not be published yet; Update publishes it, then frees the producer.
  // 3: Update is publishing a retiring producer (back to 0 or 2 right after).
  std::atomic<std::uint32_t> active{0};
  // The thread that unregistered last (ThreadToken()); 0 while owned. A thread that
  // registers again takes the same producer back (its arena continues, order kept).
  std::atomic<std::uintptr_t> lastThread{0};
  base::ArenaCycle cycle;
  ArenaView* arenas = nullptr;  // [arenasPerThread]
  std::uint32_t seq = 0;        // producer private
  std::uint64_t publishSeq = 0;
  std::uint32_t index = 0;
};

struct RejectedPlay {
  std::uint64_t handle;
  std::uint32_t oneShot;
  std::uint32_t reserved;
};

// Source services (D-08): requests posted by RT code, drained by PumpServices.
struct PostedJob {  // the job itself is in its JobWrap (taken when postJob accepts it)
  std::uint32_t wrap;
  JobLane lane;
};
struct JobWrap {  // trampoline record: counts the job for the bank epoch (3.3)
  JobDesc original;
  System::Impl* owner;
  std::uint32_t index;
  std::uint32_t epochBucket;
  std::uint32_t done;  // the wrapper's own done counter (submitted desc)
  std::uint32_t reserved;
};

// Audio -> game bank events.
enum class BankEventKind : std::uint32_t { Release = 0, UnloadRefused = 1, RegisterFailed = 2, Unregistered = 3 };
struct BankEvent {
  BankEventKind kind;
  std::uint32_t slot;
  std::uint32_t generation;
  std::uint32_t reserved;
};

// ---------------------------------------------------------------------------
// Banks: core's record of a registered bank (prepared on the owner thread at
// PopRegistration, read by the audio thread after the BankRegister command).
// ---------------------------------------------------------------------------
struct EffectInstance {
  Id type = 0;
  const snd_dsp_effect* plugin = nullptr;  // Config.plugins entry, or null (standard)
  void* state = nullptr;
  float* params = nullptr;   // current values (ParamBlock start = end)
  std::uint32_t paramCount = 0;
  std::uint32_t channels = 0;
  std::uint32_t ok = 0;      // 1 when initialised
  std::uint32_t reserved = 0;
  // D-130: runtime control ([kMaxEffectParams] each, bank record memory). `params` stays the
  // bank value (base); the ParamBlock of a block is blockStart -> blockEnd (Render).
  EffectParamControl* ctl = nullptr;
  float* blockStart = nullptr;
  float* blockEnd = nullptr;
};

struct EventState {  // RunEventLogic private, per EVNT row (4.2, 4.4)
  std::uint32_t instances;
  std::uint32_t cursor;
  std::uint32_t lastChoice;
  std::uint32_t reserved;
  std::uint64_t shuffleMask;
  ClockSample lastStartClock;
  ClockSample comboClock;
  // Stage 3 (group 4), D-141: shuffle without a clip limit. The order of a round is a
  // permutation of the clips keyed by shuffleKey (drawn from the playback that starts
  // the round); shufflePos = plays done in the round; shuffleRot = 1 when the round is
  // rotated by one so that it does not start with the last clip of the previous round.
  std::uint32_t shuffleKey;
  std::uint32_t shufflePos;
  std::uint32_t shuffleRot;
  std::uint32_t shuffleLast;  // clip index (within the track) played last + 1, 0 = none
};

struct BankRecord {
  HandleRef handle{};
  const bk::PreparedBank* prepared = nullptr;
  const bk::BankView* view = nullptr;
  Id bankId = 0;
  std::uint32_t languageId = 0;
  // Event index, sorted by id (binary search, apply time).
  std::uint32_t eventCount = 0;
  std::uint32_t* eventIds = nullptr;
  std::uint32_t* eventRows = nullptr;
  // Sources (stable while registered).
  std::uint32_t assetCount = 0;
  source::SourceAsset* assets = nullptr;
  std::uint32_t synthCount = 0;
  source::OscillatorParams* osc = nullptr;
  source::NoiseParams* noise = nullptr;
  // Mappings bank row -> core slot (filled by ApplyCommands at registration).
  std::uint32_t busCount = 0, catCount = 0, snapCount = 0, groupCount = 0, duckCount = 0;
  std::uint32_t* busSlot = nullptr;
  std::uint32_t* catSlot = nullptr;
  std::uint32_t* snapSlot = nullptr;
  std::uint32_t* groupSlot = nullptr;
  std::uint32_t* duckSlot = nullptr;
  std::uint32_t* busChannels = nullptr;  // resolved channel count per BUSS row (D-54 rule)
  // RunEventLogic per-event state.
  EventState* events = nullptr;
  // Effects (EFFX rows), instance memory from MemTag::DspPool.
  std::uint32_t effectCount = 0;
  EffectInstance* effects = nullptr;
  std::uint8_t* dspMemory = nullptr;
  std::uint64_t dspBytes = 0;
  void* memory = nullptr;  // one block for everything above except dspMemory
  // D-131: previous-block output of the source bus of each DUCK row of kind 0 (sidechain),
  // used when the source bus is mixed after the target bus. [duckCount] index into
  // sidechainPrev (kNoSlot: not a sidechain row), [n * kMaxChannels * blockFrames], channels.
  std::uint32_t* sidechainIndex = nullptr;
  float* sidechainPrev = nullptr;
  std::uint32_t* sidechainChannels = nullptr;
};

enum class BankAudioState : std::uint8_t { Free = 0, Registered = 1, Unloading = 2, AwaitEpoch = 3 };
struct BankAudio {  // ApplyCommands private, per bank slot
  BankRecord* rec = nullptr;
  std::uint32_t generation = 0;
  BankAudioState state = BankAudioState::Free;
  std::uint32_t unloadMode = 0;
  std::uint32_t discardSent = 0;
  std::uint32_t pendingUnload = 0;   // BankUnload arrived before BankRegister
  std::uint32_t pendingMode = 0;
  std::uint32_t pendingGeneration = 0;
  BlockIndex unregisterBlock = 0;
  std::uint32_t jobEpoch = 0;
  std::uint32_t order = 0;           // registration order (lookup precedence)
};

// ---------------------------------------------------------------------------
// Shared objects unified by id (A1-16). Written by ApplyCommands at bank
// registration / unregistration; read by later systems (definitions only).
// ---------------------------------------------------------------------------
struct BusDef {
  Id id = 0;
  std::uint32_t refCount = 0;
  std::uint16_t defBank = kNoBank;
  std::uint16_t reserved = 0;
  std::uint32_t defRow = 0;
  std::uint32_t channels = 2;
  std::uint32_t flags = 0;
  float baseVolumeDb = 0;
  std::uint32_t depth = 0;
};
struct CatDef {
  Id id = 0;
  std::uint32_t refCount = 0;
  std::uint32_t parentSlot = kNoSlot;
  std::uint32_t flags = 0;
  float baseVolumeDb = 0;
};
struct SnapDef {
  Id id = 0;
  std::uint32_t refCount = 0;
  std::uint16_t defBank = kNoBank;
  std::uint16_t reserved = 0;
  std::uint32_t defRow = 0;
};
struct GroupDef {
  Id id = 0;
  std::uint32_t refCount = 0;
  std::uint32_t maxVoices = 0;
  std::uint32_t stealMode = 0;  // 0 oldest, 1 quietest, 2 lowest priority, 3 never
};
struct ParamDef {
  Id id = 0;
  std::uint32_t refCount = 0;
  std::uint32_t kind = 0;
  std::uint32_t scopeMask = 7;
  float minValue = 0, maxValue = 1, defaultValue = 0, rampRate = 0;
};
struct DuckRule {
  std::uint32_t refCount = 0;
  std::uint32_t kind = 0;          // 0 sidechain (bus), 1 rule (category)
  std::uint32_t sourceSlot = kNoSlot;
  std::uint32_t targetBus = kNoSlot;
  Id sourceId = 0, targetId = 0;
  float amountDb = 0, attackMs = 10, releaseMs = 200, thresholdDb = 0;
};

// ---------------------------------------------------------------------------
// ApplyCommands private state
// ---------------------------------------------------------------------------
struct ParamChain {  // per owner (emitter / playback slot)
  std::uint32_t count = 0;
  std::uint32_t tail = kNoSlot;
  std::uint32_t firstBlock = kNoSlot;  // overflow blocks chained through blockNext
  std::uint32_t lastBlock = kNoSlot;
};
struct DynamicDesc {  // what a dynamic playback plays (Playback.dynamicSource.descIndex = playback slot)
  snd_synth_desc synth;
  snd_input_desc input;
  HandleRef userBuffer;
  Id bus;
  std::uint32_t kind;  // SourceKind
  HandleRef followTransport;  // 1.2 (D-90): Transport row (slot, generation) of the start When, or none
};
struct LiveOverride {
  std::uint32_t kind;
  Id id;
  std::uint32_t value;
  std::uint32_t used;
};
struct ApplyPriv {
  ParamChain* emitterChain = nullptr;   // [maxEmitters]
  ParamChain* playbackChain = nullptr;  // [maxPlaybacks]
  base::IndexFreeList emitterBlocks, playbackBlocks;
  std::uint32_t* emitterBlockNext = nullptr;
  std::uint32_t* playbackBlockNext = nullptr;
  IdMap globalParams;                   // param id -> ParamGlobal slot (read by later systems)
  std::uint32_t* globalParamStorage = nullptr;
  base::IndexFreeList globalFree;
  DynamicDesc* dynamic = nullptr;       // [maxPlaybacks]
  // [maxPlaybacks] 1.2 (D-104): the playback's own volume (linear) and pitch (ratio), 1 when
  // created; written by ApplyCommands (kParamPlaybackVolume / Pitch), read by EvaluateDspParams.
  float* playbackVolume = nullptr;
  float* playbackPitch = nullptr;
  std::uint32_t* implicitEmitter = nullptr;  // [maxPlaybacks] PlayAt emitter slot
  std::uint32_t* implicitOwnerGen = nullptr; // [maxEmitters] playback generation owning an implicit emitter, 0 none
  std::uint32_t* userBufferRefs = nullptr;   // [maxUserBuffers]
  // [maxUserBuffers] generation whose destroy was applied (0 = none). It stays set after the
  // release, until a new buffer takes the slot, so a play applied later is refused (D-97).
  std::uint32_t* userBufferDestroy = nullptr;
  std::uint32_t* userBufferReleased = nullptr;  // [maxUserBuffers] generation whose release was published
  // [maxTransports] generation of a standalone transport row created this block (0 = none):
  // Publish returns the slot even when the row was destroyed again before it (same block).
  std::uint32_t* transportCreated = nullptr;
  std::uint32_t reservationCursor = 0;
  std::uint64_t reservationSeq = 0;
  std::uint32_t playCounter = 0;
  Id language = 0;
  std::uint32_t registrationOrder = 0;
  BankAudio* banks = nullptr;           // [maxBanks]
  LiveOverride* overrides = nullptr;    // [maxLiveOverrides]
  std::uint32_t overrideCount = 0;
  std::uint32_t suspended = 0;
  std::uint32_t graphDirty = 0;
  // Stage 3 (group 4).
  // D-146: fade-in of the playback's start (seconds, 0 = none), from Play.flags; read by
  // RunEventLogic when it initialises the row and by EvaluateDspParams.
  float* playbackFadeIn = nullptr;  // [maxPlaybacks]
  // D-145: child playbacks ApplyCommands created for the actions inside the first block of
  // an immediate play (cancelled by RunEventLogic if the parent does not start). Per block.
  struct ActionChild {
    HandleRef parent;
    HandleRef child;
  };
  ActionChild* actionChildren = nullptr;  // [actionChildCap]
  std::uint32_t actionChildCount = 0, actionChildCap = 0;
  std::uint32_t actionChildWarned = 0;    // a LIMIT_REACHED warning was posted this block
  // D-145: snapshot actions at a play's very first frame (its start block): a weight that is
  // in force only if the play starts. RunEventLogic cancels the entries of a play that did
  // not start; EvaluateDspParams uses the others in this block; the next ApplyCommands
  // writes them into Snapshot.request.
  struct TentativeSnapshot {
    SlotIndex snapshot;
    float weight;
    HandleRef parent;
    std::uint32_t cancelled;
    std::uint32_t reserved;
  };
  TentativeSnapshot* tentative = nullptr;  // [actionChildCap]
  std::uint32_t tentativeCount = 0;
  // D-145: the start clock a scheduled play (CLOCK_SAMPLE / DELAY_US) is expected to have,
  // 0 = not known ahead (immediate, musical). RunEventLogic looks its actions ahead with it.
  ClockSample* playbackStartClock = nullptr;  // [maxPlaybacks]
};

// ---------------------------------------------------------------------------
// RunEventLogic private state
// ---------------------------------------------------------------------------
// Core-internal PlaybackRequest flag bits (above the contract's, D-17 request column).
inline constexpr std::uint32_t kRequestRejectReservationFull = 1u << 16;  // no reservation slot: end REJECTED
inline constexpr std::uint32_t kRequestBufferDestroyed = 1u << 17;  // D-97: its user buffer is being destroyed
// D-145: ApplyCommands applied the actions of the playback's first block already.
inline constexpr std::uint32_t kRequestActionsApplied = 1u << 18;

enum PlaybackPrivFlags : std::uint32_t {
  kPbStarted = 1u << 0,
  kPbPausedUser = 1u << 1,
  kPbPausedCategory = 1u << 2,
  kPbStopping = 1u << 3,
  kPbEndPending = 1u << 4,   // end once the last voice is gone
  kPbInstanceCounted = 1u << 5,
  kPbMusic = 1u << 6,
  kPbPausedState = 1u << 7,  // currently in Paused state (voices paused)
  kPbSourceStarted = 1u << 8,  // a source accepted at least one voice (FlushSourceCalls)
  kPbTimedStop = 1u << 9,    // stopping by voice releases from a clock (sample accurate)
  // Stage 3 (group 4).
  kPbVoiceDropped = 1u << 10,  // D-140: a voice ended because it went virtual ("never" return)
  kPbCancelled = 1u << 11,     // D-145: child of an action whose parent did not start
  kPbActionStop = 1u << 12,    // D-145: a StopEvent action of a start block asks to stop
};
struct PlaybackPriv {
  std::uint32_t flags;
  std::uint32_t endReason;
  std::uint32_t endDetail;
  std::uint32_t rng;
  std::uint32_t categorySlot;
  std::uint32_t generation;
  float fadeTarget;     // fade gain the fade is heading to (0 stop/pause, 1 resume)
  float fadeRate;       // gain per block (> 0)
  ClockSample stopClock;   // scheduled stop from a fired reservation
  float stopFadeSeconds;
  std::uint32_t voiceListHead;  // first voice slot of the playback (linked through VoiceStatic.nextInPlayback)
  // Stage 3 (group 4), D-145: the track time (frames the playback played, not counting
  // pauses or the stop), the track time up to which its actions were taken, the start clock
  // its actions were looked ahead with (scheduled plays), a StopEvent action's clock.
  std::uint64_t actionHorizon;
  std::uint64_t actionOnlyEnd;  // exclusive track frame of the last effective action (zero for other events)
  std::uint64_t trackTime;
  ClockSample actionStart;
  ClockSample actionStopClock;
};

enum VoiceStaticFlags : std::uint32_t {
  kVs3D = 1u << 0,
  kVsLayer = 1u << 1,
  kVsSourceRate = 1u << 2,   // source-rate kind: core resamples
  kVsFadeIn = 1u << 3,
  kVsSourceStarted = 1u << 4,
  kVsReleased = 1u << 5,     // source released
  kVsWasReal = 1u << 6,      // RunEventLogic's view of the previous block's realSlot
  kVsPaused = 1u << 7,
  kVsMusic = 1u << 8,
  kVsStopped = 1u << 9,      // stop requested (fade then end)
  kVsCtlPending = 1u << 10,  // a pause / resume waits for the end of the hold (D-43)
  kVsWantPaused = 1u << 11,  // the state to send then
  kVsVirtualized = 1u << 12, // SelectVoices sent Virtualize; Devirtualize on the next promotion
  // Stage 3 (group 4).
  kVsRestartReturn = 1u << 13,  // D-140: promoted with "restart": Render starts it at startFrame (SelectVoices sets, Render clears)
  kVsKillVirtual = 1u << 14,    // D-140: went virtual with "never": RunEventLogic ends it (SelectVoices sets)
  kVsFadeInHold = 1u << 15,     // D-146: the play's fade-in does not advance (pausing / stopping; RunEventLogic)
};
// Static per-voice values set by RunEventLogic at creation; read by
// EvaluateDspParams, SelectVoices, BuildRenderList and Render (proposed schema
// columns: Voice.clipGainDb, Voice.pitchCents, Voice.pan, Voice.channels ...).
struct VoiceStatic {
  std::uint32_t generation;  // Voice row generation
  std::uint32_t flags;       // VoiceStaticFlags
  std::uint32_t channels;    // source channels
  std::uint32_t categorySlot;
  std::uint32_t eventRow;
  std::uint32_t trackRow;
  float gainDb;              // clip volume + random volume
  float pitchCents;          // clip pitch + random pitch
  float pan;                 // random pan [-1, 1]
  float lowpassHz;           // random filter (lowpass target), 0 = none
  float linearGain;          // synth / user buffer volume
  float sourceRate;          // source-rate kinds: frames per second of the source
  float musicGain;           // music SetGain target (1 default)
  float musicGainStep;       // per block, until musicGainTarget
  float musicGainTarget;
  float fadeInSeconds;       // clip fade-in
  std::uint32_t nextInPlayback;  // voice list of a playback
  std::uint32_t musicTag;
  ClockSample firstClock;    // creation clock (oldest steal)
  std::uint64_t startFrame;  // first source frame (Render initialises Voice.playhead with it)
  SlotIndex sourceSlot;      // copy of Voice.sourceSlot for SelectVoices (not a declared read)
  std::uint32_t sourceKind;
  // 1.2 (D-90): transport a synth voice follows (generation 0 = none), and its position
  // at the block start in quarter notes (RunEventLogic, read by Render for RenderItem).
  HandleRef followTransport;
  double transportBeats;
  double transportBeatsPerSample;
  // Stage 3 (group 4).
  std::uint32_t virtualReturn;   // D-140: 0 continue, 1 restart, 2 never (Event value when the asset sets none)
  std::uint32_t markerCount;     // markers of the clip (CoreDesc::clipMarkers until a bank column, markers.hpp)
  // D-142 / CLIP.fadeOutMs: finite asset clips. playFrames = source frames from startFrame to
  // the end (the asset end, or the end of the loop's last pass); 0 = endless (or not an asset).
  std::uint64_t playFrames;
  std::uint64_t loopStartFrame, loopEndFrame;  // loop region of a looping asset clip (loopEnd 0 = none)
  float endFadeSeconds;          // CLIP.fadeOutMs: linear fade over the last part (source time)
  float reserved4;
  const std::uint64_t* markerFrames;  // [markerCount] source frames (bank memory)
  const Id* markerIds;                // [markerCount]
  std::uint32_t fadeInFrames;   // D-146: the play's fade-in (frames of played time from the voice's start), 0 none
  std::uint32_t reserved5;
};

struct StartBatch {  // RunEventLogic: Start() requests per kind, issued once per block
  source::SourceStartRequest* req = nullptr;
  source::SourceStartResult* res = nullptr;
  std::uint32_t count = 0;
};

// Work RunEventLogic hands to the next block's ApplyCommands (rows it may not create:
// child playbacks of action tracks / music PlayEvent, parameter and snapshot actions).
enum class DeferredKind : std::uint32_t { Play = 0, StopEvent = 1, SetParam = 2, ApplySnapshot = 3 };
struct Deferred {
  DeferredKind kind;
  Id id;
  HandleRef handle;    // Play: the child playback; SetParam: the owner playback
  EventRef ref;        // Play
  SlotIndex emitter;   // Play
  std::uint32_t emitterGen;
  float value;
  std::uint32_t reserved;
  ClockSample startClock;  // Play: start at this sample (CLOCK_SAMPLE) when still ahead; 0 = now
};
inline constexpr std::uint32_t kVoicePausedFlag = 1u << 4;  // core-internal VoiceLifecycle.flags bit (paused)

// D-145: actions taken ahead, waiting for their block (cancelled if the parent pauses,
// stops or does not start before they happen). RunEventLogic private.
enum class ActionPendingKind : std::uint32_t { Play, Stop, SetParam, Snapshot, Done };
enum class ActionParentState { Running, Paused, Stopped };
struct ActionPending {
  ActionPendingKind kind;
  Id target;
  HandleRef parent;
  HandleRef child;         // Play: the child playback
  float value;
  std::uint32_t reserved;
  ClockSample clock;       // the action's clock
  ClockSample due;         // SetParam / Snapshot: the block start it applies at
};

struct EventPriv {
  PlaybackPriv* pb = nullptr;      // [maxPlaybacks]
  VoiceStatic* vs = nullptr;       // [maxVirtual] (read by later systems, see above)
  base::IndexFreeList voiceFree;
  std::uint32_t* groupLive = nullptr;  // [groupCap] live voices per limit group
  StartBatch start[static_cast<std::size_t>(SourceKind::Count)];
  SlotIndex* releaseSlots = nullptr;   // per kind scratch [maxVirtual]
  source::SourceControlOp* controls = nullptr;  // [maxVirtual]
  std::uint32_t* controlKind = nullptr;
  std::uint32_t controlCount = 0;
  music::MusicInput* musicInputs = nullptr;
  std::uint32_t musicInputCount = 0, musicInputCap = 0;
  music::MusicVoiceRequest* musicRequests = nullptr;
  music::MusicVoiceResult* musicResults = nullptr;
  std::uint32_t musicRequestCap = 0;
  Deferred* deferred = nullptr;  // [deferredCap]
  std::uint32_t deferredCount = 0, deferredCap = 0;
  std::uint32_t* releaseKind = nullptr;  // [maxVirtual]
  std::uint32_t releaseCount = 0;
  // Stage 3 (group 4), D-145.
  ActionPending* pending = nullptr;  // [deferredCap]
  std::uint32_t pendingCount = 0;
  std::uint32_t actionWarned = 0;    // a LIMIT_REACHED warning was posted this block
};

// ---------------------------------------------------------------------------
// EvaluateDspParams / SelectVoices / Render / Publish private
// ---------------------------------------------------------------------------
struct LogicPriv {  // EvaluateLogicParams: generation seen per row slot (new rows start at the default)
  std::uint32_t* globalGen = nullptr;
  std::uint32_t* emitterGen = nullptr;
  std::uint32_t* playbackGen = nullptr;
};

struct DspPriv {
  float* duckDb = nullptr;           // [duckCap] current reduction (dB, <= 0)
  std::uint32_t* catPlaying = nullptr;  // [catCap] voices playing per category this block
  float* catGainPrev = nullptr;
  float* pan = nullptr;  // [maxVirtual] 2D pan of the voice this block (clip random + curves), read by Render
  // Stage 3 (group 4), D-143: layer crossfade weight at the end of the last block, per voice.
  float* layerGain = nullptr;  // [maxVirtual]
};

struct SelectPriv {
  base::IndexFreeList realFree;
  std::uint32_t* voiceGen = nullptr;     // [realTotal] generation of the voice in the real slot
  std::int32_t* fadeRemaining = nullptr; // [realTotal] frames left of a steal fade
  std::uint32_t fadingCount = 0;
  std::uint32_t assignSeq = 0;
};

// Per-item render inputs (chunk-owned, written in Render's serial part).
struct ItemInput {
  std::uint32_t kind;        // SourceKind
  std::uint32_t flags;       // ItemFlags
  std::uint32_t channels;    // source channels
  std::uint32_t outChannels; // bus channels
  std::uint32_t segment;     // accumulator segment within the chunk
  std::uint32_t frameOffset; // first frame written (sample-accurate start)
  std::uint32_t frameEnd;    // frames after this are silent (sample-accurate stop)
  float ratio0, ratio1;      // source-rate: source frames per output frame
  float gain0, gain1;        // total gain ramp across [frameOffset, blockFrames)
  FilterTarget filter;
  std::uint32_t filterActive;
  std::uint32_t releaseActive;  // the voice releases: per-sample envelope below
  std::int64_t releaseStop;     // lifecycle.stopClock - block clock (frames, may be past the block)
  float releaseFrames;          // release length in frames
  std::uint32_t fadeActive;     // the assignment fade (D-15) moves this block: per-sample below
  float fade0;                  // assignment fade gain at the block start
  float fadeDelta;              // change per frame (+ fade in, - steal fade out), clamped to [0, 1]
  PanMatrix pan;             // target matrix (unscaled) at block end
  // Outputs (chunk writes):
  float tail[kMaxChannels];  // last output frame per bus channel (late-chunk fade, D-15)
  VoiceRenderStatus status;
  std::uint32_t vfxSlot;     // D-132: voice-effect slot + 1 (0 = no heavy voice effect this block)

  // Stage 3 (group 4), D-142 / CLIP.fadeOutMs: the clip's end inside this block (frameEnd
  // is the cut; the chunk reports `ended`), and the linear fade toward it from endFadeFrom.
  std::uint32_t clipEnds;
  std::uint32_t endFadeActive;
  std::uint32_t endFadeFrom;
  float endFade0, endFadeDelta;
  // D-146: the play's fade-in, per sample from fadeInFrom: min(1, fadeIn0 + fadeInDelta (f - from)).
  std::uint32_t fadeInActive;
  std::uint32_t fadeInFrom;
  double fadeIn0, fadeInDelta;
};
enum ItemFlags : std::uint32_t {
  kItemSourceRate = 1u << 0,
  kItemInitPan = 1u << 1,  // first block of this assignment: no pan ramp
};

struct ChunkSeg {
  std::uint32_t bus;
  std::uint32_t channels;
};
struct ChunkInfo {
  std::uint32_t firstItem;
  std::uint32_t itemCount;
  std::uint32_t segCount;
  std::uint32_t reserved;
};
struct RenderSet {
  source::RenderItem* items = nullptr;  // [realTotal]
  ItemInput* inputs = nullptr;          // [realTotal]
  ChunkInfo* chunks = nullptr;          // [maxChunks]
  ChunkSeg* segs = nullptr;             // [maxChunks * voicesPerChunk]
  float* accum = nullptr;               // [maxChunks * voicesPerChunk * kMaxChannels * blockFrames]
  std::uint32_t itemCount = 0;
  std::uint32_t chunkCount = 0;
  std::uint32_t inUseLate = 0;          // a late chunk of this set may still run
  // Chunk context (Impl*, this set, block info). Lives with the set, never on the
  // audio thread's stack: a late chunk reads it after Render returned (D-43).
  alignas(16) std::uint8_t chunkCtx[64] = {};
};

struct RenderPriv {
  std::uint8_t* resamplerPool = nullptr;  // [realTotal] slots of resamplerSlotBytes (D-44)
  std::uint32_t* lastAssign = nullptr;    // [realTotal] assignSeq seen at the last init
  std::uint32_t* stateChannels = nullptr; // [realTotal]
  float* fade = nullptr;                  // [realTotal] assignment fade gain at the end of the last block
  float* tail = nullptr;                  // [realTotal * kMaxChannels] last output (D-15 fade of held voices)
  std::uint32_t* tailBus = nullptr;       // [realTotal]
  std::uint32_t* tailChannels = nullptr;
  std::uint32_t* tailFadeLeft = nullptr;  // [realTotal] frames left of the D-15 tail fade (may span blocks)
  float* tailFade = nullptr;              // [realTotal * kMaxChannels] the faded values (tail at the hold)
  std::uint32_t* tailFadeBus = nullptr;   // [realTotal]
  std::uint32_t* tailFadeChannels = nullptr;
  std::uint32_t* busGen = nullptr;        // [busCap] generation of the bus row seen (new rows: reset state)
  std::uint32_t* voiceGen = nullptr;      // [maxVirtual] generation of the voice row initialised
  std::uint64_t* holdBlock = nullptr;     // [maxVirtual] block whose chunk was late (hold start)
  RenderSet sets[2];
  std::uint32_t lastParallelSet = 1;
  std::uint32_t currentSet = 0;
  std::uint32_t parallel = 0;
  float* busBuf = nullptr;                // [busCap * kMaxChannels * blockFrames]
  std::uint32_t* busOrder = nullptr;      // [busCap] processing order (children, sends first)
  std::uint32_t busOrderCount = 0;
  dsp::PeakMeterState* peak = nullptr;    // [busCap]
  dsp::RmsMeterState* rms = nullptr;      // [busCap]
  dsp::LoudnessMeterState* loudness = nullptr;  // master only
  std::uint32_t masterSlot = kNoSlot;
  float* outBuf = nullptr;                // [kMaxChannels * blockFrames] master output
  std::uint32_t lateTotal = 0;
  std::uint32_t lod = 0;
  // Stage 3 (group 4), D-147 / D-148: loudness and spectrum of the output (meter 0) and of
  // the buses with kBusMeter (meters 1..). analysis.cpp.
  struct MeterSlot* meters = nullptr;      // [meterCount]
  std::uint32_t meterCount = 0;            // CoreLimits.maxMeteredBuses + 1
  std::uint32_t* busMeter = nullptr;       // [busCap] meter of the bus slot, kNoSlot none
  struct AnalysisShared* analysis = nullptr;
  // D-146: played frames of each voice (fade-in); markers / "continue": the resampler's
  // look-ahead (source frames read ahead of the output) at the voice's last render.
  std::uint64_t* voicePlayed = nullptr;   // [maxVirtual]
  double* voiceLookahead = nullptr;       // [maxVirtual]
};

struct TransportPriv {  // core's UpdateTransport when no music system is present
  std::uint64_t* seenSeq = nullptr;  // [maxReservations] Reservation.sequence initialised
  std::uint32_t* transportGen = nullptr;  // [maxTransports] row generation initialised
};
// kFireTransportGone (FireEntry::lateRuleApplied for a destroyed transport): snd/core/core.hpp

struct PublishPriv {
  std::uint32_t* streamStall = nullptr;   // [maxStreams] Stream.stallCount seen
  std::uint32_t* streamGen = nullptr;     // [maxStreams] row generation it belongs to
  std::uint32_t* transportGen = nullptr;  // [maxTransports] generation seen live last block
  std::uint32_t* playbackGen = nullptr;   // [3 * maxPlaybacks] generation written per snapshot frame
};

// Frame lists (one block; the producer system clears them).
struct EndedPlayback {
  SlotIndex slot;
  std::uint32_t generation;
  std::uint32_t reason;
  std::uint32_t detail;
};
struct Released {
  SlotIndex slot;
  std::uint32_t generation;
};

}  // namespace snd::core

// ===========================================================================
// System::Impl
// ===========================================================================
namespace snd::core {

struct System::Impl {
  CoreDesc desc{};
  snd_config config{};
  Cfg cfg{};
  IAllocator* alloc = nullptr;
  std::uint64_t serial = 0;

  // Allocation log (freed in reverse order by Destroy).
  struct Allocation {
    void* p;
    MemTag tag;
    std::size_t bytes;
  };
  Allocation* allocations = nullptr;
  std::uint32_t allocationCount = 0, allocationCap = 0;

  // --- game side ---------------------------------------------------------------
  Producer* producers = nullptr;  // [maxThreads]
  base::HandleAllocator playbackSlots, emitterSlots, transportSlots, userBufferSlots;
  std::atomic<std::uint8_t>* oneShot = nullptr;  // [maxPlaybacks]
  base::MpmcRing<RejectedPlay> rejected;
  std::atomic<std::uint64_t>* eventPriority = nullptr;  // [eventPriorityEntries] (id << 32) | priority
  std::uint32_t eventPriorityMask = 0;
  source::UserBufferControl* userBuffers = nullptr;  // [maxUserBuffers]
  snd_callbacks callbacks{};
  std::atomic<std::uint32_t> rejectedCommands{0};
  std::atomic<std::uint32_t> reserveOverflows{0};
  std::atomic<std::uint32_t> fallbackCommands{0};  // coalesced into the fallback (stats.fallback_commands)
  base::Mutex gameMutex;  // language switches, pending registrations (non-RT threads only)
  bk::LanguageSwitch* languagePlan = nullptr;  // [kMaxLanguageSwitches]
  struct PendingSwitch {
    HandleRef oldBank;
    HandleRef newBank;
  };
  PendingSwitch* switches = nullptr;  // [kMaxLanguageSwitches]
  std::uint32_t switchCount = 0;
  HandleRef* draining = nullptr;      // [kMaxLanguageSwitches] old language banks unloaded when unused
  std::uint32_t drainingCount = 0;
  cmd::BankRegister* pendingRegs = nullptr;  // [maxBanks] registrations not recorded yet (arena full)
  std::uint32_t pendingRegCount = 0;
  BankRecord** records = nullptr;  // [maxBanks] written by the owner thread, read by ApplyCommands
  struct ReleaseCandidate {  // unregistered banks waiting for source jobs (owner thread)
    std::uint32_t slot, generation, ticket, used;
  };
  ReleaseCandidate* releaseCandidates = nullptr;  // [maxBanks]

  // --- banks ---------------------------------------------------------------------
  bk::BankManager* banks = nullptr;
  notify::BankSlotState* bankStates = nullptr;
  base::SpscRing<BankEvent> bankEvents;  // audio -> owner
  std::atomic<std::uint64_t> dspReserved{0};

  // --- services ------------------------------------------------------------------
  base::MpmcRing<PostedJob> jobQueue;
  base::MpmcRing<source::IoReadRequest> ioQueue;
  bool servicesReady = false;  // the queues above are initialised (a failed Create may stop before)
  JobWrap* jobWraps = nullptr;
  base::ConcurrentIndexFreeList jobWrapFree;
  std::atomic<std::uint32_t> jobEpoch{0};
  std::atomic<std::uint32_t> jobsInFlight[2];
  std::atomic<std::uint32_t> droppedJobs{0}, droppedIo{0};
  std::atomic<std::uint32_t> trampolinesInFlight{0};  // submitted wrapped jobs not finished (Destroy waits)
  base::Mutex serviceMutex;  // serialises PumpServices (epoch flips)
  source::SourceServices services{};

  // --- notifications ---------------------------------------------------------------
  notify::EndFlags endFlags{};
  notify::ReleaseFlags emitterRelease{}, transportRelease{}, userBufferRelease{};
  notify::EventRing* eventRing = nullptr;
  notify::SnapshotTripleBuffer* triple = nullptr;

  // --- audio side ------------------------------------------------------------------
  schema::Tables tables{};
  BlockIndex block = 0;
  ClockSample clock = 0;
  base::LinearArena frameArena;
  jobs::RtWorkerPool rt;
  bool blockQuiescent = true;
  source::ISourceKind* kinds[static_cast<std::size_t>(SourceKind::Count)] = {};
  music::IMusicSystem* music = nullptr;
  music::FireList fire{};
  music::MusicFrame musicFrame{};

  // Shared definitions (A1-16), written by ApplyCommands.
  BusDef* busDefs = nullptr;
  CatDef* catDefs = nullptr;
  SnapDef* snapDefs = nullptr;
  GroupDef* groupDefs = nullptr;
  ParamDef* paramDefs = nullptr;
  DuckRule* duckRules = nullptr;
  IdMap busMap, catMap, snapMap, groupMap, paramDefMap;
  base::IndexFreeList busFree, catFree, snapFree, groupFree, paramDefFree, duckFree;
  std::uint32_t globalCategory = kNoSlot;  // Category row of SND_ID_NONE (D-30)

  ApplyPriv ap;
  LogicPriv lp;
  EventPriv ev;
  DspPriv dp;
  SelectPriv sel;
  RenderPriv rd;
  TransportPriv tp;
  PublishPriv pub;

  // Frame lists.
  EndedPlayback* ended = nullptr;      // [maxPlaybacks] RunEventLogic -> Publish
  std::uint32_t endedCount = 0;
  Released* releasedEmitters = nullptr;     // [maxEmitters] ApplyCommands -> Publish
  std::uint32_t releasedEmitterCount = 0;
  Released* releasedUserBuffers = nullptr;  // [maxUserBuffers]
  std::uint32_t releasedUserBufferCount = 0;
  notify::EventRecord* events = nullptr;    // [eventRingCapacity] any system -> Publish
  std::uint32_t eventCount = 0, eventCap = 0;

  // Statistics (audio thread).
  snd_stats stats{};
  // IOutputDevice::OutputLatencySeconds; CoreDesc's value until System::SetOutputLatency.
  std::atomic<double> outputLatency{0.0};
  BlockProfile profile{};
  std::uint64_t timeSumNs = 0;
  std::uint32_t timeSamples = 0;
  std::uint32_t lodOver = 0, lodUnder = 0;
  std::uint32_t streamStalls = 0;

  // Render(out, frames) FIFO.
  float* fifo = nullptr;  // [blockFrames * outChannels]
  std::uint32_t fifoPos = 0, fifoFill = 0;

  // Stage 3 group 2 (fx_control.hpp): voice effects (D-132..D-134).
  VoiceFxPriv vfx;
};

// ---------------------------------------------------------------------------
// Shared helpers (core_util.cpp)
// ---------------------------------------------------------------------------
using Impl = System::Impl;

// Event records for Publish (dropped and counted when the frame list is full).
void PostEvent(Impl& im, notify::EventType type, std::uint32_t a, std::uint64_t object, std::uint16_t detail = 0,
               std::uint32_t b = 0) noexcept;
inline void PostWarning(Impl& im, snd_result code, std::uint32_t detail, std::uint64_t object) noexcept {
  PostEvent(im, notify::EventType::Warning, static_cast<std::uint32_t>(code), object,
            static_cast<std::uint16_t>(detail));
}
inline void PostError(Impl& im, snd_result code, std::uint32_t detail, std::uint64_t object) noexcept {
  PostEvent(im, notify::EventType::Error, static_cast<std::uint32_t>(code), object,
            static_cast<std::uint16_t>(detail));
}

// Live link (live_link.cpp): event of the playback whose voice reads stream slot `slot`.
Id LiveStreamEventOf(const Impl& im, std::uint32_t slot) noexcept;

// Bank lookups (audio thread).
const BankRecord* RegisteredBank(const Impl& im, std::uint16_t bankSlot) noexcept;
EventRef ResolveEvent(const Impl& im, Id event) noexcept;

// Curve evaluation (CURV/CPNT of `bank`), x clamped to the end points.
float EvaluateCurve(const bk::BankView& v, std::uint32_t curveRow, float x) noexcept;

// Deterministic random (xorshift32); returns [0, 1).
inline float NextRandom(std::uint32_t& state) noexcept {
  std::uint32_t x = state != 0u ? state : 0x9E3779B9u;
  x ^= x << 13;
  x ^= x >> 17;
  x ^= x << 5;
  state = x;
  return static_cast<float>(x >> 8) * (1.0f / 16777216.0f);
}

inline float DbToGain(float db) noexcept { return dsp::DbToGain(db); }

// Value of a TimedRamp at `clock`.
inline float RampValue(const TimedRamp& r, ClockSample clock) noexcept {
  if (r.durationSamples == 0u || clock >= r.startClock + r.durationSamples) return r.to;
  if (clock <= r.startClock) return r.from;
  const float t = static_cast<float>(clock - r.startClock) / static_cast<float>(r.durationSamples);
  return r.from + (r.to - r.from) * t;
}

// Systems (one per file, design 3.2 order).
void ApplyCommands(Impl& im) noexcept;
void UpdateTransport(Impl& im) noexcept;
void EvaluateLogicParams(Impl& im) noexcept;
ActionParentState ActionParentStatus(Impl& im, HandleRef parent, ClockSample at) noexcept;
void ApplyDueActions(Impl& im) noexcept;
void RunEventLogic(Impl& im) noexcept;
void Compute3D(Impl& im) noexcept;
void EvaluateDspParams(Impl& im) noexcept;
void SelectVoices(Impl& im) noexcept;
void BuildRenderList(Impl& im) noexcept;
void Render(Impl& im) noexcept;
void MixBuses(Impl& im) noexcept;  // part of Render (serial)
void Publish(Impl& im) noexcept;

// Bank registration (ApplyCommands) and its owner-thread preparation.
bool PrepareBankRecord(Impl& im, const cmd::BankRegister& reg) noexcept;  // owner thread
void FreeBankRecord(Impl& im, std::uint32_t slot) noexcept;               // owner thread
void ApplyBankRegister(Impl& im, const cmd::BankRegister& reg) noexcept;  // audio thread
void UnregisterBank(Impl& im, std::uint32_t slot) noexcept;               // audio thread
void RebuildBusGraph(Impl& im) noexcept;                                  // audio thread

// Game-side producer helpers.
std::uint32_t CurrentProducer(const Impl& im) noexcept;  // kNoSlot when not registered
std::uintptr_t ThreadToken() noexcept;  // identity of the calling thread (never 0)
bool IsOwnerThread(const Impl& im) noexcept;
bool TlsRegister(const Impl& im, std::uint32_t producer) noexcept;
void TlsUnregister(const Impl& im) noexcept;

// Row removal moving every column of the swapped row (row lifecycle, not a column write).
base::rows::RemoveResult RemovePlaybackRow(schema::Tables& t, SlotIndex slot) noexcept;
base::rows::RemoveResult RemoveVoiceRow(schema::Tables& t, SlotIndex slot) noexcept;
base::rows::RemoveResult RemoveEmitterRow(schema::Tables& t, SlotIndex slot) noexcept;
base::rows::RemoveResult RemoveParamGlobalRow(schema::Tables& t, SlotIndex slot) noexcept;
base::rows::RemoveResult RemoveParamEmitterRow(schema::Tables& t, SlotIndex slot) noexcept;
base::rows::RemoveResult RemoveParamPlaybackRow(schema::Tables& t, SlotIndex slot) noexcept;
base::rows::RemoveResult RemoveBusRow(schema::Tables& t, SlotIndex slot) noexcept;
base::rows::RemoveResult RemoveTransportRow(schema::Tables& t, SlotIndex slot) noexcept;
base::rows::RemoveResult RemoveReservationRow(schema::Tables& t, SlotIndex slot) noexcept;
base::rows::RemoveResult RemoveCategoryRow(schema::Tables& t, SlotIndex slot) noexcept;
base::rows::RemoveResult RemoveSnapshotRow(schema::Tables& t, SlotIndex slot) noexcept;

// ---------------------------------------------------------------------------
// Stage 3 group 2: runtime effect control (fx_control.cpp, voice_fx.cpp)
// ---------------------------------------------------------------------------
// D-130. Audio thread, ApplyCommands or a later system of the same block before Render.
// Effect instance `effectIndex` of the chain of bus `busSlot` (in its defining bank), or null.
EffectInstance* BusEffectInstance(Impl& im, SlotIndex busSlot, std::uint32_t effectIndex) noexcept;
// Sets the value parameter `param` heads to: from its current set value at the block start,
// linearly over `fadeSeconds` (0: this block). Clamped to the parameter range (standard
// effects); parameters past the instance's count are ignored. Contributions
// (EffectParamControl::contrib) add on top.
void SetEffectParam(Impl& im, EffectInstance& e, std::uint32_t param, float value, float fadeSeconds) noexcept;
// True for a standard effect's [init] parameter (kParamInitOnly): not changeable at run time.
bool EffectParamIsInitOnly(const snd_dsp_effect* plugin, Id type, std::uint32_t param) noexcept;
// ApplyCommands: event id of a live playback (0 if none).
Id PlaybackEventId(Impl& im, HandleRef playback) noexcept;
// Clamps `v` to parameter `param`'s range (standard effects; plugins: unchanged).
float ClampEffectParam(const EffectInstance& e, std::uint32_t param, float v) noexcept;
// Render (MixBuses): this block's ParamBlock values of `e` from its controls; returns the mask.
std::uint32_t StepEffectParams(Impl& im, EffectInstance& e) noexcept;
// ApplyCommands: SetBusVolume variant 1 (bus effect parameter, D-130).
void ApplyBusEffectParam(Impl& im, const cmd::SetBusVolume& c) noexcept;
// D-131 (MixBuses): sidechain source of bus `s` (defining bank `rec`) for this block.
struct SidechainSource {
  const float* channels[kMaxChannels];
  std::uint32_t count;
  std::uint32_t prevRow;  // DUCK row whose previous-block buffer the source refreshes after its mix (kNoSlot: none)
};
bool FindSidechain(Impl& im, const BankRecord& rec, SlotIndex s, std::uint32_t orderIndex, SidechainSource* out) noexcept;
// After bus `s` was mixed: refresh the previous-block buffers that take it as their source.
void StoreSidechainSources(Impl& im, SlotIndex s, std::uint32_t orderIndex, const float* buf, std::uint32_t ch) noexcept;

// D-132..D-134 (voice_fx.cpp).
bool InitVoiceFx(Impl& im) noexcept;  // Create
void ApplyVoiceFxSwitch(Impl& im, const cmd::SetSwitch& c) noexcept;  // SetSwitch variant 1
void ApplyVoiceFxParam(Impl& im, const cmd::SetParam& c) noexcept;    // SetParam variant 1
// Render, serial, before the items: frees the slots of voices that left their real assignment.
void VfxReleaseStale(Impl& im) noexcept;
// Render, serial, per item: binds / keeps a slot and writes this block's parameters (in.vfxSlot).
void VfxPrepareItem(Impl& im, const source::RenderItem& it, ItemInput& in) noexcept;
// Chunk: runs the voice's effect chain in place.
void VfxProcess(Impl& im, std::uint32_t slot, float* const* voiceOut, std::uint32_t channels) noexcept;

// --- Stage 3 (group 4) -------------------------------------------------------------------
// Playback time (frames) of an action at ACTN.timeMs = `ms` (sys_event.cpp, D-145).
std::uint64_t ActionFrame(const Impl& im, float ms) noexcept;
// Analysis (analysis.cpp, D-147 / D-148): meters of the output and of kBusMeter buses.
bool InitAnalysis(Impl& im) noexcept;                       // Create
void AnalysisMixBus(Impl& im, SlotIndex bus, std::uint32_t d, const float* const* chans, std::uint32_t channels) noexcept;  // Render (MixBuses)
void AnalysisMixOutput(Impl& im) noexcept;                  // Render (MixBuses, after the buses)
void AnalysisMaintain(Impl& im) noexcept;                   // Render (MixBuses, before the buses)
bool AnalysisLoudness(const Impl& im, SlotIndex bus, float* shortTerm, float* integrated) noexcept;  // Publish
void AnalysisPublish(Impl& im) noexcept;                    // Publish
void AnalysisSwap(Impl& im) noexcept;                       // Update (owner thread)

}  // namespace snd::core

#endif  // SND_CORE_SRC_CORE_IMPL_HPP
