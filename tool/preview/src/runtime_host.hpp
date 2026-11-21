// A Sonus runtime inside the tool (private to the preview library). The same composition as the
// game's (api/compose/compose.cpp, CONTRACTS.md D-93): an in-memory file system, normal workers,
// the codec registry, the stream pool and IO scheduler (streams), the source kinds
// (compose::SourceKinds), the music system, core::System, and either the live link (LiveRuntime +
// Server on an in-process MemoryLink) or a Replayer installed into core. Output: blocks rendered by
// the owner (Manual) or the platform's audio driver on a device (Default) or a timer (SilentTimer).
//
// Order (D-93): fs -> workers -> codecs -> stream pool + IO -> source kinds -> [LiveRuntime::Create
// -> Install | Replayer::Install] -> core::System::Create -> [Bind] -> [audio driver] -> [Server].
// Destroy is the reverse. Owner thread: the creator.
#ifndef SND_TOOL_PREVIEW_RUNTIME_HOST_HPP
#define SND_TOOL_PREVIEW_RUNTIME_HOST_HPP

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

#include "memory_fs.hpp"
#include "snd/api/compose.hpp"
#include "snd/codec/registry.hpp"
#include "snd/config.h"
#include "snd/core/core.hpp"
#include "snd/io/io_scheduler.hpp"
#include "snd/io/stream_pool.hpp"
#include "snd/jobs/worker_pool.hpp"
#include "snd/livelink/memory_transport.hpp"
#include "snd/livelink/replayer.hpp"
#include "snd/livelink/runtime.hpp"
#include "snd/livelink/server.hpp"
#include "snd/music/music_system.hpp"
#include "snd/platform/audio_driver.hpp"
#include "snd/platform/output_device.hpp"
#include "snd/platform/platform_output_device.hpp"

namespace snd::tool::preview {

enum class HostHooks : std::uint8_t { None = 0, Live = 1, Replay = 2 };
enum class HostOutput : std::uint8_t { Manual = 0, Device = 1, Silent = 2 };

struct RuntimeHostDesc {
  snd_config config{};  // copied; livelink.enabled is ignored (hooks decide)
  HostHooks hooks = HostHooks::None;
  livelink::Replayer* replayer = nullptr;      // Replay
  livelink::IStructEditor* structEditor = nullptr;  // Live (RuntimeDesc::structEditor)
  std::uint32_t chunkSlots = 1;                // Live (RuntimeDesc::chunkSlots)
  HostOutput output = HostOutput::Manual;
};

class RuntimeHost {
 public:
  RuntimeHost() = default;
  ~RuntimeHost() { Destroy(); }
  RuntimeHost(const RuntimeHost&) = delete;
  RuntimeHost& operator=(const RuntimeHost&) = delete;

  snd_result Create(const RuntimeHostDesc& desc, std::string* error);
  void Destroy();
  bool created() const { return sys_ != nullptr; }

  // Live link server on the in-process transport (Live hooks).
  snd_result StartServer(livelink::ServerDesc extra);
  livelink::IConnector* connector() { return &link_.connector(); }

  // Owner, Manual output: Update + one block (out: block frames x channels, may be null).
  void Step(float* out);
  // Owner: publishes the owner's commands (Device / Silent; Step does it in Manual).
  void Pump();
  // Loads a bank file of the file system; renders / waits until it is loaded (or failed).
  snd_bank_handle LoadBank(const std::string& path, std::uint32_t timeoutMs, std::string* error);
  bool UnloadBank(snd_bank_handle h, std::uint32_t timeoutMs);

  std::uint64_t BlocksRendered() const { return blocks_.load(std::memory_order_acquire); }
  std::uint32_t Underruns() const;
  std::uint32_t Channels() const { return channels_; }
  std::uint32_t BlockFrames() const { return blockFrames_; }
  std::uint32_t SampleRate() const { return config_.sample_rate; }
  HostOutput output() const { return output_; }
  const snd_config& config() const { return config_; }

  core::System* system() { return sys_; }
  livelink::LiveRuntime* live() { return live_; }
  livelink::Server* server() { return server_; }
  music::MusicSystem& music() { return music_; }
  MemoryFs& fs() { return fs_; }

  // Device / Silent: the last rendered blocks (for meters and tests), oldest first.
  std::vector<float> RecentOutput(std::uint32_t blocks) const;

 private:
  static void RenderThunk(void* user, float* interleaved, std::uint32_t frames, std::uint32_t channels,
                          const platform::RenderInfo* info) noexcept;
  void Keep(const float* block);

  snd_config config_{};
  snd_config coreConfig_{};
  HostOutput output_ = HostOutput::Manual;
  std::uint32_t channels_ = 2, blockFrames_ = 256;
  MemoryFs fs_;
  jobs::WorkerPool workers_;
  bool workersStarted_ = false;
  codec::CodecRegistry codecs_;
  io::StreamPool streamPool_;
  io::IoScheduler io_;
  bool ioInit_ = false, ioStarted_ = false;
  compose::SourceKinds kinds_;
  music::MusicSystem music_;
  livelink::LiveRuntime* live_ = nullptr;
  livelink::Replayer* replayer_ = nullptr;
  core::System* sys_ = nullptr;
  livelink::MemoryLink link_;
  bool linkInit_ = false;
  livelink::Server* server_ = nullptr;
  platform::PlatformOutputDevice* device_ = nullptr;
  platform::AudioDriver driver_;
  bool driverStarted_ = false;
  std::vector<float> mix_;
  std::atomic<std::uint64_t> blocks_{0};
  std::uint64_t lostPending_ = 0;  // audio thread
  mutable std::mutex recentMutex_;
  std::vector<float> recent_;  // ring of the last kRecentBlocks blocks
  std::size_t recentAt_ = 0;
};

}  // namespace snd::tool::preview

#endif  // SND_TOOL_PREVIEW_RUNTIME_HOST_HPP
