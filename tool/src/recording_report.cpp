// Recordings for bug reports and tests (design.md 2.5, 11.3), group P.
#include "snd/tool/recording_report.hpp"

#include <algorithm>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <sstream>
#include <type_traits>

#include "snd/internal/commands.hpp"
#include "snd/livelink/record_format.hpp"
#include "snd/livelink/runtime.hpp"
#include "snd/tool/profiler_streams.hpp"

namespace snd::tool {
namespace ll = livelink;
namespace fs = std::filesystem;

namespace {

std::string Hex(std::uint64_t v) {
  char b[24];
  std::snprintf(b, sizeof b, "%016" PRIx64, v);
  return b;
}

bool ParseU64(const std::string& s, std::uint64_t* out) {
  if (s.empty()) return false;
  char* end = nullptr;
  const bool hex = s.size() > 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X');
  const unsigned long long v = std::strtoull(s.c_str() + (hex ? 2 : 0), &end, hex ? 16 : 10);
  if (end == nullptr || *end != '\0') return false;
  *out = static_cast<std::uint64_t>(v);
  return true;
}

bool ParseF32(const std::string& s, float* out) {
  if (s.empty()) return false;
  char* end = nullptr;
  const float v = std::strtof(s.c_str(), &end);
  if (end == nullptr || *end != '\0') return false;
  *out = v;
  return true;
}

std::string Trim(const std::string& s) {
  std::size_t a = 0, b = s.size();
  while (a < b && (s[a] == ' ' || s[a] == '\t' || s[a] == '\r')) ++a;
  while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r')) --b;
  return s.substr(a, b - a);
}

bool ReadText(const std::string& path, std::string* out) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return false;
  out->assign((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  return true;
}

bool WriteBytes(const std::string& path, const void* data, std::size_t n) {
  std::ofstream f(path, std::ios::binary | std::ios::trunc);
  if (!f) return false;
  f.write(static_cast<const char*>(data), static_cast<std::streamsize>(n));
  return static_cast<bool>(f);
}

// --- snd_config <-> text (every numeric member, X macros of config.h) ---------------------
template <class T>
void EmitField(std::string& out, const std::string& name, const T& v) {
  if constexpr (std::is_pointer_v<T>) {
    (void)out;
    (void)name;
    (void)v;
  } else if constexpr (std::is_floating_point_v<T>) {
    char b[64];
    std::snprintf(b, sizeof b, "%.9g", static_cast<double>(v));
    out += name + " = " + b + "\n";
  } else {
    out += name + " = " + std::to_string(static_cast<unsigned long long>(v)) + "\n";
  }
}

template <class T>
bool SetField(const std::string& value, T* field) {
  if constexpr (std::is_pointer_v<T>) {
    (void)value;
    (void)field;
    return false;
  } else if constexpr (std::is_floating_point_v<T>) {
    float f = 0.0f;
    if (!ParseF32(value, &f)) return false;
    *field = static_cast<T>(f);
    return true;
  } else {
    std::uint64_t u = 0;
    if (!ParseU64(value, &u)) return false;
    *field = static_cast<T>(u);
    return true;
  }
}

}  // namespace

std::string ConfigText(const snd_config& c) {
  std::string out;
#define SND_EMIT_TOP(cn, cpp, ct, cppt, d) EmitField(out, #cn, c.cn);
  SND_CONFIG_TOP(SND_EMIT_TOP)
  SND_CONFIG_TOP_1_1(SND_EMIT_TOP)
  SND_CONFIG_TOP_1_2(SND_EMIT_TOP)
  EmitField(out, "output_kind", c.output_kind);
#define SND_EMIT_MEMORY(cn, cpp, ct, cppt, d) EmitField(out, "memory." #cn, c.memory.cn);
#define SND_EMIT_COMMANDS(cn, cpp, ct, cppt, d) EmitField(out, "commands." #cn, c.commands.cn);
#define SND_EMIT_JOBS(cn, cpp, ct, cppt, d) EmitField(out, "jobs." #cn, c.jobs.cn);
#define SND_EMIT_TUNING(cn, cpp, ct, cppt, d) EmitField(out, "tuning." #cn, c.tuning.cn);
#define SND_EMIT_DEV(cn, cpp, ct, cppt, d) EmitField(out, "dev_reserve." #cn, c.dev_reserve.cn);
#define SND_EMIT_LL(cn, cpp, ct, cppt, d) EmitField(out, "livelink." #cn, c.livelink.cn);
  SND_CONFIG_MEMORY(SND_EMIT_MEMORY)
  SND_CONFIG_COMMANDS(SND_EMIT_COMMANDS)
  SND_CONFIG_JOBS(SND_EMIT_JOBS)
  SND_CONFIG_TUNING(SND_EMIT_TUNING)
  SND_CONFIG_DEV_RESERVE(SND_EMIT_DEV)
  SND_CONFIG_LIVELINK(SND_EMIT_LL)
#undef SND_EMIT_TOP
#undef SND_EMIT_MEMORY
#undef SND_EMIT_COMMANDS
#undef SND_EMIT_JOBS
#undef SND_EMIT_TUNING
#undef SND_EMIT_DEV
#undef SND_EMIT_LL
  return out;
}

bool ParseConfigLine(const std::string& name, const std::string& value, snd_config* c) {
#define SND_PARSE_TOP(cn, cpp, ct, cppt, d) \
  if (name == #cn) return SetField(value, &c->cn);
#define SND_PARSE_MEMORY(cn, cpp, ct, cppt, d) \
  if (name == "memory." #cn) return SetField(value, &c->memory.cn);
#define SND_PARSE_COMMANDS(cn, cpp, ct, cppt, d) \
  if (name == "commands." #cn) return SetField(value, &c->commands.cn);
#define SND_PARSE_JOBS(cn, cpp, ct, cppt, d) \
  if (name == "jobs." #cn) return SetField(value, &c->jobs.cn);
#define SND_PARSE_TUNING(cn, cpp, ct, cppt, d) \
  if (name == "tuning." #cn) return SetField(value, &c->tuning.cn);
#define SND_PARSE_DEV(cn, cpp, ct, cppt, d) \
  if (name == "dev_reserve." #cn) return SetField(value, &c->dev_reserve.cn);
#define SND_PARSE_LL(cn, cpp, ct, cppt, d) \
  if (name == "livelink." #cn) return SetField(value, &c->livelink.cn);
  SND_CONFIG_TOP(SND_PARSE_TOP)
  SND_CONFIG_TOP_1_1(SND_PARSE_TOP)
  SND_CONFIG_TOP_1_2(SND_PARSE_TOP)
  if (name == "output_kind") return SetField(value, &c->output_kind);
  SND_CONFIG_MEMORY(SND_PARSE_MEMORY)
  SND_CONFIG_COMMANDS(SND_PARSE_COMMANDS)
  SND_CONFIG_JOBS(SND_PARSE_JOBS)
  SND_CONFIG_TUNING(SND_PARSE_TUNING)
  SND_CONFIG_DEV_RESERVE(SND_PARSE_DEV)
  SND_CONFIG_LIVELINK(SND_PARSE_LL)
#undef SND_PARSE_TOP
#undef SND_PARSE_MEMORY
#undef SND_PARSE_COMMANDS
#undef SND_PARSE_JOBS
#undef SND_PARSE_TUNING
#undef SND_PARSE_DEV
#undef SND_PARSE_LL
  return false;
}

// --- describing --------------------------------------------------------------------------------
RecordingDescription DescribeRecording(const std::uint8_t* data, std::uint64_t bytes) {
  RecordingDescription d;
  d.summary = Summarize(data, bytes);
  d.commandsByType.assign(static_cast<std::size_t>(cmd::CommandType::Count), 0u);
  ll::RecordReader reader(data, bytes);
  ll::RecordItem item;
  bool sawArena = false;
  while (reader.Next(&item)) {
    const std::uint32_t kind = static_cast<std::uint32_t>(item.kind);
    if (item.kind == ll::RecordKind::Arena) {
      sawArena = true;
      auto count = [&](const std::uint8_t* p, std::uint32_t n) {
        if (p == nullptr || n == 0u) return;
        cmd::ForEachCommand(p, p + n, [&](const cmd::CommandHeader& h, const std::uint8_t*) {
          ++d.commands;
          if (h.type < d.commandsByType.size()) ++d.commandsByType[h.type];
        });
      };
      count(item.normal, item.sections.normalBytes);
      count(item.reserve, item.sections.reserveBytes);
      if (item.fallback != nullptr && item.sections.fallbackBytes >= sizeof(ll::FallbackHeader)) {
        ++d.fallbacks;
        if (d.capPlaybacks == 0u && d.capBanks == 0u) {
          ll::FallbackHeader fh{};
          std::memcpy(&fh, item.fallback, sizeof fh);
          d.capPlaybacks = fh.playbackBits;
          d.capEmitters = fh.emitterBits;
          d.capTransports = fh.transportBits;
          d.capUserBuffers = fh.userBufferBits;
          d.capBanks = fh.bankBits;
        }
      }
    } else if (item.kind == ll::RecordKind::Lod) {
      d.lods.push_back(RecordedLod{item.lod.block, item.lod.level});
    } else if (item.kind == ll::RecordKind::Stall) {
      d.stalls.push_back(RecordedStall{item.stall.block, item.stall.streamSlot});
    } else if (item.kind == ll::RecordKind::BankMap) {
      d.banks.push_back(RecordedBank{item.bankMap.bankId, item.bankMap.contentHash, !sawArena});
    } else if (kind == ll::kRecordBegin) {
      d.hasBegin = true;
      d.origin = item.begin.origin;
      d.lostState = item.begin.lostState;
      d.beginBlock = item.begin.block;
    } else if (kind == ll::kRecordCell) {
      ++d.startCells;
    } else if (kind == ll::kRecordOverride) {
      ++d.startOverrides;
    } else if (kind == ll::kRecordLiveData) {
      ++d.liveData;
      if (item.liveData.fourcc == ll::kStructEditTag) {
        ++d.structEdits;
      } else {
        ++d.replacedChunks;
      }
    }
  }
  return d;
}

std::string RecordingDescription::Text() const {
  std::ostringstream o;
  if (!summary.valid) {
    o << "unreadable recording: " << summary.error << " at byte " << summary.errorOffset << "\n";
  }
  o << "bytes: " << summary.bytes << "\n";
  o << "blocks: " << summary.firstBlock << " .. " << summary.lastBlock << "\n";
  if (hasBegin) {
    o << "start: " << (origin == static_cast<std::uint32_t>(ll::RecordOrigin::FromCreation) ? "from creation" : "mid-run")
      << " at block " << beginBlock << ", " << startCells << " override cells, " << startOverrides
      << " live overrides";
    if (lostState != 0u) o << ", " << lostState << " starting-state items lost";
    o << "\n";
  } else {
    o << "start: unknown (protocol 1.0 recording, no Begin record)\n";
  }
  o << "arenas: " << summary.arenas << ", commands: " << commands;
  if (fallbacks != 0u) o << ", overflow fallbacks: " << fallbacks;
  o << "\n";
  for (std::size_t t = 1; t < commandsByType.size(); ++t) {
    if (commandsByType[t] == 0u) continue;
    const cmd::CommandInfo* i = cmd::FindCommandInfo(static_cast<std::uint16_t>(t));
    o << "  " << (i != nullptr ? i->name : "?") << ": " << commandsByType[t] << "\n";
  }
  o << "live data: " << liveData << " (" << replacedChunks << " chunk replacements, " << structEdits
    << " structural edits)\n";
  o << "LOD records: " << lods.size();
  for (const RecordedLod& l : lods) o << " [block " << l.block << " level " << l.level << "]";
  o << "\n";
  o << "stalls: " << stalls.size();
  for (const RecordedStall& s : stalls) o << " [block " << s.block << " slot " << s.slot << "]";
  o << "\n";
  o << "banks:";
  for (const RecordedBank& b : banks) {
    o << " [" << Hex(b.bankId).substr(8) << " content " << Hex(b.contentHash) << (b.atStart ? "" : " (during)") << "]";
  }
  o << "\n";
  return o.str();
}

snd_config ReplayConfigFor(const Recording& r) {
  snd_config c;
  snd_config_init(&c);
  if (r.info.sampleRate != 0u) c.sample_rate = r.info.sampleRate;
  if (r.info.blockSize != 0u) c.block_size = r.info.blockSize;
  const RecordingDescription d = DescribeRecording(r);
  if (d.capPlaybacks != 0u) c.max_playbacks = d.capPlaybacks;
  if (d.capEmitters != 0u) c.max_emitters = d.capEmitters;
  if (d.capTransports != 0u) c.max_transports = d.capTransports;
  if (d.capUserBuffers != 0u) c.max_user_buffers = d.capUserBuffers;
  if (d.capBanks != 0u) c.max_banks = d.capBanks;
  c.tuning.lod_down_blocks = 0;  // 2.5: LOD disabled
  c.jobs.realtime_workers = 0;
  c.livelink.enabled = 0;
  return c;
}

// --- replays --------------------------------------------------------------------------------
std::vector<std::string> ConditionReasons(std::uint32_t c) {
  std::vector<std::string> r;
  if ((c & ll::kReplayLodRecorded) != 0u) r.push_back("the recording has load LOD levels / changes (not reproduced)");
  if ((c & ll::kReplayStallRecorded) != 0u) r.push_back("the recording has stream stalls (not reproduced)");
  if ((c & ll::kReplayMidRun) != 0u) r.push_back("recorded mid-run: core state before it is not in the recording");
  if ((c & ll::kReplayNoBegin) != 0u) r.push_back("a protocol 1.0 recording: no starting state");
  if ((c & ll::kReplayStateLost) != 0u) r.push_back("starting-state items did not fit when recording");
  if ((c & ll::kReplaySkipped) != 0u) r.push_back("commands left out (not replayable or invalid)");
  if ((c & ll::kReplayStructEdits) != 0u) r.push_back("structural edits not replayed (no editor)");
  if ((c & ll::kReplayBankChanges) != 0u) r.push_back("banks changed during the recording (hot reload)");
  if ((c & ll::kReplayLodInReplay) != 0u) r.push_back("the replay changed its LOD level");
  if ((c & ll::kReplayStallInReplay) != 0u) r.push_back("the replay stalled");
  if ((c & ll::kReplayStartStateFailed) != 0u) r.push_back("a starting cell could not be written in the replay");
  return r;
}

namespace {
bool SameDecision(const ll::ReplayDecision& a, const ll::ReplayDecision& b) {
  return a.block == b.block && a.kind == b.kind && a.id == b.id && a.slot == b.slot && a.detail == b.detail;
}
}  // namespace

ReplayComparison CompareRuns(const ReplayRun& a, const ReplayRun& b) {
  ReplayComparison c;
  c.decisionsA = static_cast<std::uint32_t>(a.decisions.size());
  c.decisionsB = static_cast<std::uint32_t>(b.decisions.size());
  const std::size_t n = std::min(a.decisions.size(), b.decisions.size());
  for (std::size_t i = 0; i < n && c.firstDecisionDiff < 0; ++i) {
    if (!SameDecision(a.decisions[i], b.decisions[i])) c.firstDecisionDiff = static_cast<std::int64_t>(i);
  }
  if (c.firstDecisionDiff < 0 && a.decisions.size() != b.decisions.size()) c.firstDecisionDiff = static_cast<std::int64_t>(n);
  c.decisionsMatch = a.ok && b.ok && c.firstDecisionDiff < 0 && a.info.decisionsLost == 0u && b.info.decisionsLost == 0u;
  c.bitExactExpected = a.bitExactExpected && b.bitExactExpected;
  c.digestsMatch = a.digest == b.digest && a.samples == b.samples;
  c.bitsMatch = c.bitExactExpected && c.digestsMatch;
  if (!a.ok || !b.ok) c.reasons.push_back("a replay failed: " + (a.ok ? b.error : a.error));
  if (a.info.decisionsLost != 0u || b.info.decisionsLost != 0u) c.reasons.push_back("the decision log overflowed");
  if (c.firstDecisionDiff >= 0) {
    c.reasons.push_back("the decisions differ from entry " + std::to_string(c.firstDecisionDiff));
  }
  if (!c.bitExactExpected) {
    for (const std::string& s : ConditionReasons(a.info.conditions | b.info.conditions)) {
      c.reasons.push_back("no bit identity expected: " + s);
    }
  } else if (!c.digestsMatch) {
    c.reasons.push_back("the output bits differ although 2.5's conditions hold");
  }
  if (!a.blockNumbersKept || !b.blockNumbersKept) {
    c.reasons.push_back("the replay could not keep the recorded block numbers (bank loading took longer)");
  }
  c.ok = c.decisionsMatch && (c.bitsMatch || !c.bitExactExpected);
  return c;
}

std::string ReplayComparison::Text() const {
  std::ostringstream o;
  o << "decisions: " << (decisionsMatch ? "match" : "DIFFER") << " (" << decisionsA << " / " << decisionsB << ")\n";
  o << "bit identity: " << (bitExactExpected ? (bitsMatch ? "expected and holds" : "expected but DIFFERS") : "not expected")
    << "\n";
  for (const std::string& r : reasons) o << "  - " << r << "\n";
  o << "verdict: " << (ok ? "OK" : "MISMATCH") << "\n";
  return o.str();
}

RecordedRunComparison CompareWithRecordedRun(const ReplayRun& replay, const RecordingDescription& rec,
                                             const RecordedRunLog& run) {
  RecordedRunComparison c;
  // Log: the kinds both sides can show (commands taken, playback ends, events).
  if (!run.log.empty()) {
    c.compared = true;
    std::vector<std::pair<std::uint32_t, std::uint32_t>> a, b;
    std::vector<std::uint32_t> kinds;
    for (const ll::LogEntry& e : run.log) {
      a.emplace_back(e.kind, e.id);
      if (std::find(kinds.begin(), kinds.end(), e.kind) == kinds.end()) kinds.push_back(e.kind);
    }
    for (const ll::ReplayDecision& d : replay.decisions) {
      if (std::find(kinds.begin(), kinds.end(), d.kind) != kinds.end()) b.emplace_back(d.kind, d.id);
    }
    c.logEntries = static_cast<std::uint32_t>(a.size());
    c.replayEntries = static_cast<std::uint32_t>(b.size());
    const std::size_t n = std::min(a.size(), b.size());
    for (std::size_t i = 0; i < n && c.firstDiff < 0; ++i) {
      if (a[i] != b[i]) c.firstDiff = static_cast<std::int64_t>(i);
    }
    if (c.firstDiff < 0 && a.size() != b.size()) c.firstDiff = static_cast<std::int64_t>(n);
    c.logMatches = c.firstDiff < 0;
    if (!c.logMatches) c.notes.push_back("the replay's decisions differ from the recorded event log at entry " + std::to_string(c.firstDiff));
  } else {
    c.notes.push_back("no event log of the recorded run");
  }
  if (!run.lods.empty()) {
    c.compared = true;
    std::vector<RecordedLod> changes;
    for (std::size_t i = 1; i < rec.lods.size(); ++i) changes.push_back(rec.lods[i]);
    c.lodsMatch = changes.size() == run.lods.size();
    for (std::size_t i = 0; c.lodsMatch && i < changes.size(); ++i) {
      c.lodsMatch = changes[i].block == run.lods[i].block && changes[i].level == run.lods[i].level;
    }
    if (!c.lodsMatch) c.notes.push_back("the recording's LOD changes differ from the profiler's");
  }
  if (!run.stalls.empty() || !rec.stalls.empty()) {
    c.compared = true;
    c.stallsMatch = run.stalls.empty() || run.stalls.size() == rec.stalls.size();
    if (!rec.stalls.empty()) c.notes.push_back("stalls are recorded but not reproduced by the replay (2.5)");
  }
  if ((replay.info.conditions & ll::kReplayBreaksRecordedRun) != 0u) {
    for (const std::string& s : ConditionReasons(replay.info.conditions & ll::kReplayBreaksRecordedRun)) {
      c.notes.push_back("the replay may not reproduce the recorded run: " + s);
    }
  }
  return c;
}

std::string RecordedRunComparison::Text() const {
  std::ostringstream o;
  if (!compared) {
    o << "recorded run: nothing to compare\n";
  } else {
    o << "recorded run: event log " << (logMatches ? "matches" : "DIFFERS") << " (" << logEntries << " / " << replayEntries
      << "), LOD " << (lodsMatch ? "consistent" : "DIFFERS") << ", stalls " << (stallsMatch ? "consistent" : "DIFFER") << "\n";
  }
  for (const std::string& n : notes) o << "  - " << n << "\n";
  return o.str();
}

// --- expectations ------------------------------------------------------------------------------
ReplayExpectation ExpectationOf(const ReplayRun& run, bool withDigest) {
  ReplayExpectation e;
  e.decisions = run.decisions;
  e.hasDigest = withDigest;
  e.digest = run.digest;
  return e;
}

std::string EncodeExpectation(const ReplayExpectation& e) {
  std::ostringstream o;
  o << "sonus-replay-expectation 1\n";
  if (e.hasDigest) o << "digest " << Hex(e.digest) << "\n";
  o << "decisions " << e.decisions.size() << "\n";
  for (const ll::ReplayDecision& d : e.decisions) {
    o << d.block << " " << d.kind << " " << d.id << " " << d.slot << " " << d.detail << "\n";
  }
  return o.str();
}

bool DecodeExpectation(const std::string& text, ReplayExpectation* out, std::string* error) {
  auto fail = [&](const std::string& m) {
    if (error != nullptr) *error = m;
    return false;
  };
  std::istringstream in(text);
  std::string line;
  if (!std::getline(in, line) || Trim(line) != "sonus-replay-expectation 1") return fail("not an expectation file");
  ReplayExpectation e;
  std::size_t count = 0;
  bool haveCount = false;
  while (std::getline(in, line)) {
    line = Trim(line);
    if (line.empty()) continue;
    if (line.rfind("digest ", 0) == 0) {
      std::uint64_t v = 0;
      if (!ParseU64("0x" + line.substr(7), &v)) return fail("bad digest");
      e.hasDigest = true;
      e.digest = v;
    } else if (line.rfind("decisions ", 0) == 0) {
      std::uint64_t v = 0;
      if (!ParseU64(line.substr(10), &v) || v > (1u << 24)) return fail("bad decision count");
      count = static_cast<std::size_t>(v);
      haveCount = true;
    } else {
      if (!haveCount) return fail("decision before the count");
      std::istringstream ls(line);
      unsigned long long b = 0, k = 0, i = 0, s = 0, d = 0;
      if (!(ls >> b >> k >> i >> s >> d)) return fail("bad decision line: " + line);
      if (k > 0xFFFFFFFFull || i > 0xFFFFFFFFull || s > 0xFFFFFFFFull || d > 0xFFFFFFFFull) return fail("bad decision value");
      e.decisions.push_back(ll::ReplayDecision{b, static_cast<std::uint32_t>(k), static_cast<std::uint32_t>(i),
                                               static_cast<std::uint32_t>(s), static_cast<std::uint32_t>(d)});
    }
  }
  if (!haveCount || e.decisions.size() != count) return fail("decision count does not match");
  *out = std::move(e);
  return true;
}

bool SaveExpectation(const std::string& path, const ReplayExpectation& e, std::string* error) {
  const std::string t = EncodeExpectation(e);
  if (!WriteBytes(path, t.data(), t.size())) {
    if (error != nullptr) *error = "cannot write " + path;
    return false;
  }
  return true;
}

bool LoadExpectation(const std::string& path, ReplayExpectation* out, std::string* error) {
  std::string t;
  if (!ReadText(path, &t)) {
    if (error != nullptr) *error = "cannot read " + path;
    return false;
  }
  return DecodeExpectation(t, out, error);
}

ExpectationResult CheckExpectation(const ReplayRun& run, const ReplayExpectation& e) {
  ExpectationResult r;
  if (!run.ok) {
    r.messages.push_back("the replay failed: " + run.error);
    return r;
  }
  const std::size_t n = std::min(run.decisions.size(), e.decisions.size());
  for (std::size_t i = 0; i < n; ++i) {
    if (SameDecision(run.decisions[i], e.decisions[i])) continue;
    const ll::ReplayDecision& a = run.decisions[i];
    const ll::ReplayDecision& b = e.decisions[i];
    char m[256];
    std::snprintf(m, sizeof m,
                  "decision %zu: block %" PRIu64 " %s id %08x slot %u detail %08x, expected block %" PRIu64
                  " %s id %08x slot %u detail %08x",
                  i, a.block, LogKindName(a.kind).c_str(), a.id, a.slot, a.detail, b.block, LogKindName(b.kind).c_str(),
                  b.id, b.slot, b.detail);
    r.messages.push_back(m);
    break;
  }
  if (run.decisions.size() != e.decisions.size()) {
    r.messages.push_back("decisions: " + std::to_string(run.decisions.size()) + ", expected " +
                         std::to_string(e.decisions.size()));
  }
  if (e.hasDigest) {
    if (!run.bitExactExpected) {
      r.messages.push_back("an output digest is expected but the replay is outside 2.5's conditions");
      for (const std::string& s : ConditionReasons(run.info.conditions)) r.messages.push_back("  " + s);
    } else if (run.digest != e.digest) {
      r.messages.push_back("output digest " + Hex(run.digest) + ", expected " + Hex(e.digest));
    }
  }
  r.ok = r.messages.empty();
  return r;
}

// --- bug reports ----------------------------------------------------------------------------
bool WriteBugReport(const std::string& dir, const BugReport& rep, std::string* error) {
  auto fail = [&](const std::string& m) {
    if (error != nullptr) *error = m;
    return false;
  };
  std::error_code ec;
  fs::create_directories(fs::path(dir) / "banks", ec);
  if (ec) return fail("cannot create " + dir + ": " + ec.message());
  std::string err;
  if (!SaveRecording((fs::path(dir) / "recording.snrec").string(), rep.recording, &err)) return fail(err);
  std::ostringstream o;
  o << "format = sonus-bug-report 1\n";
  o << "title = " << rep.title.substr(0, rep.title.find('\n')) << "\n";
  o << "recording = recording.snrec\n";
  o << "recording.flags = " << rep.recording.info.flags << "\n";
  o << "recording.projectBuildHash = 0x" << Hex(rep.recording.info.projectBuildHash) << "\n";
  o << "recording.runtimeBuildHash = 0x" << Hex(rep.recording.info.runtimeBuildHash) << "\n";
  o << "recording.sampleRate = " << rep.recording.info.sampleRate << "\n";
  o << "recording.blockSize = " << rep.recording.info.blockSize << "\n";
  o << "recording.platform = " << rep.recording.info.platform << "\n";
  o << "recording.contractVersion = " << rep.recording.info.contractVersion << "\n";
  o << "protocol = " << rep.protocolMajor << "." << rep.protocolMinor << "\n";
  o << "config.known = " << (rep.configKnown ? 1 : 0) << "\n";
  std::istringstream cfg(ConfigText(rep.config));
  std::string line;
  while (std::getline(cfg, line)) o << "config." << line << "\n";
  o << "banks = " << rep.banks.size() << "\n";
  for (std::size_t i = 0; i < rep.banks.size(); ++i) {
    const ReportBank& b = rep.banks[i];
    if (b.file.empty() || b.file.find('/') != std::string::npos || b.file.find('\\') != std::string::npos ||
        b.file.find("..") != std::string::npos) {
      return fail("bad bank file name: " + b.file);
    }
    o << "bank." << i << ".file = " << b.file << "\n";
    o << "bank." << i << ".bankId = 0x" << Hex(b.bankId).substr(8) << "\n";
    o << "bank." << i << ".languageId = 0x" << Hex(b.languageId).substr(8) << "\n";
    o << "bank." << i << ".contentHash = 0x" << Hex(b.contentHash) << "\n";
    if (!WriteBytes((fs::path(dir) / "banks" / b.file).string(), b.bytes.data(), b.bytes.size())) {
      return fail("cannot write bank " + b.file);
    }
  }
  o << "replayed = " << (rep.replayed ? 1 : 0) << "\n";
  o << "decisionsMatch = " << (rep.decisionsMatch ? 1 : 0) << "\n";
  o << "bitExactExpected = " << (rep.bitExactExpected ? 1 : 0) << "\n";
  o << "bitsMatch = " << (rep.bitsMatch ? 1 : 0) << "\n";
  const std::string report = o.str();
  if (!WriteBytes((fs::path(dir) / "report.txt").string(), report.data(), report.size())) return fail("cannot write report.txt");
  std::string summary = "# " + rep.title + "\n\n" + rep.notes + (rep.notes.empty() ? "" : "\n\n") + "## recording\n" +
                        rep.summary + "\n## replay\n" + (rep.comparison.empty() ? "not replayed\n" : rep.comparison);
  if (!WriteBytes((fs::path(dir) / "summary.txt").string(), summary.data(), summary.size())) {
    return fail("cannot write summary.txt");
  }
  if (rep.replayed) {
    if (!SaveExpectation((fs::path(dir) / "expected.txt").string(), rep.expectation, &err)) return fail(err);
  }
  return true;
}

bool ReadBugReport(const std::string& dir, BugReport* out, std::string* error) {
  auto fail = [&](const std::string& m) {
    if (error != nullptr) *error = m;
    return false;
  };
  std::string text;
  if (!ReadText((fs::path(dir) / "report.txt").string(), &text)) return fail("no report.txt in " + dir);
  BugReport r;
  snd_config_init(&r.config);
  std::istringstream in(text);
  std::string line;
  bool format = false;
  std::size_t banks = 0;
  std::vector<std::string> files;
  while (std::getline(in, line)) {
    const std::size_t eq = line.find('=');
    if (eq == std::string::npos) continue;
    const std::string key = Trim(line.substr(0, eq)), value = Trim(line.substr(eq + 1));
    std::uint64_t u = 0;
    if (key == "format") {
      format = value == "sonus-bug-report 1";
    } else if (key == "title") {
      r.title = value;
    } else if (key == "protocol") {
      const std::size_t dot = value.find('.');
      std::uint64_t ma = 0, mi = 0;
      if (dot != std::string::npos && ParseU64(value.substr(0, dot), &ma) && ParseU64(value.substr(dot + 1), &mi)) {
        r.protocolMajor = static_cast<std::uint16_t>(ma);
        r.protocolMinor = static_cast<std::uint16_t>(mi);
      }
    } else if (key == "config.known") {
      r.configKnown = value == "1";
    } else if (key.rfind("config.", 0) == 0) {
      if (!ParseConfigLine(key.substr(7), value, &r.config)) return fail("bad config line: " + line);
    } else if (key == "banks") {
      if (!ParseU64(value, &u) || u > 4096u) return fail("bad bank count");
      banks = static_cast<std::size_t>(u);
      r.banks.resize(banks);
    } else if (key.rfind("bank.", 0) == 0) {
      const std::size_t dot = key.find('.', 5);
      std::uint64_t i = 0;
      if (dot == std::string::npos || !ParseU64(key.substr(5, dot - 5), &i) || i >= banks) return fail("bad bank line: " + line);
      ReportBank& b = r.banks[static_cast<std::size_t>(i)];
      const std::string field = key.substr(dot + 1);
      if (field == "file") {
        if (value.empty() || value.find('/') != std::string::npos || value.find('\\') != std::string::npos ||
            value.find("..") != std::string::npos) {
          return fail("bad bank file name: " + value);
        }
        b.file = value;
      } else if (field == "bankId" && ParseU64(value, &u)) {
        b.bankId = static_cast<Id>(u);
      } else if (field == "languageId" && ParseU64(value, &u)) {
        b.languageId = static_cast<Id>(u);
      } else if (field == "contentHash" && ParseU64(value, &u)) {
        b.contentHash = u;
      } else {
        return fail("bad bank line: " + line);
      }
    } else if (key == "replayed") {
      r.replayed = value == "1";
    } else if (key == "decisionsMatch") {
      r.decisionsMatch = value == "1";
    } else if (key == "bitExactExpected") {
      r.bitExactExpected = value == "1";
    } else if (key == "bitsMatch") {
      r.bitsMatch = value == "1";
    }
  }
  if (!format) return fail("not a sonus bug report");
  std::string err;
  if (!LoadRecording((fs::path(dir) / "recording.snrec").string(), &r.recording, &err)) return fail(err);
  for (ReportBank& b : r.banks) {
    if (b.file.empty()) return fail("a bank without a file");
    std::string bytes;
    if (!ReadText((fs::path(dir) / "banks" / b.file).string(), &bytes)) return fail("missing bank " + b.file);
    b.bytes.assign(bytes.begin(), bytes.end());
  }
  std::string summary;
  if (ReadText((fs::path(dir) / "summary.txt").string(), &summary)) r.summary = summary;
  r.notes.clear();
  if (r.replayed) {
    if (!LoadExpectation((fs::path(dir) / "expected.txt").string(), &r.expectation, &err)) return fail(err);
  }
  *out = std::move(r);
  return true;
}

}  // namespace snd::tool
