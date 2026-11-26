// The preview engine: a Sonus runtime inside the tool (design.md 11.1 試聴, 11.2), group P.
// Library Sonus::tool_preview (built with SND_BUILD_TOOL_CORE / SND_BUILD_TOOL; no Tela).
//
// "The game inside the tool": the same composition as a game (api/compose, D-93: file system,
// workers, codecs, IO and streams, source kinds, music, core), with the live link server on an
// in-process transport. The tool connects to connector() with an ordinary Connection, so live
// edits (LiveSync), reverts, read-backs, the profiler and recordings run the game's path. The
// engine's own API (IPreviewEngine, preview.hpp) plays auditions that need the runtime directly.
//
// Banks: LoadBuild puts the document's built banks into the engine's in-memory file system
// ("build/<n>/<file>", and the build's other files, stream files, at their output paths) and loads
// them (dependencies first; language banks are skipped and said in the error text). Hot reloads
// (LiveSync's writer = PutRebuiltBank) write "live/<n>/<file>". LoadBuild first unloads every bank
// loaded before, those a hot reload put in place too (D-184), however often it is called.
//
// Output (PreviewOutput): Manual (the owner calls Render: deterministic, tests and WAV export),
// Device (the platform's default device, real time; the owner calls Pump each frame), Silent
// (real time on a timer, no sound). In Device / Silent mode RecentOutput gives the last blocks.
//
// Threads: the owner (creator) thread for everything but connector() and RecentOutput (any).
#ifndef SND_TOOL_PREVIEW_PREVIEW_ENGINE_HPP
#define SND_TOOL_PREVIEW_PREVIEW_ENGINE_HPP

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "snd/config.h"
#include "snd/tool/preview.hpp"

namespace snd::core {
class System;
}
namespace snd::livelink {
class IStructEditor;
IStructEditor& CoreStructEditor() noexcept;
class Server;
}  // namespace snd::livelink

namespace snd::tool::preview {

class RuntimeHost;

// A Config for the preview: snd_config_init with the live link on, one real-time-free render
// thread (realtime_workers 0: Manual output renders on the owner, deterministically), LOD off
// (2.5) and modest capacities.
snd_config PreviewConfig();

struct PreviewEngineDesc {
  snd_config config = PreviewConfig();
  PreviewOutput output = PreviewOutput::Manual;
  bool liveLink = true;                                // the in-process server
  bool revertOnDisconnect = false;                     // ServerDesc::revertOnDisconnect
  livelink::IStructEditor* structEditor = &livelink::CoreStructEditor();  // null: hot reload fallback
  std::uint32_t chunkSlots = 1;                        // ReplaceChunk in flight (RuntimeDesc)
  std::uint32_t bankTimeoutMs = 10000;
};

// One executed music transition (music::MusicTransitionRecord).
struct PreviewTransition {
  Id from = 0, to = 0;
  std::uint64_t clock = 0;  // common clock sample of the switch
  std::int64_t tick = 0;    // transport tick
};

class PreviewEngine final : public IPreviewEngine {
 public:
  static std::unique_ptr<PreviewEngine> Create(const PreviewEngineDesc& desc, std::string* error);
  ~PreviewEngine() override;
  PreviewEngine(const PreviewEngine&) = delete;
  PreviewEngine& operator=(const PreviewEngine&) = delete;

  // IPreviewEngine
  bool LoadBuild(const DocBuild& build, std::string* error) override;
  bool PutFile(const std::string& path, const std::vector<std::uint8_t>& bytes) override;
  std::string PutRebuiltBank(const std::string& file, const std::vector<std::uint8_t>& bytes) override;
  livelink::IConnector* connector() override;
  std::uint64_t projectBuildHash() const override { return buildHash_; }
  std::uint64_t Play(Id event, const EmitterPose* emitter) override;
  bool MoveEmitter(std::uint64_t playback, const EmitterPose& pose) override;
  bool SetListener(const ListenerPose& pose) override;
  bool SetParam(Id param, float value) override;
  bool SetMusicState(std::uint64_t playback, Id state, std::uint64_t atBlock) override;
  bool Stop(std::uint64_t playback) override;
  std::uint32_t StopAll() override;
  std::uint64_t NextBlock() const override;
  std::uint32_t Render(std::uint32_t blocks, std::vector<float>* out) override;
  void Pump() override;
  PreviewStatus Status() const override;
  double StreamPrimeMs() const override;

  // --- more (owner) --------------------------------------------------------------------------
  // Device / Silent: the last `blocks` blocks rendered (any thread).
  std::vector<float> RecentOutput(std::uint32_t blocks) const;
  // Executed music transitions (oldest first). Manual: between Render calls.
  std::vector<PreviewTransition> Transitions() const;
  const snd_config& config() const;
  core::System* system();               // tests (owner, between Render calls)
  livelink::Server* server();           // tests
  void ResetErrors() { error_.clear(); }

 private:
  PreviewEngine() = default;
  struct Scheduled {
    std::uint64_t block;
    std::uint64_t playback;
    Id state;
  };
  struct Playing {
    std::uint64_t playback;
    std::uint64_t emitter;  // emitter handle value, 0 = none
    std::uint64_t startBlock;
  };
  void RunScheduled(std::uint64_t upToBlock);
  void Forget(std::uint64_t playback);

  PreviewEngineDesc desc_;
  std::unique_ptr<RuntimeHost> host_;
  std::uint64_t buildHash_ = 0;
  std::vector<std::uint64_t> banks_;  // loaded bank handles
  std::uint32_t buildSerial_ = 0, reloadSerial_ = 0;
  std::vector<std::string> rebuilt_;  // PutRebuiltBank paths (a hot reload may have loaded them)
  std::vector<Scheduled> scheduled_;
  std::vector<Playing> playing_;
  std::string error_;
};

}  // namespace snd::tool::preview

#endif  // SND_TOOL_PREVIEW_PREVIEW_ENGINE_HPP
