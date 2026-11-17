// The tool's models together (UI independent): project, connection, mixer,
// value sync and profiler. Tick() runs once per UI frame on the UI thread.
#ifndef SND_TOOL_SESSION_HPP
#define SND_TOOL_SESSION_HPP

#include <cstdint>
#include <string>
#include <vector>

#include "snd/tool/cell_sync.hpp"
#include "snd/tool/connection.hpp"
#include "snd/tool/mixer.hpp"
#include "snd/tool/profiler.hpp"
#include "snd/tool/project.hpp"

namespace snd::tool {

class Session {
 public:
  Session() = default;
  explicit Session(const Profiler::Desc& profiler) : profiler_(profiler) {}
  Session(const Session&) = delete;
  Session& operator=(const Session&) = delete;

  // Loads the project; with `buildDir` also builds it (the build hash for Hello).
  bool LoadProject(const std::string& dir, const std::string& buildDir = std::string());
  // Uses an already loaded project (copied).
  void UseProject(const Project& project);

  // Opens the connection; projectBuildHash 0 takes the project's build hash.
  int Connect(ConnectionDesc desc);
  void Disconnect();
  void Tick(std::uint64_t nowNs);

  Project& project() { return project_; }
  const Project& project() const { return project_; }
  MixerModel& mixer() { return mixer_; }
  const MixerModel& mixer() const { return mixer_; }
  CellSync& sync() { return sync_; }
  const CellSync& sync() const { return sync_; }
  Profiler& profiler() { return profiler_; }
  const Profiler& profiler() const { return profiler_; }
  Connection& connection() { return connection_; }
  const Connection& connection() const { return connection_; }

  RecordingInfo CurrentRecordingInfo() const;
  bool SaveRecording(const std::string& path);
  bool LoadRecording(const std::string& path);

  // Status messages for the UI (newest last, at most 64).
  const std::vector<std::string>& messages() const { return messages_; }
  void Message(std::string text);
  std::uint64_t ticks() const { return ticks_; }

 private:
  Project project_;
  MixerModel mixer_;
  CellSync sync_;
  Profiler profiler_;
  Connection connection_;
  std::vector<std::string> messages_;
  std::uint64_t refusedSeen_ = 0;
  std::uint64_t ticks_ = 0;
};

}  // namespace snd::tool

#endif  // SND_TOOL_SESSION_HPP
