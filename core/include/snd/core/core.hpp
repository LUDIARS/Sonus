// core (design.md 2, 3, 4; 12.1): tables, commands, the block systems, voice
// management, the bus graph, 3D (basic), parameters and categories.
//
// One snd::core::System per runtime. It owns every table of the schema
// (snd/internal/schema.hpp), the command queues of the registered threads,
// the notification channels (snd/internal/notify.hpp), the RT workers and the
// bank manager. Everything is allocated in Create() from the IAllocator; after
// Create() the audio-thread path (RenderBlock / Render) never allocates, locks
// or does IO (design 1.2 principle 1).
//
// Threads (sonus.h "Threads"):
//   owner        the thread that called Create(). Update(), SetCallbacks(),
//                Destroy() and GetBankChunk() only there.
//   registered   the owner and every thread that called RegisterThread().
//                Recording functions (Play, Stop, SetParam, ...) record a
//                command into the calling thread's arena and return at once.
//                UnregisterThread() while the thread's arena cannot be published
//                yet retires its producer (the owner's Update publishes it later).
//                The same thread registering again before that takes the same
//                producer back and continues its arena, so its commands stay in
//                order. "The same thread" is told by a per-thread number core
//                takes from a process-wide counter on the thread's first
//                RegisterThread (thread_local): never reused, so a later thread
//                (even one that gets the same OS thread id) is a different thread.
//   any          queries (GetPlaybackState, GetStats, ...), PumpServices().
//   audio        RenderBlock() / Render(): the device driver (platform) or a
//                test drives them; one block at a time, never concurrently.
//
// Driving (design 2.2): RenderBlock(out) renders exactly one block of
// Config.blockSize frames into `out` (interleaved float32, OutputChannels()
// channels). Render(out, frames) renders any number of frames through an
// internal FIFO of one block (for device periods that are not a multiple of
// the block). The device and the audio thread are not part of core.
//
// Services (D-08): source kinds post jobs and stream reads through lock-free
// queues. PumpServices() (any non-RT thread; Update() calls it too) submits
// them to the IJobSystem / CoreDesc::submitIo. A host whose game loop may
// stall should call PumpServices() from a service thread as well.
//
// Banks (10.3): Create() makes the bank::BankManager (Scheduler mode when
// CoreDesc::bankScheduler is set, DirectFileSystem mode otherwise) with core's
// DSP hooks (D-54: dsp::ComputeDspPoolNeed with Config.plugins.effects).
// Update() pumps it, records the registrations, and releases unloaded banks
// once core's epoch says nobody references them (block boundary passed, RT
// workers quiescent, source jobs posted before the unregistration finished).
#ifndef SND_CORE_CORE_HPP
#define SND_CORE_CORE_HPP

#include <cstdint>

#include "snd/config.h"
#include "snd/ext/allocator.hpp"
#include "snd/ext/codec.hpp"
#include "snd/ext/file_system.hpp"
#include "snd/ext/job_system.hpp"
#include "snd/format/livelink_protocol.hpp"
#include "snd/internal/commands.hpp"
#include "snd/internal/music_if.hpp"
#include "snd/internal/notify.hpp"
#include "snd/internal/source_if.hpp"
#include "snd/types.h"

namespace snd::io {
class IoScheduler;  // io module (bank's Scheduler mode); opaque to core's users
}  // namespace snd::io

namespace snd::core {

// Conventions between core and the music system (music_if.hpp) that the
// frozen contract does not spell out (stage-4 proposals, see the music report):
//  - FireEntry::lateRuleApplied == kFireTransportGone: the reservation's
//    transport was destroyed (Play ends REJECTED / TRANSPORT_GONE, stops are
//    dropped). Written by whichever UpdateTransport runs (core's minimal one or
//    the music system's).
//  - ClipRef::trackIndex == kClipRefAssetTrack: clipIndex is an ASST row (music
//    segments and stems are assets, not CLIP rows), for StartClip and
//    MusicServices::primeFrames.
//  - MusicVoiceRequest::fadeSeconds of StartClip is the clip's fade-in;
//    startClock of StopVoice is the sample the release starts at (0 = now).
// Contract 1.2 moved the definitions to music_if.hpp (D-82); the names stay here.
inline constexpr std::uint8_t kFireTransportGone = music::kFireTransportGone;
inline constexpr std::uint16_t kClipRefAssetTrack = music::kClipRefAssetTrack;

// 1.2 (D-104): reserved parameter ids of the per-playback volume and pitch
// (snd_set_playback_volume / _pitch, snd_play_user_buffer_ex). They travel as
// playback-scope cmd::SetParam (must-deliver, reserve and coalescing fallback
// included); ApplyCommands stores them per playback instead of a parameter row,
// EvaluateDspParams multiplies every voice of the playback by them. Next to
// bank_format's kInputDistance (0xFFFFFFFE). Other scopes ignore them.
inline constexpr Id kParamPlaybackVolume = 0xFFFFFFFDu;
inline constexpr Id kParamPlaybackPitch = 0xFFFFFFFCu;
static_assert(kParamPlaybackPitch == SND_ID_RESERVED_FIRST && kParamPlaybackVolume < SND_ID_RESERVED_LAST);

// Capacities that Config does not have (proposed as Config fields, see the
// stage-2 report). Shared objects are unified by id across banks (A1-16), so
// these count distinct ids of the whole project that are loaded at once.
struct CoreLimits {
  std::uint32_t maxBuses = 128;          // + Config.dev_reserve.buses
  std::uint32_t maxCategories = 64;      // + 1 row for "all categories" (D-30)
  std::uint32_t maxSnapshots = 64;
  std::uint32_t maxLimitGroups = 64;     // VLGR ids
  std::uint32_t maxParamDefs = 512;      // PARM ids
  std::uint32_t maxDuckRules = 64;
  std::uint32_t eventPriorityEntries = 4096;  // game-side EVNT.priority lookup (D-10)
  std::uint32_t maxVoicesPerPlay = 64;   // voices one Play may create (nested events included)
  std::uint32_t serviceQueueEntries = 1024;   // postJob / postIo queues
  std::uint32_t maxLiveOverrides = 32;
};

// Called on the audio thread (ApplyCommands) for live-link commands that core
// does not interpret itself (LiveSetValue, LiveReplaceChunk). Principle 1 applies.
using LiveCommandFn = void (*)(void* user, cmd::CommandType type, const void* payload) noexcept;

class System;

// --- Live link taps (11.2 profiler, 11.3 recording; dev builds) ----------------------
// Called on the audio thread. Principle 1 applies: copy into preallocated memory
// and return. Null members are skipped. The live link module installs them.
struct LiveArenaView {
  std::uint32_t producer = 0;      // registration index; kInvalidSlot for replayed arenas
  std::uint32_t flags = 0;         // cmd::ArenaFlags
  BlockIndex appliedBlock = 0;     // block whose ApplyCommands consumes the arena (D-12)
  const std::uint8_t* normal = nullptr;
  std::uint32_t normalBytes = 0;
  std::uint32_t reserveBytes = 0;
  const std::uint8_t* reserve = nullptr;
  const cmd::OverflowFallback* fallback = nullptr;  // non-null only with kArenaHasFallback
};
struct LiveTap {
  void* user = nullptr;
  // ApplyCommands, for every producer arena, before its commands are applied.
  void (*arena)(void* user, const LiveArenaView& arena) noexcept = nullptr;
  // ApplyCommands, block start, before the producers: arenas of a replay (11.3). Called
  // until it returns false; each returned arena is applied like a producer arena
  // (normal, reserve, fallback). The memory must stay valid until the next call.
  bool (*replay)(void* user, BlockIndex block, LiveArenaView* next) noexcept = nullptr;
  // Every event record a system posts this block (dropped ones too).
  void (*event)(void* user, const notify::EventRecord& record) noexcept = nullptr;
  // Publish: a playback ended (slot, generation, snd_end_reason, snd_end_detail).
  void (*ended)(void* user, std::uint32_t slot, std::uint32_t generation, std::uint32_t reason,
                std::uint32_t detail) noexcept = nullptr;
  // Publish: stream slot `slot` stalled `count` more times in this block (6.3). eventId: the
  // event of the playback whose voice reads the stream (0 if no voice reads it any more).
  void (*stall)(void* user, std::uint32_t slot, Id eventId, std::uint32_t count, BlockIndex block) noexcept = nullptr;
  // ApplyCommands, before core applies a live link command it interprets itself
  // (LiveOverride, LiveClearOverrides): false skips it (the live link drops the edits
  // of an earlier session, D-87). Null: always applied.
  bool (*admit)(void* user, cmd::CommandType type, const void* payload) noexcept = nullptr;
  // End of every RenderBlock (suspended blocks too). The Live* queries below may be
  // called from here.
  void (*block)(void* user, System& sys) noexcept = nullptr;
};

// One editable cell of a registered bank's override layer (11.2).
struct LiveCell {
  std::uint32_t bankSlot = 0;
  std::uint32_t row = 0;
};
struct LiveBankInfo {
  HandleRef handle{};
  Id bankId = 0;
  std::uint32_t languageId = 0;
  std::uint64_t projectBuildHash = 0;
  std::uint64_t contentHash = 0;
  std::uint32_t writable = 0;  // 1 = dev override layer present
  std::uint32_t reserved = 0;
};

struct CoreDesc {
  const snd_config* config = nullptr;  // required, already validated by api (struct_size, version)
  IAllocator* allocator = nullptr;     // required: every byte of the runtime
  const source::SourceKindSet* sources = nullptr;  // null: no source kinds (voices fail UNSUPPORTED)
  music::IMusicSystem* music = nullptr;            // null: musical When kinds act as IMMEDIATE + warning
  IJobSystem* jobs = nullptr;          // normal-worker jobs (bank validation, source jobs); required for banks
  io::IoScheduler* bankScheduler = nullptr;  // banks read through the IO scheduler (default mode)
  IFileSystem* bankFileSystem = nullptr;     // else: banks read by PumpIo (DirectFileSystem mode)
  // Stream reads posted by sources (D-08), submitted by PumpServices(). Null: dropped and counted.
  bool (*submitIo)(void* user, const source::IoReadRequest& request) noexcept = nullptr;
  void* ioUser = nullptr;
  // Codec lookup for sources (custom codecs first). Null: no codecs.
  ICodec* (*findCodec)(void* user, std::uint32_t fourcc) noexcept = nullptr;
  void* codecUser = nullptr;
  // Called on the audio thread (ApplyCommands, RT: principle 1) when a bank is
  // unregistered (its playbacks are gone; its memory goes back after the epoch).
  // The composition root passes source::Runtime::BankUnregistered so the PCM
  // cache drops the entries of that bank. Null: not called.
  void (*bankUnregistered)(void* user, std::uint16_t bankSlot) noexcept = nullptr;
  void* bankUser = nullptr;
  LiveCommandFn liveCommand = nullptr;
  void* liveUser = nullptr;
  LiveTap liveTap{};  // live link profiler / recording / replay hooks (dev builds)
  double outputLatencySeconds = 0.0;   // IOutputDevice::OutputLatencySeconds (8.1 notifications, stats)
  bool rtWorkerRealtimePriority = true;  // false in tests (no SCHED_FIFO)
  std::uint32_t rtDebugClaimDelayMicroseconds = 0;  // tests only (jobs A1-14)
  CoreLimits limits;
};

// Timing of the last rendered block (audio thread; tests and the profiler).
struct BlockProfile {
  std::uint64_t block = 0;
  std::uint64_t totalNs = 0;     // the whole block
  std::uint64_t logicNs = 0;     // systems 1-8
  std::uint64_t renderNs = 0;    // Render (voices + buses)
  std::uint32_t renderedVoices = 0;
  std::uint32_t chunks = 0;
  std::uint32_t lateChunks = 0;
  std::uint32_t heldVoices = 0;
  std::uint32_t parallel = 0;    // 1 = RtWorkerPool::Run, 0 = serial on the audio thread
  std::uint32_t renderSet = 0;   // render-list / accumulator set used (D-43)
};

class System {
 public:
  // Owner thread. *out = nullptr on failure.
  //   SND_ERR_INVALID_ARGUMENT  missing desc fields, a Config value out of range
  //   SND_ERR_OUT_OF_BUDGET     the allocator refused memory
  static snd_result Create(const CoreDesc& desc, System** out) noexcept;
  // Owner thread. No audio thread may be inside RenderBlock / Render.
  static void Destroy(System* sys) noexcept;

  // --- threads --------------------------------------------------------------------
  snd_result RegisterThread() noexcept;
  snd_result UnregisterThread() noexcept;
  snd_result Flush() noexcept;
  snd_result Update() noexcept;  // owner
  snd_result SetCallbacks(const snd_callbacks* callbacks) noexcept;  // owner
  void PumpServices() noexcept;  // any non-RT thread

  // --- recording (registered threads; semantics of snd/sonus.h) ----------------------
  snd_result Suspend() noexcept;
  snd_result Resume() noexcept;
  snd_bank_handle LoadBankAsync(const char* path) noexcept;
  snd_result UnloadBank(snd_bank_handle bank, snd_unload_mode mode) noexcept;
  snd_result DeclareLanguage(const char* name) noexcept;
  snd_result SetLanguage(Id language) noexcept;
  snd_playback_handle Play(Id event, snd_emitter_handle emitter, const snd_when* when) noexcept;
  snd_result PlayOneShot(Id event, snd_emitter_handle emitter, const snd_when* when) noexcept;
  snd_result PlayAt(Id event, const snd_vec3* position, const snd_when* when) noexcept;
  snd_result Stop(snd_playback_handle playback, float fadeSeconds, const snd_when* when) noexcept;
  snd_result Pause(snd_playback_handle playback, float fadeSeconds) noexcept;
  snd_result ResumePlayback(snd_playback_handle playback, float fadeSeconds) noexcept;
  snd_result SetScheduledStop(snd_playback_handle playback, const snd_when* when, float fadeSeconds) noexcept;
  snd_emitter_handle CreateEmitter() noexcept;
  snd_result DestroyEmitter(snd_emitter_handle emitter) noexcept;
  snd_result SetTransform(snd_emitter_handle emitter, const snd_vec3* position, const snd_vec3* forward,
                          const snd_vec3* velocity) noexcept;
  snd_result SetEmitterShape(snd_emitter_handle emitter, snd_emitter_shape kind, const snd_vec3* halfExtents) noexcept;
  snd_result SetReverbZone(snd_emitter_handle emitter, snd_id bus, float amount, float fadeDistance) noexcept;
  snd_result SetListener(std::uint32_t index, const snd_listener_desc* listener) noexcept;
  snd_result SetParamGlobal(Id param, float value) noexcept;
  snd_result SetParamEmitter(snd_emitter_handle emitter, Id param, float value) noexcept;
  snd_result SetParamPlayback(snd_playback_handle playback, Id param, float value) noexcept;
  snd_result SetSwitchGlobal(Id group, Id value) noexcept;
  snd_result SetSwitchEmitter(snd_emitter_handle emitter, Id group, Id value) noexcept;
  snd_result SetSwitchPlayback(snd_playback_handle playback, Id group, Id value) noexcept;
  snd_result SetBusVolume(Id bus, float volume, float fadeSeconds) noexcept;
  snd_result ApplySnapshot(Id snapshot, float weight, float timeSeconds) noexcept;
  snd_result SetCategoryVolume(Id category, float volume, float fadeSeconds) noexcept;
  snd_result PauseCategory(Id category, bool paused) noexcept;
  snd_result MuteCategory(Id category, bool muted) noexcept;
  snd_result SetMusicState(snd_playback_handle music, Id state, const snd_when* when) noexcept;
  snd_result PrepareMusicState(snd_playback_handle music, Id state) noexcept;
  snd_transport_handle CreateTransport(const snd_transport_desc* desc) noexcept;
  snd_result DestroyTransport(snd_transport_handle transport) noexcept;
  snd_result SetTransportState(snd_transport_handle transport, snd_transport_state state,
                               const snd_when* when) noexcept;
  snd_result SetTempo(snd_transport_handle transport, float bpm, const snd_when* when) noexcept;
  snd_result SeekTransport(snd_transport_handle transport, const snd_musical_time* position,
                           const snd_when* when) noexcept;
  // 1.2 (D-88, D-89): cmd::SetTempo with kTempoVariantPitch, cmd::SetMusicState with
  // kMusicStateVariantStinger (commands.hpp).
  snd_result SetTransportPitch(snd_transport_handle transport, float pitch, const snd_when* when) noexcept;
  snd_result PlayStinger(snd_playback_handle music, Id stinger, const snd_when* when) noexcept;
  snd_playback_handle PlaySynth(const snd_synth_desc* desc, snd_emitter_handle emitter,
                                const snd_when* when) noexcept;
  snd_user_buffer_handle CreateUserBuffer(const snd_user_buffer_desc* desc) noexcept;
  snd_result DestroyUserBuffer(snd_user_buffer_handle buffer) noexcept;
  snd_playback_handle PlayUserBuffer(snd_user_buffer_handle buffer, Id bus, snd_emitter_handle emitter,
                                     const snd_when* when) noexcept;
  snd_playback_handle PlayInput(const snd_input_desc* desc, snd_emitter_handle emitter,
                                const snd_when* when) noexcept;
  // Live link (11.2, dev builds): records the command with Origin::LiveLink semantics
  // from the calling registered thread (the live link thread registers itself).
  snd_result RecordLive(cmd::CommandType type, const void* payload, std::uint32_t payloadBytes) noexcept;

  // --- any thread -----------------------------------------------------------------------
  std::uint32_t WriteUserBuffer(snd_user_buffer_handle buffer, const float* const* channels,
                                std::uint32_t frames) noexcept;
  std::int32_t GetUserBufferReleased(snd_user_buffer_handle buffer) const noexcept;
  snd_playback_state GetPlaybackState(snd_playback_handle playback) const noexcept;
  snd_result GetPosition(snd_playback_handle playback, snd_position* out) const noexcept;
  snd_result GetTransportPosition(snd_transport_handle transport, snd_position* out) const noexcept;
  snd_result GetMeter(Id bus, snd_meter* out) const noexcept;
  snd_result GetStats(snd_stats* out) const noexcept;
  snd_bank_state GetBankState(snd_bank_handle bank) const noexcept;
  snd_result GetBankChunk(snd_bank_handle bank, std::uint32_t fourcc, const void** data,
                          std::uint64_t* bytes) const noexcept;  // owner

  // --- audio thread ---------------------------------------------------------------------
  void RenderBlock(float* interleaved) noexcept;
  void Render(float* interleaved, std::uint32_t frames) noexcept;
  std::uint32_t OutputChannels() const noexcept;
  // Any thread: the device's latency once it is open (CoreDesc::outputLatencySeconds is an
  // estimate taken before). Used from the next block (8.1 notification times, stats).
  void SetOutputLatency(double seconds) noexcept;
  std::uint32_t BlockFrames() const noexcept;
  std::uint32_t SampleRate() const noexcept;
  // Last block's timing (read on the audio thread or after it stopped).
  const BlockProfile& LastBlock() const noexcept;

  // --- live link (audio thread: only from a LiveTap hook or CoreDesc::liveCommand) ------
  // Profiler reads at the end of a block (after every system). Each returns the number
  // of entries that exist and writes min(that, capacity).
  std::uint32_t LiveVoices(livelink::VoiceInfo* out, std::uint32_t capacity) const noexcept;
  std::uint32_t LiveMeters(livelink::MeterEntry* out, std::uint32_t capacity) const noexcept;
  std::uint32_t LiveBanks(LiveBankInfo* out, std::uint32_t capacity) const noexcept;
  void LiveStats(snd_stats* out) const noexcept;
  // Removes one live override (kind = cmd::LiveOverrideKind, id); nothing if absent. Only
  // from a LiveTap hook (an older session's revert removes its own overrides, D-87).
  void LiveRemoveOverride(std::uint32_t kind, Id id) noexcept;
  // Event id of the playback whose voice reads stream slot `slot` (0 if none).
  Id LiveStreamEvent(std::uint32_t slot) const noexcept;
  // Override layer (11.2). Cells of the object `objectId` in table `fourcc`: tables with
  // an id column match the row by id (index must be 0); list tables are addressed
  // through their owner (livelink-protocol.md "値の変更の宛先", see live_link.cpp):
  // TRAK/SEND/EFFX/SNPV/MPLE/CURV by the owner's id and the offset `index`, CLIP/ACTN by
  // event id and (track << 16 | item), EFPV by bus id and (effect << 16 | param), CPNT
  // by event id and (curve << 16 | point). One cell per registered bank holding it.
  std::uint32_t LiveFindCells(Id objectId, std::uint32_t fourcc, std::uint32_t index, LiveCell* out,
                              std::uint32_t capacity) const noexcept;
  // Registered bank slots with this bank id (ReplaceChunk).
  std::uint32_t LiveFindBanks(Id bankId, std::uint32_t* slots, std::uint32_t capacity) const noexcept;
  // Rows of table `fourcc` in the override layer of a registered bank (0 if none).
  std::uint32_t LiveTableRows(std::uint32_t bankSlot, std::uint32_t fourcc) const noexcept;
  // Reads (*previous) and, with apply, writes one cell, then refreshes what core derived
  // from it at registration (bus / category volume, voice limits, parameter definitions,
  // ducking, effect parameters). apply = false only validates.
  //   SND_ERR_UNSUPPORTED       no override layer, or the column is not live-editable
  //                             (references and layout: rebuild and hot reload)
  //   SND_ERR_NOT_FOUND         unknown bank slot / table / column / row
  //   SND_ERR_INVALID_ARGUMENT  value out of the column's range (NaN, enum)
  // Writing the value a non-editable cell already holds succeeds (no change).
  snd_result LiveWriteCell(const LiveCell& cell, std::uint32_t fourcc, std::uint32_t column, std::uint64_t bits,
                           std::uint64_t* previous, bool apply) noexcept;

  // --- stage 3 (coverage, group 2): effects at run time ---------------------------------
  // Recording (registered threads; semantics of snd/sonus.h snd_set_effect_param,
  // snd_set_event_voice_effect, snd_set_playback_effect_param). D-130, D-132.
  snd_result SetEffectParam(Id bus, std::uint32_t effectIndex, std::uint32_t paramIndex, float value,
                            float fadeSeconds) noexcept;
  snd_result SetEventVoiceEffect(Id event, std::uint32_t slot, Id effectKind, const float* params,
                                 std::uint32_t paramCount) noexcept;
  snd_result SetPlaybackEffectParam(snd_playback_handle playback, std::uint32_t slot, std::uint32_t paramIndex,
                                    float value) noexcept;
  // Voice-effect pool counters (D-133). Read on the audio thread or after it stopped (tests,
  // tools); not part of snd_stats (the public struct is not extended).
  struct VoiceFxStats {
    std::uint64_t acquired = 0;     // real assignments that got a voice-effect slot
    std::uint64_t dry = 0;          // real assignments that played dry (no slot, or too large)
    std::uint64_t dryTooLarge = 0;  // of dry: the chain's state did not fit voice_effect_bytes
    std::uint32_t slotsInUse = 0;
    std::uint32_t slots = 0;
  };
  VoiceFxStats GetVoiceFxStats() const noexcept;
  // Tests only (no audio thread inside RenderBlock): sets the load LOD level (2.4, 0..3).
  // UpdateLod keeps moving it from the measured block times afterwards.
  void SetLodLevelForTesting(std::uint32_t level) noexcept;

  struct Impl;
  Impl* impl() noexcept { return impl_; }
  const Impl* impl() const noexcept { return impl_; }

 private:
  System() = default;
  ~System() = default;
  Impl* impl_ = nullptr;
};

}  // namespace snd::core

#endif  // SND_CORE_CORE_HPP
