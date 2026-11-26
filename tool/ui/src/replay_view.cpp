// Recording and replay screen (design.md 2.5, 11.3): record on the connected game, save / load the
// recording file, its description (everything the record stream says), replaying it twice in the
// tool and the comparison (decisions; bits when 2.5's conditions hold), the comparison with the
// recorded run (the profiler's event log, LOD changes and stalls taken while recording), a WAV of
// the replay and listening to it, and the bug report folder (write; open and replay). Replays go
// through the host's ReplayService (services.hpp); without one the screen says so.
#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <memory>

#include "editor_util.hpp"
#include "snd/tool/doc_build.hpp"
#include "snd/tool/recording_report.hpp"
#include "snd/tool/ui/app.hpp"
#include "snd/tool/ui/kit.hpp"
#include "snd/tool/ui/layout_metrics.hpp"
#include "snd/tool/ui/services.hpp"
#include "snd/tool/ui/views.hpp"

namespace snd::tool::ui {
namespace {

constexpr float kRow = 26.0f;

std::vector<std::string> Lines(const std::string& text) {
  std::vector<std::string> v;
  std::size_t a = 0;
  while (a < text.size()) {
    const std::size_t b = text.find('\n', a);
    v.push_back(text.substr(a, b == std::string::npos ? std::string::npos : b - a));
    if (b == std::string::npos) break;
    a = b + 1;
  }
  return v;
}

// Recording files next to `path` (*.snrec in its folder, name order).
std::vector<PickerItem> RecordingsNear(const std::string& path) {
  namespace fs = std::filesystem;
  std::vector<PickerItem> v;
  std::error_code ec;
  fs::path dir = fs::path(path).parent_path();
  if (dir.empty()) dir = ".";
  for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
    if (it->path().extension() == ".snrec") v.push_back(PickerItem{it->path().filename().string(), it->path().string()});
    if (v.size() >= 1000u) break;
  }
  std::sort(v.begin(), v.end(), [](const PickerItem& a, const PickerItem& b) { return a.label < b.label; });
  return v;
}

// The recorded run as the tool saw it (profiler event log, the detail profiler's LOD and stalls).
RecordedRunLog RunLog(const Session& s) {
  RecordedRunLog run;
  for (const auto& l : s.profiler().log()) run.log.push_back(l.entry);
  const std::uint32_t block = s.connection().hello().blockSize != 0u ? s.connection().hello().blockSize : 256u;
  for (const LodPoint& p : s.streams().lods()) run.lods.push_back(RecordedLod{p.change.blockIndex, p.change.newLevel});
  for (const StallPoint& p : s.streams().stalls()) {
    run.stalls.push_back(RecordedStall{p.stall.clockSample / block, p.stall.streamSlot});
  }
  return run;
}

}  // namespace

void DeclareReplay(tela::Document& doc, ViewContext& ctx, float width, float height) {
  Kit kit(doc, ctx);
  App* app = ctx.app;
  if (app == nullptr) return;
  Session* s = &app->session();
  ReplayViewState* rv = &app->view().replay;
  ReplayService* service = app->workspace().replay;
  ProjectDocument* d = app->document();
  const Profiler& prof = s->profiler();
  const float lw = std::min(520.0f, width * 0.45f);
  Panel(doc, "rp.left", 0, 0, lw, height, [&] {
    const float w = lw - 12.0f;
    float y = 0.0f;
    kit.Title("rp.title", "recording (11.3)", 0, y, w);
    y += 24.0f;
    const RecordingState rs = prof.recordingState();
    const bool recording = rs == RecordingState::Recording || rs == RecordingState::Starting;
    App* a = app;
    kit.Action("rp.record", recording ? "Stop recording" : "Record", [a] { a->OnRecordToggle(); }, 0, y, 130);
    kit.Action("rp.save", "Save", [a] { a->OnSaveRecording(); }, 134, y, 60, 22, rs == RecordingState::Ready);
    kit.Action("rp.load", "Load", [a] { a->OnLoadRecording(); }, 198, y, 60);
    kit.Label("rp.state", std::string(RecordingStateName(rs)) + "  " + FormatBytes(prof.recordedBytes()), 262, y, w - 262, 22, 0.55f);
    y += kRow;
    kit.NameField("rp.path", "File", app->desc().recordingPath, [a](const std::string& p) {
      if (p.empty()) return false;
      a->SetRecordingPath(p);
      return true;
    }, 0, y, w - 110, 40);
    // The recordings in the same folder: pick one to load it.
    const std::string near = app->desc().recordingPath;
    kit.PickField("rp.list", "", "Recordings", "Recordings in the folder", [near] { return RecordingsNear(near); },
                  [a](const std::string& v) {
                    a->SetRecordingPath(v);
                    a->OnLoadRecording();
                  }, w - 106, y, 106);
    y += kRow + 4.0f;
    // The loaded recording, described.
    const bool loaded = !prof.loadedPath().empty();
    std::vector<std::string> lines;
    if (loaded) {
      lines.push_back("loaded " + prof.loadedPath());
      for (const std::string& l : Lines(DescribeRecording(prof.loaded()).Text())) lines.push_back(l);
    } else {
      lines.push_back("no recording loaded (Record, Save, Load)");
    }
    const std::uint32_t per = static_cast<std::uint32_t>(std::max(1.0f, (height - y - 30.0f) / 18.0f));
    const std::uint32_t first = kit.Pager("rp.desc.pager", static_cast<std::uint32_t>(lines.size()), per, &rv->page, w - 200, y, 200);
    if (lines.size() > per) y += 22.0f;
    for (std::uint32_t i = first; i < lines.size() && i < first + per; ++i) {
      kit.Label("rp.desc." + std::to_string(i - first), Clip(lines[i], 110), 0, y, w, 18, 0.5f);
      y += 18.0f;
    }
  }, 6.0f);
  const float rx = lw + 8.0f, rw = width - rx;
  Panel(doc, "rp.right", rx, 0, rw, height, [&] {
    const float w = rw - 12.0f;
    float y = 0.0f;
    const bool loaded = !prof.loadedPath().empty();
    std::string why;
    if (service == nullptr) why = "no replay service (the host did not give one)";
    else if (d == nullptr) why = "no project document (the banks to replay come from its build)";
    else if (!loaded) why = "load a recording first";
    const bool can = why.empty();
    kit.Title("rp.replay.title", "replay in the tool (2.5)", 0, y, w);
    y += 24.0f;
    kit.Action("rp.twice", "Replay twice and compare", [service, d, s, rv, a = app] {
      const DocBuild build = BuildDocument(*d);
      if (!build.ok) {
        a->Message("replay: the project does not build");
        return;
      }
      auto run = std::make_shared<ReplayRun>();
      const ReplayComparison c = service->ReplayTwice(s->profiler().loaded(), build, run.get());
      rv->result = Lines(c.Text());
      const RecordedRunComparison rc =
          CompareWithRecordedRun(*run, DescribeRecording(s->profiler().loaded()), RunLog(*s));
      for (const std::string& l : Lines(rc.Text())) rv->result.push_back(l);
      if (!run->ok) rv->result.push_back("replay failed: " + run->error);
      rv->lastRun = run;
      a->Message(c.ok ? "replay: the two replays agree" : "replay: the replays differ");
    }, 0, y, 200, 22, can);
    const bool haveRun = rv->lastRun != nullptr && rv->lastRun->ok && !rv->lastRun->output.empty();
    kit.Action("rp.wav", "Write WAV", [service, rv, a = app] {
      std::string err;
      if (service->WriteWav(rv->wavPath, *rv->lastRun, &err)) a->Message("wrote " + rv->wavPath);
      else a->Message("WAV: " + err);
    }, 204, y, 90, 22, service != nullptr && haveRun);
    const bool listening = service != nullptr && service->Listening();
    kit.Action("rp.listen", listening ? "Stop" : "Listen", [service, rv, listening, a = app] {
      if (listening) return service->StopListening();
      std::string err;
      if (!service->Listen(*rv->lastRun, false, &err)) a->Message("listen: " + err);
    }, 298, y, 70, 22, service != nullptr && (haveRun || listening));
    y += kRow;
    kit.NameField("rp.wavpath", "WAV", rv->wavPath, [rv](const std::string& p) {
      if (p.empty()) return false;
      rv->wavPath = p;
      return true;
    }, 0, y, w, 40);
    y += kRow;
    if (!can) {
      kit.Label("rp.why", "replay: " + why, 0, y, w, 20, 0.5f);
      y += 22.0f;
    }
    // Bug report.
    kit.Title("rp.report.title", "bug report", 0, y, w);
    y += 24.0f;
    kit.NameField("rp.report.title.text", "Title", rv->title, [rv](const std::string& v) {
      rv->title = v;
      return true;
    }, 0, y, w, 50);
    y += kRow;
    kit.NameField("rp.report.notes", "Notes", rv->notes, [rv](const std::string& v) {
      rv->notes = v;
      return true;
    }, 0, y, w, 50);
    y += kRow;
    kit.NameField("rp.report.dir", "Folder", rv->reportDir, [rv](const std::string& v) {
      if (v.empty()) return false;
      rv->reportDir = v;
      return true;
    }, 0, y, w, 50);
    y += kRow;
    kit.Action("rp.report.write", "Write report", [service, d, s, rv, a = app] {
      const DocBuild build = BuildDocument(*d);
      std::string err;
      const RecordedRunLog run = RunLog(*s);
      if (!service->WriteReport(rv->reportDir, s->profiler().loaded(), build, rv->title, rv->notes, &run, &err)) {
        a->Message("report: " + err);
        return;
      }
      a->Message("wrote the bug report " + rv->reportDir);
    }, 0, y, 120, 22, can);
    kit.Action("rp.report.open", "Open and replay", [service, rv, a = app] {
      BugReport report;
      ReplayComparison c;
      ExpectationResult e;
      std::string err;
      if (!service->OpenReport(rv->reportDir, &report, &c, &e, &err)) {
        a->Message("report: " + err);
        return;
      }
      rv->result.clear();
      rv->result.push_back("report: " + report.title);
      for (const std::string& l : Lines(report.summary)) rv->result.push_back(l);
      for (const std::string& l : Lines(c.Text())) rv->result.push_back(l);
      rv->result.push_back(std::string("expectation: ") + (e.ok ? "met" : "not met"));
      for (const std::string& m : e.messages) rv->result.push_back("  " + m);
    }, 124, y, 140, 22, service != nullptr);
    y += kRow + 6.0f;
    // The last result, line by line.
    kit.Label("rp.result.title", "result", 0, y, w, 20, 0.6f);
    y += 22.0f;
    for (std::size_t i = 0; i < rv->result.size() && y < height - 30.0f; ++i) {
      kit.Label("rp.result." + std::to_string(i), Clip(rv->result[i], 120), 0, y, w, 18, 0.5f);
      y += 18.0f;
    }
  }, 6.0f);
}

}  // namespace snd::tool::ui
