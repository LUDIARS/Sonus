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
#include "snd/internal/commands.hpp"
#include "snd/internal/music_if.hpp"
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
inline constexpr std::uint8_t kFireTransportGone = 0xFEu;
inline constexpr std::uint16_t kClipRefAssetTrack = 0xFFFFu;

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
  LiveCommandFn liveCommand = nullptr;
  void* liveUser = nullptr;
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
  std::uint32_t BlockFrames() const noexcept;
  std::uint32_t SampleRate() const noexcept;
  // Last block's timing (read on the audio thread or after it stopped).
  const BlockProfile& LastBlock() const noexcept;

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
