// Profiler model (design.md 11.1, 11.3).
#include "snd/tool/profiler.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "snd/livelink/client.hpp"
#include "snd/types.h"

namespace snd::tool {
namespace {

// `linear` lowered by `db` (0 below the meter floor).
float Fallen(float linear, float db) {
  if (db <= 0.0f) return linear;
  const float d = LinearToDb(linear) - db;
  return d <= kMeterFloorDb ? 0.0f : std::pow(10.0f, d / 20.0f);
}

}  // namespace

const char* RecordingStateName(RecordingState s) {
  switch (s) {
    case RecordingState::Idle: return "idle";
    case RecordingState::Starting: return "starting";
    case RecordingState::Recording: return "recording";
    case RecordingState::Stopping: return "stopping";
    case RecordingState::Ready: return "ready";
    case RecordingState::Failed: return "failed";
  }
  return "?";
}

void Profiler::Clear() {
  timings_.clear();
  voices_.clear();
  meters_.clear();
  holdIds_.clear();
  holds_.clear();
  lastMeterNs_ = 0;
  hasMemory_ = false;
  log_.clear();
  stalls_ = lods_ = 0;
  std::memset(counts_, 0, sizeof(counts_));
}

void Profiler::Poll(livelink::Client* client, std::uint64_t nowNs) {
  if (client == nullptr) return;
  // timings
  std::vector<livelink::TimingSample> ts(desc_.maxTimings);
  const std::uint32_t nt = client->CopyTimings(ts.data(), desc_.maxTimings);
  timings_.resize(nt);
  for (std::uint32_t i = 0; i < nt; ++i) timings_[i] = TimingPoint{ts[i].receivedNs, ts[i].timing};
  // voices (loudest first; stable for equal audibility)
  voices_.resize(desc_.maxVoices);
  const std::uint32_t nv = client->CopyVoices(voices_.data(), desc_.maxVoices);
  voices_.resize(std::min(nv, desc_.maxVoices));
  std::stable_sort(voices_.begin(), voices_.end(), [](const livelink::VoiceInfo& a, const livelink::VoiceInfo& b) {
    const bool va = a.isVirtual != 0u, vb = b.isVirtual != 0u;  // any non-zero value is "virtual"
    if (va != vb) return !va;
    return a.audibility > b.audibility;
  });
  // meters with peak hold
  std::vector<livelink::MeterEntry> me(desc_.maxMeters);
  std::uint64_t meterNs = 0;
  const std::uint32_t nm = std::min(client->CopyMeters(me.data(), desc_.maxMeters, &meterNs), desc_.maxMeters);
  // A snapshot is new when its receive time changed: only a new one feeds the peak hold.
  // A snapshot older than kStaleMeterNs is stale (the game stopped sending): its bars fall
  // like the hold (fallDbPerSecond after kStaleMeterNs) and the hold keeps falling to -inf.
  const bool fresh = meterNs != lastMeterNs_;
  lastMeterNs_ = meterNs;
  const std::uint64_t age = nowNs > meterNs ? nowNs - meterNs : 0u;
  const float staleFallDb = age > kStaleMeterNs ? static_cast<float>(static_cast<double>(age - kStaleMeterNs) * 1e-9 *
                                                                     desc_.hold.fallDbPerSecond)
                                                : 0.0f;
  meters_.resize(nm);
  for (std::uint32_t i = 0; i < nm; ++i) {
    BusMeter& m = meters_[i];
    // The first entry is the output (System::LiveMeters). Since the live link's
    // independent-test fixes it carries the master bus's own id; the tool's model
    // keeps 0 for the output (FindMeter(0), the master strip).
    m.busId = i == 0u ? 0u : me[i].busId;
    m.channels = std::min<std::uint32_t>(me[i].channels, SND_MAX_CHANNELS);
    m.receivedNs = meterNs;
    m.stale = staleFallDb > 0.0f;
    std::size_t slot = 0;
    while (slot < holdIds_.size() && holdIds_[slot] != m.busId) ++slot;
    if (slot == holdIds_.size()) {
      holdIds_.push_back(m.busId);
      holds_.resize(holdIds_.size() * SND_MAX_CHANNELS);
    }
    m.clipped = false;
    for (std::uint32_t c = 0; c < SND_MAX_CHANNELS; ++c) {
      m.peak[c] = c < m.channels ? Fallen(me[i].peak[c], staleFallDb) : 0.0f;
      m.rms[c] = c < m.channels ? Fallen(me[i].rms[c], staleFallDb) : 0.0f;
      PeakHold& h = holds_[slot * SND_MAX_CHANNELS + c];
      if (c < m.channels && fresh) h.Update(me[i].peak[c], nowNs, desc_.hold);
      m.holdDb[c] = h.HeldDb(nowNs, desc_.hold);
      m.clipped = m.clipped || h.clipped();
    }
  }
  // memory
  hasMemory_ = client->LatestMemory(&memory_);
  // event log
  std::vector<livelink::LogSample> ls(desc_.maxLog);
  const std::uint32_t nl = std::min(client->CopyEventLog(ls.data(), desc_.maxLog), desc_.maxLog);
  log_.resize(nl);
  for (std::uint32_t i = 0; i < nl; ++i) log_[i] = LogLine{ls[i].receivedNs, ls[i].entry};
  // stalls / LOD changes (counts over the history window)
  std::vector<livelink::StallSample> ss(256);
  stalls_ = client->CopyStalls(ss.data(), 256);
  std::vector<livelink::LodSample> lo(256);
  lods_ = client->CopyLods(lo.data(), 256);
  for (std::uint16_t t = 0; t < 8; ++t) {
    counts_[t] = client->Received(static_cast<livelink::MessageType>(0x0100u + t));
  }
  PollRecording(client, nowNs);
}

TimingSummary Profiler::Summary() const {
  TimingSummary s;
  s.samples = static_cast<std::uint32_t>(timings_.size());
  if (timings_.empty()) return s;
  double sumR = 0.0, sumL = 0.0;
  for (const TimingPoint& p : timings_) {
    sumR += p.timing.renderMicroseconds;
    sumL += p.timing.logicMicroseconds;
    s.maxRenderUs = std::max(s.maxRenderUs, p.timing.renderMicroseconds);
    if (p.timing.budgetMicroseconds > 0.0f &&
        p.timing.logicMicroseconds + p.timing.renderMicroseconds > p.timing.budgetMicroseconds) {
      ++s.overBudget;
    }
  }
  const livelink::BlockTiming& last = timings_.back().timing;
  s.lastBlock = last.blockIndex;
  s.lastRenderUs = last.renderMicroseconds;
  s.lastLogicUs = last.logicMicroseconds;
  s.avgRenderUs = static_cast<float>(sumR / static_cast<double>(timings_.size()));
  s.avgLogicUs = static_cast<float>(sumL / static_cast<double>(timings_.size()));
  s.budgetUs = last.budgetMicroseconds;
  s.loadPercent = last.budgetMicroseconds > 0.0f
                      ? 100.0f * (last.logicMicroseconds + last.renderMicroseconds) / last.budgetMicroseconds
                      : 0.0f;
  s.lod = last.lodLevel;
  s.lateChunks = last.lateChunks;
  s.underruns = last.underruns;
  return s;
}

std::uint32_t Profiler::realVoices() const {
  std::uint32_t n = 0;
  for (const livelink::VoiceInfo& v : voices_) n += v.isVirtual == 0u ? 1u : 0u;
  return n;
}

const BusMeter* Profiler::FindMeter(std::uint32_t busId) const {
  for (const BusMeter& m : meters_) {
    if (m.busId == busId) return &m;
  }
  return nullptr;
}

std::uint64_t Profiler::received(livelink::MessageType t) const {
  const std::uint32_t i = static_cast<std::uint32_t>(t) - 0x0100u;
  return i < 8u ? counts_[i] : 0u;
}

void Profiler::ResetClips() {
  for (PeakHold& h : holds_) h.Reset();
}

bool Profiler::StartRecording(livelink::Client* client) {
  if (client == nullptr || recState_ == RecordingState::Starting || recState_ == RecordingState::Recording) {
    return false;
  }
  const std::uint32_t seq = client->StartRecording(true);
  if (seq == 0u) return false;
  recSeq_ = seq;
  recState_ = RecordingState::Starting;
  recError_ = 0;
  recBytes_ = 0;
  return true;
}

bool Profiler::StopRecording(livelink::Client* client) {
  if (client == nullptr || (recState_ != RecordingState::Recording && recState_ != RecordingState::Starting)) {
    return false;
  }
  const std::uint32_t seq = client->StopRecording(true);
  if (seq == 0u) return false;
  recSeq_ = seq;
  recState_ = RecordingState::Stopping;
  return true;
}

void Profiler::PollRecording(livelink::Client* client, std::uint64_t nowNs) {
  const std::uint64_t bytes = client->RecordingBytes();
  if (bytes != recBytes_) {
    recBytes_ = bytes;
    recChangeNs_ = nowNs;
  }
  if (recSeq_ != 0u) {
    std::int32_t r = 0;
    if (client->TakeAck(recSeq_, &r)) {
      recSeq_ = 0;
      if (r != SND_OK) {
        recState_ = RecordingState::Failed;
        recError_ = r;
      } else if (recState_ == RecordingState::Starting) {
        recState_ = RecordingState::Recording;
      }
      recChangeNs_ = nowNs;
    }
  }
  if (recState_ == RecordingState::Stopping && recSeq_ == 0u &&
      nowNs - recChangeNs_ >= std::uint64_t{desc_.settleMs} * 1000000u) {
    recState_ = RecordingState::Ready;
  }
}

bool Profiler::TakeRecording(livelink::Client* client, const RecordingInfo& info, Recording* out) const {
  if (client == nullptr) return false;
  Recording r;
  r.info = info;
  if (client->Stats().recordOverflow != 0u) r.info.flags |= kRecordingOverflow;
  r.records.resize(client->RecordingBytes());
  const std::uint64_t n = client->CopyRecording(r.records.data(), r.records.size());
  r.records.resize(n);
  *out = std::move(r);
  return true;
}

bool Profiler::SaveRecording(livelink::Client* client, const RecordingInfo& info, const std::string& path,
                             std::string* error) {
  Recording r;
  if (!TakeRecording(client, info, &r)) {
    if (error != nullptr) *error = "not connected";
    return false;
  }
  return tool::SaveRecording(path, r, error);
}

bool Profiler::LoadRecording(const std::string& path, std::string* error) {
  Recording r;
  if (!tool::LoadRecording(path, &r, error)) return false;
  loaded_ = std::move(r);
  loadedSummary_ = Summarize(loaded_.records.data(), loaded_.records.size());
  loadedPath_ = path;
  return true;
}

}  // namespace snd::tool
