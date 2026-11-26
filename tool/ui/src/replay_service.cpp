// ReplayService over the preview library (Sonus::tool_preview): library sonus_tool_ui_replay.
#include <memory>

#include "snd/tool/preview/replay_host.hpp"
#include "snd/tool/ui/services.hpp"

namespace snd::tool::ui {
namespace {

class PreviewReplayService final : public ReplayService {
 public:
  ReplayComparison ReplayTwice(const Recording& recording, const DocBuild& build, ReplayRun* first) override {
    preview::ReplayOptions o;
    o.keepOutput = first != nullptr;
    return preview::ReplayTwice(recording, preview::BanksOf(build), o, first, nullptr);
  }
  bool WriteReport(const std::string& dir, const Recording& recording, const DocBuild& build, const std::string& title,
                   const std::string& notes, const RecordedRunLog* run, std::string* error) override {
    const BugReport report =
        preview::MakeBugReport(recording, preview::BanksOf(build), nullptr, title, notes, true, run);
    return snd::tool::WriteBugReport(dir, report, error);
  }
  bool OpenReport(const std::string& dir, BugReport* report, ReplayComparison* comparison,
                  ExpectationResult* expectation, std::string* error) override {
    if (!ReadBugReport(dir, report, error)) return false;
    *comparison = preview::ReplayBugReport(*report, expectation, nullptr);
    return true;
  }
  bool WriteWav(const std::string& path, const ReplayRun& run, std::string* error) override {
    return preview::WriteWav(path, run.output, run.channels, run.sampleRate, error);
  }
  bool Listen(const ReplayRun& run, bool silent, std::string* error) override {
    player_ = std::make_unique<preview::OutputPlayer>();
    if (!player_->Start(run.output, run.channels, run.sampleRate, silent, error)) {
      player_.reset();
      return false;
    }
    return true;
  }
  void StopListening() override {
    if (player_ != nullptr) player_->Stop();
    player_.reset();
  }
  bool Listening() const override { return player_ != nullptr && !player_->Done(); }

 private:
  std::unique_ptr<preview::OutputPlayer> player_;
};

}  // namespace

std::unique_ptr<ReplayService> MakeReplayService() { return std::make_unique<PreviewReplayService>(); }

}  // namespace snd::tool::ui
