// Preview engine abstraction and the audition router (design.md 11.1 試聴, 11.2), group P.
// UI independent (Sonus::tool_core).
//
// The preview engine is "the game inside the tool": a Sonus runtime that runs in the tool's
// process so that the user can listen without a game. Its implementation needs the whole runtime
// (source kinds, music, IO): it lives in the preview library (snd/tool/preview/preview_engine.hpp,
// Sonus::tool_preview, built with SND_BUILD_TOOL_CORE; the tool's core only depends on
// buildtool, dsp, core and livelink, 12.1). The core sees it through IPreviewEngine.
//
// The engine runs the live link server on an in-process transport (livelink MemoryLink): the
// tool connects to connector() with an ordinary Connection / Client, so live edits (LiveSync),
// item reverts, read-backs, the profiler and recordings take the same path as with a real game.
// Auditions that need the runtime's own API (moving emitters, the listener, a music state at an
// exact block) use the engine's functions below.
//
// Threads: every IPreviewEngine function is called from the engine's owner thread (the thread
// that created it, the tool's UI thread), except connector() (any). In Manual output the owner
// renders (Render); in Device / Silent output the platform's audio thread renders and the owner
// calls Pump() once per frame (it publishes the owner's commands: snd_update's job).
//
// The audition router sends one request to either destination: the connected game (live link
// Audition / OverrideParam / OverrideState / StopAuditions) or the preview engine (its API). What a
// destination cannot do is said, with the reason (AuditionResult::unsupported), never dropped
// silently: a game can only start an event at a fixed position (no moving emitter, no listener,
// protocol 1.0 has no stop-all), and its music state switches when the message arrives (not at an
// exact block).
#ifndef SND_TOOL_PREVIEW_HPP
#define SND_TOOL_PREVIEW_HPP

#include <cstdint>
#include <string>
#include <vector>

#include "snd/id.hpp"
#include "snd/tool/spatial_preview.hpp"

namespace snd::livelink {
class IConnector;
}

namespace snd::tool {

struct DocBuild;  // doc_build.hpp
class Connection;

enum class PreviewOutput : std::uint8_t {
  Manual = 0,  // the owner renders blocks (Render): tests, offline listening, WAV export
  Device = 1,  // the platform's output device (real time)
  Silent = 2,  // real time without sound (a timer; headless hosts)
};

struct PreviewStatus {
  bool running = false;
  PreviewOutput output = PreviewOutput::Manual;
  std::uint32_t sampleRate = 0, blockFrames = 0, channels = 0;
  std::uint64_t block = 0;               // next block to render (Manual) / blocks rendered so far
  std::uint32_t banks = 0;               // banks loaded
  std::uint64_t buildHash = 0;           // project build hash of the loaded build
  std::uint32_t activePlaybacks = 0;     // auditions started by the engine and not stopped
  std::uint32_t underruns = 0;           // Device / Silent
  std::string error;                     // last error (device, bank loading)
};

class IPreviewEngine {
 public:
  virtual ~IPreviewEngine() = default;

  // --- banks ------------------------------------------------------------------------------
  // Loads every bank of `build` (dev builds: the override layer is writable) in place of the
  // banks loaded before. Returns when they are loaded (Manual: renders the blocks it takes;
  // Device: waits). false + error if one does not load.
  virtual bool LoadBuild(const DocBuild& build, std::string* error) = 0;
  // A file of the engine's file system (the root of ReloadBank paths and stream files).
  virtual bool PutFile(const std::string& path, const std::vector<std::uint8_t>& bytes) = 0;
  // A path not used before for a rebuilt bank ("live/<n>/<file>"), and PutFile of it: the
  // LiveSync hot reload writer of the engine.
  virtual std::string PutRebuiltBank(const std::string& file, const std::vector<std::uint8_t>& bytes) = 0;

  // --- live link (any thread) ---------------------------------------------------------------
  virtual livelink::IConnector* connector() = 0;
  virtual std::uint64_t projectBuildHash() const = 0;  // of the loaded build (Hello)

  // --- auditions (owner) --------------------------------------------------------------------
  // Plays an event (emitter null: 2D, no emitter). Returns the playback handle value, 0 if refused.
  virtual std::uint64_t Play(Id event, const EmitterPose* emitter) = 0;
  // Moves the emitter of a Play started with one (false: unknown handle / no emitter).
  virtual bool MoveEmitter(std::uint64_t playback, const EmitterPose& pose) = 0;
  virtual bool SetListener(const ListenerPose& pose) = 0;
  virtual bool SetParam(Id param, float value) = 0;  // global parameter
  // Requests a music state of a playing music event at block `atBlock` (the transition then
  // follows the rule table from that moment). atBlock <= the next block: now.
  virtual bool SetMusicState(std::uint64_t playback, Id state, std::uint64_t atBlock) = 0;
  virtual bool Stop(std::uint64_t playback) = 0;
  virtual std::uint32_t StopAll() = 0;  // every playback the engine started (not the live link's)

  // --- time and output ----------------------------------------------------------------------
  virtual std::uint64_t NextBlock() const = 0;  // the number of the next block to render
  // Manual: renders `blocks` blocks (applying what is scheduled for them) and appends the
  // interleaved output to `out` (may be null). Returns the blocks rendered (0 in Device / Silent).
  virtual std::uint32_t Render(std::uint32_t blocks, std::vector<float>* out) = 0;
  virtual void Pump() = 0;  // Device / Silent: once per UI frame (owner)
  virtual PreviewStatus Status() const = 0;
};

// --- the audition router --------------------------------------------------------------------
enum class AuditionTarget : std::uint8_t { Game = 0, Preview = 1 };

struct AuditionResult {
  bool ok = false;
  AuditionTarget target = AuditionTarget::Game;
  std::uint64_t playback = 0;      // Preview: the handle; Game: 0 (the game does not tell it)
  std::uint32_t seq = 0;           // Game: the live link request (Ack with the result)
  std::vector<std::string> unsupported;  // what the destination could not do, with the reason
  std::string error;
};

// Music transition audition (8.7): plays the music event in `from`, asks for `to` at
// `requestSeconds` after the start; the switch follows the rule table (MusicEditor::Preview
// computes where it should happen).
struct TransitionAudition {
  Id musicEvent = 0;
  Id from = 0;
  Id to = 0;
  double requestSeconds = 0.0;
};

class AuditionRouter {
 public:
  void Bind(Connection* game, IPreviewEngine* preview) {
    game_ = game;
    preview_ = preview;
  }
  void SetTarget(AuditionTarget t) { target_ = t; }
  AuditionTarget target() const { return target_; }
  // The destination can take requests now (connected game / an engine).
  bool Ready(std::string* why = nullptr) const;

  // Event audition (AuditionRequest of the 3D preview / Event editor): at the request's
  // position (atPosition) with the request's listener.
  AuditionResult PlayEvent(const AuditionRequest& request);
  // 3D preview: starts like PlayEvent, then Update3D moves the emitter and the listener while it
  // plays (the preview engine only; the game keeps the start position).
  AuditionResult Start3D(const AuditionRequest& request);
  AuditionResult Update3D(const ListenerPose& listener, const EmitterPose& emitter);
  // Music transition audition.
  AuditionResult PlayTransition(const TransitionAudition& t);
  // Parameter and state while auditioning (global parameter override / music state override).
  AuditionResult SetParam(Id param, float value);
  AuditionResult ReleaseParam(Id param);
  AuditionResult SetState(Id musicEvent, Id state);
  // Stops every audition this router started on its destination.
  AuditionResult StopAll();

  std::uint64_t moving() const { return moving_; }  // the 3D audition being moved (Preview)
  const std::vector<std::uint64_t>& playing() const { return playing_; }  // Preview handles

 private:
  AuditionResult Begin() const;
  Connection* game_ = nullptr;
  IPreviewEngine* preview_ = nullptr;
  AuditionTarget target_ = AuditionTarget::Preview;
  std::uint64_t moving_ = 0;
  std::vector<std::uint64_t> playing_;
  std::vector<std::pair<Id, std::uint64_t>> musicPlaybacks_;  // music event -> playback (Preview)
};

}  // namespace snd::tool

#endif  // SND_TOOL_PREVIEW_HPP
