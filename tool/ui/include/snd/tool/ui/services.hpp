// What the replay screen needs from a runtime (design.md 2.5, 11.3). Replaying runs a Sonus
// runtime inside the tool (Sonus::tool_preview, which composes the runtime modules); the UI
// library itself only depends on the tool's core, so the screen talks to this interface and the
// host passes an implementation (MakeReplayService, library sonus_tool_ui_replay, links
// Sonus::tool_preview). Without one the screen says so and only describes recordings.
#ifndef SND_TOOL_UI_SERVICES_HPP
#define SND_TOOL_UI_SERVICES_HPP

#include <memory>
#include <string>

#include "snd/tool/doc_build.hpp"
#include "snd/tool/recording_report.hpp"

namespace snd::tool::ui {

class ReplayService {
 public:
  virtual ~ReplayService() = default;
  // Replays `recording` twice with the banks of `build` and compares the two (decisions; bits
  // when 2.5's conditions hold). `first` (optional) gets the first replay with its output.
  virtual ReplayComparison ReplayTwice(const Recording& recording, const DocBuild& build, ReplayRun* first) = 0;
  // Writes a bug report folder (description, two replays and their comparison, the recorded
  // run's log when given, the banks).
  virtual bool WriteReport(const std::string& dir, const Recording& recording, const DocBuild& build,
                           const std::string& title, const std::string& notes, const RecordedRunLog* run,
                           std::string* error) = 0;
  // Reads a bug report folder and replays it (twice, and against its expectation).
  virtual bool OpenReport(const std::string& dir, BugReport* report, ReplayComparison* comparison,
                          ExpectationResult* expectation, std::string* error) = 0;
  virtual bool WriteWav(const std::string& path, const ReplayRun& run, std::string* error) = 0;
  // Plays a replay's output on the output device (silent: a timer, no sound).
  virtual bool Listen(const ReplayRun& run, bool silent, std::string* error) = 0;
  virtual void StopListening() = 0;
  virtual bool Listening() const = 0;
};

// The implementation over Sonus::tool_preview (library sonus_tool_ui_replay).
std::unique_ptr<ReplayService> MakeReplayService();

}  // namespace snd::tool::ui

#endif  // SND_TOOL_UI_SERVICES_HPP
