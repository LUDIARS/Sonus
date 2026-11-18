// The snd_system object (snd/sonus.h) and the composition root's lifecycle
// (CONTRACTS.md D-03, D-93). Private to the `sonus` library.
//
// One snd_system owns, for its whole life, every object snd_create() makes:
//   allocator adapter, file system (the game's, or io::DefaultFileSystem),
//   normal workers (the game's IJobSystem, or jobs::WorkerPool), the codec
//   registry (Config.plugins.codecs), the stream pool and IO scheduler, the
//   source kinds (compose::SourceKinds), the music system (one per system; it
//   outlives core::System, CoreDesc::music), core::System, the audio driver
//   (platform::AudioDriver on the game's device or the platform default) and,
//   when Config.livelink.enabled and the build has SND_LIVELINK=1, the live link
//   (LiveRuntime + Server).
// Order (D-93): see Compose() / Decompose() in compose.cpp.
#ifndef SND_API_COMPOSE_SYSTEM_HPP
#define SND_API_COMPOSE_SYSTEM_HPP

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <new>
#include <thread>
#include <utility>

#include "snd/api/compose.hpp"
#include "snd/codec/registry.hpp"
#include "snd/core/core.hpp"
#include "snd/ext/allocator.hpp"
#include "snd/ext/file_system.hpp"
#include "snd/ext/job_system.hpp"
#include "snd/ext/output_device.hpp"
#include "snd/io/default_file_system.hpp"
#include "snd/io/io_scheduler.hpp"
#include "snd/io/stream_pool.hpp"
#include "snd/jobs/worker_pool.hpp"
#include "snd/livelink/command_check.hpp"
#include "snd/livelink/runtime.hpp"
#include "snd/livelink/server.hpp"
#include "snd/music/music_system.hpp"
#include "snd/platform/audio_driver.hpp"
#include "snd/platform/output_device.hpp"
#include "snd/sonus.h"

namespace snd::compose {

// In-place storage for an object built later (constructors with arguments, no exceptions).
template <class T>
class Late {
 public:
  Late() noexcept = default;
  ~Late() { Reset(); }
  Late(const Late&) = delete;
  Late& operator=(const Late&) = delete;
  template <class... A>
  T* Emplace(A&&... a) noexcept {
    Reset();
    T* p = new (buf_) T(std::forward<A>(a)...);
    live_ = true;
    return p;
  }
  void Reset() noexcept {
    if (live_) Get()->~T();
    live_ = false;
  }
  T* Get() noexcept { return live_ ? std::launder(reinterpret_cast<T*>(buf_)) : nullptr; }
  bool Live() const noexcept { return live_; }

 private:
  alignas(T) unsigned char buf_[sizeof(T)];
  bool live_ = false;
};

// Device events (any thread: device / OS notification) -> owner thread (snd_update).
struct DeviceEventFlags {
  std::atomic<std::uint32_t> suspended{0};    // SUSPENDED seen, RESUMED not yet
  std::atomic<std::uint32_t> lost{0};         // LOST events not reported yet
  std::atomic<std::uint32_t> focusLost{0};    // FOCUS_LOST without FOCUS_GAINED
  std::atomic<std::uint32_t> rateMismatch{0}; // device rate != internal rate (reported once)
  std::atomic<std::uint32_t> deviceRate{0};
  std::atomic<std::uint32_t> deviceChannels{0};
};

}  // namespace snd::compose

struct snd_system {
  // --- configuration -----------------------------------------------------------------
  snd_config config{};     // normalised (D-91): struct_size completed, defaults filled
  snd_config coreConfig{}; // what core / live link get (+1 registered thread for the live link)
  snd::livelink::CommandLimits limits{};  // the shared rule table's limits (D-84, D-93)
  std::thread::id owner{};

  // Copies of the tables the game passed by pointer (snd_create copies every table,
  // snd/sonus.hpp; core keeps pointing at the effect table): the configs point here.
  snd_codec codecTables[32] = {};
  snd_dsp_effect effectTables[32] = {};
  snd_job_system jobTable{};
  snd_output_device deviceTable{};

  // --- services ------------------------------------------------------------------------
  snd_allocator gameAllocator{};  // copy of the game's table (frees the system object itself)
  bool hasGameAllocator = false;
  snd::IAllocator* alloc = nullptr;
  snd::compose::Late<snd::CAllocator> cAllocator;
  snd::IFileSystem* fs = nullptr;
  snd::compose::Late<snd::CFileSystem> cFileSystem;
  snd::io::DefaultFileSystem defaultFs;
  bool ownFs = false;
  snd::IJobSystem* jobs = nullptr;
  snd::compose::Late<snd::CJobSystem> cJobs;
  snd::jobs::WorkerPool workers;
  bool ownWorkers = false;
  snd::codec::CodecRegistry codecs;
  snd::io::StreamPool streamPool;
  snd::io::IoScheduler io;
  bool ioStarted = false;
  snd::compose::SourceKinds kinds;
  snd::music::MusicSystem music;

  // --- runtime -------------------------------------------------------------------------
  snd::core::System* core = nullptr;
  snd::livelink::LiveRuntime* live = nullptr;
  snd::livelink::Server* server = nullptr;

  // --- output --------------------------------------------------------------------------
  snd::IOutputDevice* device = nullptr;
  snd::compose::Late<snd::COutputDevice> cDevice;
  snd::platform::PlatformOutputDevice* ownDevice = nullptr;
  snd::platform::AudioDriver driver;
  bool driverStarted = false;
  std::uint32_t coreChannels = 2;
  std::uint32_t blockFrames = 256;
  float* mix = nullptr;               // core-format block when the device format differs
  std::uint64_t lostPending = 0;      // audio thread: device silence not yet caught up (2.2)
  float focusGain = 1.0f;             // audio thread: SND_FOCUS_SILENCE ramp
  snd::compose::DeviceEventFlags deviceEvents;
  std::atomic<std::uint32_t> resumeRequested{0};  // snd_resume from a non-owner thread

  // --- callbacks (owner thread) ----------------------------------------------------------
  snd_callbacks callbacks{};
  bool hasCallbacks = false;
};

namespace snd::compose {

// Builds every object of `sys` (zero-initialised storage already constructed) in
// the D-93 order. On failure everything built so far is taken down (Decompose)
// and the error is returned.
snd_result Compose(snd_system& sys, const snd_allocator* allocator, const snd_file_system* fileSystem) noexcept;
// Takes everything down in the reverse order. Idempotent.
void Decompose(snd_system& sys) noexcept;
// Owner thread, inside snd_update before core's Update: pending device work.
void ServiceDevice(snd_system& sys) noexcept;
// Owner thread: AudioDriver::Resume for a device the OS left SUSPENDED (D-66). false if
// there was nothing to resume or the device refused.
bool ResumeDevice(snd_system& sys) noexcept;

// Lifecycle steps reported to the observer (tests).
void Note(const char* step) noexcept;

}  // namespace snd::compose

#endif  // SND_API_COMPOSE_SYSTEM_HPP
