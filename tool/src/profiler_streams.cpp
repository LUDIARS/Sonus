// The rest of the profiler (design.md 11.1), group P.
#include "snd/tool/profiler_streams.hpp"

#include <algorithm>

#include "snd/livelink/client.hpp"
#include "snd/livelink/runtime.hpp"

namespace snd::tool {
namespace ll = livelink;

std::uint32_t ProfilerMaskFor(std::uint32_t views, std::uint16_t peerMinor) {
  std::uint32_t m = 0;
  if ((views & kViewVoices) != 0u) m |= ll::kProfVoices;
  if ((views & (kViewCpu | kViewCpuDetail | kViewLod)) != 0u) m |= ll::kProfTiming;  // LOD level rides on BlockTiming
  if ((views & kViewMemory) != 0u) m |= ll::kProfMemory;
  if ((views & kViewStreams) != 0u) m |= ll::kProfStreams;
  if ((views & kViewLod) != 0u) m |= ll::kProfLod;
  if ((views & kViewEventLog) != 0u) m |= ll::kProfEventLog;
  if ((views & kViewMeters) != 0u) m |= ll::kProfMeters;
  if ((views & kViewStreams) != 0u) m |= ll::kProfStreamList;
  if ((views & kViewCpuDetail) != 0u) m |= ll::kProfBlockDetail;
  return m & ll::ProfilerMaskOf(peerMinor);
}

bool LogFilter::Matches(const ll::LogEntry& e) const {
  const bool isCommand = (e.kind & ll::kLogCommandBit) != 0u;
  const bool isEnd = !isCommand && e.kind == ll::kLogPlaybackEnded;
  const bool isEvent = !isCommand && !isEnd;
  if ((isCommand && !commands) || (isEnd && !ends) || (isEvent && !events)) return false;
  if (!kinds.empty() && std::find(kinds.begin(), kinds.end(), e.kind) == kinds.end()) return false;
  if (id != 0u && e.id != id) return false;
  if (slot != ~0u && e.handleSlot != slot) return false;
  return true;
}

std::string LogEntryKindName(std::uint32_t kind) {
  if (kind == ll::kLogPlaybackEnded) return "playback ended";
  return LogKindName(kind);
}

bool ProfilerStreams::Available(std::uint32_t view, std::string* why) const {
  if ((view & (kViewStreams | kViewCpuDetail)) != 0u && peerMinor_ < 1u) {
    if (why != nullptr) {
      *why = (view & kViewCpuDetail) != 0u ? "the game speaks protocol 1.0: no BlockDetail (1.1)"
                                           : "the game speaks protocol 1.0: no StreamList (1.1), stalls only";
    }
    return false;
  }
  if (why != nullptr) why->clear();
  return true;
}

void ProfilerStreams::Clear() {
  cpu_.clear();
  detail_.clear();
  hasMemory_ = false;
  streams_.clear();
  stalls_.clear();
  lods_.clear();
  log_.clear();
  lod_ = 0;
  cpuCount_ = detailCount_ = logCount_ = stallCount_ = lodCount_ = StreamCount{};
  dropped_ = 0;
}

namespace {

template <class T>
void Keep(std::vector<T>& v, std::size_t max, std::uint64_t* dropped) {
  if (v.size() <= max) return;
  const std::size_t n = v.size() - max;
  v.erase(v.begin(), v.begin() + static_cast<std::ptrdiff_t>(n));
  *dropped += n;
}

// Reads the entries numbered after *seq with `copy` (a Client::Copy*Since), in batches.
template <class Sample, class Copy, class Add>
void ReadSince(std::uint64_t* seq, StreamCount* count, Copy copy, Add add) {
  Sample buf[256];
  for (;;) {
    std::uint64_t first = 0;
    const std::uint32_t n = copy(*seq, buf, 256u, &first);
    if (first > *seq + 1u) count->missed += first - *seq - 1u;
    if (n == 0u) {
      if (first > *seq + 1u) *seq = first - 1u;
      return;
    }
    for (std::uint32_t i = 0; i < n; ++i) add(buf[i]);
    count->received += n;
    *seq = first + n - 1u;
    if (n < 256u) return;
  }
}

}  // namespace

void ProfilerStreams::Poll(ll::Client* client, std::uint64_t nowNs) {
  (void)nowNs;
  if (client == nullptr) {
    client_ = nullptr;
    return;
  }
  const ll::ClientSequences sq = client->Sequences();
  if (client != client_ || sq.timings < seqTimings_ || sq.eventLog < seqLog_ || sq.details < seqDetails_ ||
      sq.stalls < seqStalls_ || sq.lods < seqLods_) {
    client_ = client;  // a new client: its numbers start again
    seqTimings_ = seqDetails_ = seqLog_ = seqStalls_ = seqLods_ = 0;
    streamLists_ = memories_ = 0;
    subscribedConnects_ = ~0u;
  }
  // Subscription: when the views or the game changed.
  const ll::ClientStats st = client->Stats();
  if (client->State() == ll::ClientState::Ready) {
    peerMinor_ = client->PeerMinor();
    const std::uint32_t mask = ProfilerMaskFor(views_, peerMinor_);
    if (mask != subscribed_ || st.connects != subscribedConnects_) {
      if (client->SetProfilerMask(mask, desc_.meterHz, false) != 0u) {
        subscribed_ = mask;
        subscribedConnects_ = st.connects;
      }
    }
  }

  ReadSince<ll::TimingSample>(
      &seqTimings_, &cpuCount_,
      [&](std::uint64_t a, ll::TimingSample* o, std::uint32_t c, std::uint64_t* f) {
        return client->CopyTimingsSince(a, o, c, f);
      },
      [&](const ll::TimingSample& s) {
        CpuPoint p;
        p.receivedNs = s.receivedNs;
        p.timing = s.timing;
        const float b = s.timing.budgetMicroseconds;
        p.load = b > 0.0f ? (s.timing.logicMicroseconds + s.timing.renderMicroseconds) / b : 0.0f;
        cpu_.push_back(p);
        lod_ = s.timing.lodLevel;
      });
  ReadSince<ll::BlockDetailSample>(
      &seqDetails_, &detailCount_,
      [&](std::uint64_t a, ll::BlockDetailSample* o, std::uint32_t c, std::uint64_t* f) {
        return client->CopyBlockDetailsSince(a, o, c, f);
      },
      [&](const ll::BlockDetailSample& s) {
        CpuDetailPoint p;
        p.receivedNs = s.receivedNs;
        p.detail = s.detail;
        const float b = s.detail.budgetMicroseconds;
        p.load = b > 0.0f ? s.detail.totalMicroseconds / b : 0.0f;
        detail_.push_back(p);
      });
  ReadSince<ll::LogSample>(
      &seqLog_, &logCount_,
      [&](std::uint64_t a, ll::LogSample* o, std::uint32_t c, std::uint64_t* f) {
        return client->CopyEventLogSince(a, o, c, f);
      },
      [&](const ll::LogSample& s) { log_.push_back(LogPoint{s.receivedNs, s.entry}); });
  ReadSince<ll::StallSample>(
      &seqStalls_, &stallCount_,
      [&](std::uint64_t a, ll::StallSample* o, std::uint32_t c, std::uint64_t* f) {
        return client->CopyStallsSince(a, o, c, f);
      },
      [&](const ll::StallSample& s) { stalls_.push_back(StallPoint{s.receivedNs, s.stall}); });
  ReadSince<ll::LodSample>(
      &seqLods_, &lodCount_,
      [&](std::uint64_t a, ll::LodSample* o, std::uint32_t c, std::uint64_t* f) {
        return client->CopyLodsSince(a, o, c, f);
      },
      [&](const ll::LodSample& s) {
        lods_.push_back(LodPoint{s.receivedNs, s.lod});
        lod_ = s.lod.newLevel;
      });
  if (sq.memory != memories_) {
    memories_ = sq.memory;
    hasMemory_ = client->LatestMemory(&memory_);
  }
  if (sq.streamLists != streamLists_) {
    streamLists_ = sq.streamLists;
    streams_.resize(1024);
    const std::uint32_t n = client->CopyStreams(streams_.data(), static_cast<std::uint32_t>(streams_.size()));
    streams_.resize(std::min<std::size_t>(n, streams_.size()));
  }
  Keep(cpu_, desc_.maxCpu, &dropped_);
  Keep(detail_, desc_.maxDetail, &dropped_);
  Keep(log_, desc_.maxLog, &dropped_);
  Keep(stalls_, desc_.maxStalls, &dropped_);
  Keep(lods_, desc_.maxLods, &dropped_);
}

float ProfilerStreams::maxLoad() const {
  float m = 0.0f;
  for (const CpuPoint& p : cpu_) m = std::max(m, p.load);
  return m;
}

std::uint32_t ProfilerStreams::overBudget() const {
  std::uint32_t n = 0;
  for (const CpuPoint& p : cpu_) n += p.load > 1.0f ? 1u : 0u;
  return n;
}

std::vector<MemoryRow> ProfilerStreams::Memory() const {
  std::vector<MemoryRow> out;
  if (!hasMemory_) return out;
  for (std::uint32_t t = 0; t < SND_MEM_TAG_COUNT; ++t) {
    MemoryRow r;
    r.tag = t;
    r.name = MemTagName(t);
    r.used = memory_.used[t];
    r.budget = memory_.budget[t];
    r.fraction = r.budget != 0u ? static_cast<float>(static_cast<double>(r.used) / static_cast<double>(r.budget)) : 0.0f;
    out.push_back(r);
  }
  return out;
}

std::uint64_t ProfilerStreams::totalUsed() const {
  std::uint64_t n = 0;
  if (!hasMemory_) return 0;
  for (std::uint32_t t = 0; t < SND_MEM_TAG_COUNT; ++t) n += memory_.used[t];
  return n;
}

std::uint32_t ProfilerStreams::activeStreams() const {
  std::uint32_t n = 0;
  for (const ll::StreamInfo& s : streams_) n += s.state == static_cast<std::uint32_t>(ll::StreamState::Active) ? 1u : 0u;
  return n;
}

std::vector<LogPoint> ProfilerStreams::Log(const LogFilter& f) const {
  std::vector<LogPoint> out;
  for (const LogPoint& p : log_) {
    if (f.Matches(p.entry)) out.push_back(p);
  }
  return out;
}

}  // namespace snd::tool
