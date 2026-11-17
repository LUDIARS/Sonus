// The tool's models together.
#include "snd/tool/session.hpp"

#include "snd/livelink/client.hpp"
#include "snd/tool/names.hpp"
#include "snd/version.h"

namespace snd::tool {

bool Session::LoadProject(const std::string& dir, const std::string& buildDir) {
  bool ok = project_.Load(dir, buildDir);
  if (ok && !buildDir.empty()) ok = project_.Build(buildDir);
  for (const ProjectIssue& i : project_.issues()) {
    if (i.error) Message((i.where.empty() ? std::string() : i.where + ": ") + i.message);
  }
  mixer_.Bind(project_);
  sync_.Reset();
  return ok;
}

void Session::UseProject(const Project& project) {
  project_ = project;
  mixer_.Bind(project_);
  sync_.Reset();
}

int Session::Connect(ConnectionDesc desc) {
  if (desc.projectBuildHash == 0u) desc.projectBuildHash = project_.buildHash();
  const int r = connection_.Open(desc);
  if (r != 0) Message(std::string("connect failed: ") + ResultName(r));
  return r;
}

void Session::Disconnect() { connection_.Close(); }

void Session::Tick(std::uint64_t nowNs) {
  ++ticks_;
  const ConnectionEvents ev = connection_.Poll();
  if (ev.sessionLost) Message("connection lost (" + connection_.Target() + ")");
  if (ev.sessionStarted) {
    Message(connection_.mismatch() ? "connected: build mismatch, sending the tool's values"
                                   : "connected to " + connection_.Target());
  }
  livelink::Client* c = connection_.client();
  const bool sendAll = ev.sessionStarted && connection_.mismatch();
  sync_.Update(c, mixer_.Cells(), mixer_.ParamCells(), ev.sessionStarted, sendAll);
  if (sync_.stats().refused != refusedSeen_) {
    refusedSeen_ = sync_.stats().refused;
    Message(std::string("value refused by the game: ") + ResultName(sync_.stats().lastError));
  }
  profiler_.Poll(c, nowNs);
}

RecordingInfo Session::CurrentRecordingInfo() const {
  RecordingInfo i;
  const livelink::HelloReply& h = connection_.hello();
  i.projectBuildHash = project_.buildHash();
  i.runtimeBuildHash = h.runtimeBuildHash;
  i.sampleRate = h.sampleRate;
  i.blockSize = h.blockSize;
  i.platform = h.platform;
  i.contractVersion = h.contractVersion;
  return i;
}

bool Session::SaveRecording(const std::string& path) {
  std::string err;
  if (!profiler_.SaveRecording(connection_.client(), CurrentRecordingInfo(), path, &err)) {
    Message("save failed: " + err);
    return false;
  }
  Message("recording saved: " + path);
  return true;
}

bool Session::LoadRecording(const std::string& path) {
  std::string err;
  if (!profiler_.LoadRecording(path, &err)) {
    Message("load failed: " + err);
    return false;
  }
  Message("recording loaded: " + path);
  return true;
}

void Session::Message(std::string text) {
  messages_.push_back(std::move(text));
  if (messages_.size() > 64u) messages_.erase(messages_.begin());
}

}  // namespace snd::tool
