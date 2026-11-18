// Composition root of the `sonus` library (api/compose/, CONTRACTS.md D-03, D-93).
//
// snd_create() (snd/sonus.h) builds one runtime per snd_system from the modules
// api may not depend on at compile time (12.1). The order (D-93):
//
//   create   allocator, file system, normal workers, codec registry
//            -> StreamPool + IoScheduler (channels = Config.maxStreams), IO thread
//            -> SourceKinds::Init(alloc, io, pool, maxVoiceChannels, maxBanks)
//            -> [live link] LiveRuntime::Create -> Install(&coreDesc)
//            -> core::System::Create(sources, music, jobs, io, bankUnregistered,
//               findCodec, dsp.compute (core), Config 1.2 capacities)
//            -> [live link] LiveRuntime::Bind
//            -> AudioDriver::Start (render = core RenderBlock, workgroupSource)
//            -> [live link] Server::Create -> Start
//   destroy  [live link] Server::Destroy -> AudioDriver::Stop -> io.Shutdown
//            -> workers Shutdown -> core::System::Destroy -> [live link]
//            LiveRuntime::Destroy -> SourceKinds::Destroy -> io / pool Destroy
//            -> device, file system, adapters
// The live link runs only when Config.livelink.enabled != 0 and the build has
// SND_LIVELINK=1; its thread registers itself, so core gets
// Config.maxRegisteredThreads + 1 producers. The music system lives in the
// snd_system (one per system) and outlives core::System (CoreDesc::music).
//
// The rest of this header is for tests: the lifecycle observer and read-only
// access to the composed objects.
#ifndef SND_API_COMPOSE_HPP
#define SND_API_COMPOSE_HPP

#include <cstdint>

#include "snd/ext/allocator.hpp"
#include "snd/internal/source_if.hpp"
#include "snd/sonus.h"

namespace snd::io {
class IoScheduler;
class StreamPool;
}  // namespace snd::io
namespace snd::core {
class System;
}  // namespace snd::core
namespace snd::livelink {
class LiveRuntime;
class Server;
}  // namespace snd::livelink
namespace snd::platform {
class AudioDriver;
}  // namespace snd::platform
namespace snd::source {
class Runtime;
}  // namespace snd::source
namespace snd::music {
class MusicSystem;
}  // namespace snd::music

namespace snd::compose {

// The source module's kinds (Sample, Stream, Callback, UserBuffer) for core
// (CoreDesc::sources). snd_create() makes one per system: Init before core's
// Create, Destroy after core's Destroy. Without an IO scheduler (and its stream
// pool) the Stream kind is absent (stream voices end UNSUPPORTED).
class SourceKinds {
 public:
  SourceKinds() noexcept = default;
  ~SourceKinds() { Destroy(); }
  SourceKinds(const SourceKinds&) = delete;
  SourceKinds& operator=(const SourceKinds&) = delete;

  // maxVoiceChannels: Config.maxVoiceChannels (the PCM rings are sized for it, source/runtime.hpp).
  // maxBanks: Config.maxBanks (one unregistration counter per bank slot).
  bool Init(IAllocator& allocator, io::IoScheduler* io, io::StreamPool* streamPool,
            std::uint32_t maxVoiceChannels = 8, std::uint32_t maxBanks = 64) noexcept;
  void Destroy() noexcept;
  // nullptr before Init.
  const source::SourceKindSet* Kinds() const noexcept;
  // For core::CoreDesc::bankUnregistered / bankUser (the PCM cache drops an unregistered bank).
  static void BankUnregistered(void* user, std::uint16_t bankSlot) noexcept;
  void* BankUser() const noexcept { return runtime_; }
  // The source::Runtime (nullptr before Init), for its statistics.
  const source::Runtime* SourceRuntime() const noexcept { return static_cast<const source::Runtime*>(runtime_); }

 private:
  void* runtime_ = nullptr;  // snd::source::Runtime
  IAllocator* alloc_ = nullptr;
};

// Names of the runtime modules linked into `sonus`, in dependency order.
const char* const* LinkedModules(unsigned* count) noexcept;

// --- tests ---------------------------------------------------------------------------
// Called with the name of every lifecycle step of snd_create / snd_destroy (the
// D-93 order), on the calling thread. Process-wide; nullptr removes it.
using LifecycleObserver = void (*)(void* user, const char* step) noexcept;
void SetLifecycleObserver(LifecycleObserver observer, void* user) noexcept;

// The objects a system was composed of (nullptr when absent). For tests and tools:
// the thread rules of each object apply.
struct Parts {
  core::System* core = nullptr;
  music::MusicSystem* music = nullptr;
  const source::Runtime* sources = nullptr;
  io::IoScheduler* io = nullptr;
  platform::AudioDriver* driver = nullptr;  // nullptr when the driver did not start
  livelink::LiveRuntime* live = nullptr;
  livelink::Server* server = nullptr;
  std::uint32_t coreRegisteredThreads = 0;  // Config.maxRegisteredThreads given to core
};
Parts PartsOf(snd_system* sys) noexcept;

}  // namespace snd::compose

#endif  // SND_API_COMPOSE_HPP
