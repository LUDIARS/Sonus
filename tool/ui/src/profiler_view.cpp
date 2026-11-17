// Profiler screen (design.md 11.1): block time graph with the budget line and
// a summary, the voice list, the event log, memory per tag and the recording
// controls (11.3).
#include <algorithm>
#include <cstdio>

#include "snd/tool/names.hpp"
#include "snd/tool/ui/layout_metrics.hpp"
#include "snd/tool/ui/views.hpp"
#include "snd/tool/ui/widgets.hpp"

namespace snd::tool::ui {
namespace {

constexpr float kGraphHeight = 200.0f;
constexpr float kBottomHeight = 150.0f;
constexpr float kRow = 18.0f;

std::string Line(const char* fmt, double a, double b = 0, double c = 0) {
  char buf[128];
  std::snprintf(buf, sizeof buf, fmt, a, b, c);
  return buf;
}

std::string BusName(const Project& p, std::uint32_t id) {
  if (id == 0u) return "master";
  const std::int32_t i = p.FindBus(id);
  return i >= 0 ? p.buses()[static_cast<std::size_t>(i)].name : Hex32(id);
}

void DeclareGraph(tela::Document& doc, ViewContext& ctx, float w) {
  const Profiler& prof = ctx.session.profiler();
  GraphLook g;
  g.width = w;
  g.height = kGraphHeight;
  for (const TimingPoint& t : prof.timings()) {
    g.total.push_back(t.timing.logicMicroseconds + t.timing.renderMicroseconds);
    g.render.push_back(t.timing.renderMicroseconds);
    g.budget = t.timing.budgetMicroseconds;
  }
  Canvas(doc, "prof.graph", DrawTimingGraph(g, ctx.palette), 0, 0, w, kGraphHeight);
  Text(doc, "prof.graph.title", "block time (logic + render, us) / budget", 4, 2, w - 8, 18, 0.55f);
}

void DeclareSummary(tela::Document& doc, ViewContext& ctx, float x, float w) {
  const Profiler& prof = ctx.session.profiler();
  const TimingSummary s = prof.Summary();
  std::vector<std::string> lines;
  lines.push_back(Line("render  last %.1f  avg %.1f  max %.1f us", s.lastRenderUs, s.avgRenderUs, s.maxRenderUs));
  lines.push_back(Line("logic   last %.1f  avg %.1f us", s.lastLogicUs, s.avgLogicUs));
  lines.push_back(Line("budget  %.1f us   load %.0f %%", s.budgetUs, s.loadPercent));
  lines.push_back(Line("over budget %.0f of %.0f blocks", s.overBudget, s.samples));
  lines.push_back(Line("LOD %.0f   late chunks %.0f   underruns %.0f", s.lod, s.lateChunks, s.underruns));
  lines.push_back(Line("voices %.0f real / %.0f listed", prof.realVoices(), static_cast<double>(prof.voices().size())));
  lines.push_back(Line("stream stalls %.0f   LOD changes %.0f", prof.stalls(), prof.lodChanges()));
  lines.push_back(Line("block %.0f", static_cast<double>(s.lastBlock)));
  for (std::size_t i = 0; i < lines.size(); ++i) {
    Text(doc, "prof.sum." + std::to_string(i), lines[i], x, static_cast<float>(i) * 22.0f, w, 22, 0.6f);
  }
}

void DeclareVoices(tela::Document& doc, ViewContext& ctx, float x, float y, float w, float h) {
  const Profiler& prof = ctx.session.profiler();
  const Project& project = ctx.session.project();
  Panel(doc, "prof.voices", x, y, w, h, [&] {
    static const char* const kHead[] = {"event", "state", "bus", "audib.", "gain", "source"};
    const float cols[] = {0.0f, 0.24f, 0.40f, 0.62f, 0.74f, 0.86f};
    for (int c = 0; c < 6; ++c) {
      Text(doc, "prof.voices.h" + std::to_string(c), kHead[c], cols[c] * w, 0, w * 0.2f, kRow, 0.55f);
    }
    const std::size_t rows = static_cast<std::size_t>(std::max(0.0f, (h - 2.0f * kRow - 12.0f) / kRow));
    const auto& voices = prof.voices();
    for (std::size_t r = 0; r < std::min(rows, voices.size()); ++r) {
      const livelink::VoiceInfo& v = voices[r];
      const std::string id = "prof.voice." + std::to_string(r);
      const float yy = kRow * static_cast<float>(r + 1);
      char aud[24], gain[24];
      std::snprintf(aud, sizeof aud, "%.3f", static_cast<double>(v.audibility));
      std::snprintf(gain, sizeof gain, "%.3f", static_cast<double>(v.gain));
      const std::string cells[] = {Hex32(v.eventId) + (v.isVirtual != 0u ? " (v)" : ""), PlaybackStateName(v.playbackState),
                                   BusName(project, v.busId), aud, gain, SourceKindName(v.sourceKind)};
      for (int c = 0; c < 6; ++c) {
        Text(doc, id + "." + std::to_string(c), cells[c], cols[c] * w, yy, w * 0.2f, kRow, 0.5f);
      }
    }
    if (voices.size() > rows) {
      Text(doc, "prof.voices.more", "+" + std::to_string(voices.size() - rows) + " voices",
           0, kRow * static_cast<float>(rows + 1), w, kRow, 0.5f);
    }
  }, 6.0f);
}

void DeclareLog(tela::Document& doc, ViewContext& ctx, float x, float y, float w, float h) {
  const Profiler& prof = ctx.session.profiler();
  Panel(doc, "prof.log", x, y, w, h, [&] {
    Text(doc, "prof.log.title", "event log (newest first)", 0, 0, w, kRow, 0.55f);
    const std::size_t rows = static_cast<std::size_t>(std::max(0.0f, (h - kRow - 12.0f) / kRow));
    const auto& log = prof.log();
    for (std::size_t r = 0; r < std::min(rows, log.size()); ++r) {
      const livelink::LogEntry& e = log[log.size() - 1 - r].entry;
      char b[160];
      std::snprintf(b, sizeof b, "%10llu  %-18s id %s  slot %u  detail %u", static_cast<unsigned long long>(e.clockSample),
                    LogKindName(e.kind).c_str(), Hex32(e.id).c_str(), e.handleSlot, e.detail);
      Text(doc, "prof.log." + std::to_string(r), b, 0, kRow * static_cast<float>(r + 1), w, kRow, 0.5f);
    }
  }, 6.0f);
}

void DeclareBottom(tela::Document& doc, ViewContext& ctx, float y, float w) {
  const Profiler& prof = ctx.session.profiler();
  Actions* a = &ctx.actions;
  const float memW = w * 0.6f;
  Panel(doc, "prof.memory", 0, y, memW, kBottomHeight, [&] {
    const float colW = (memW - 12.0f) / 2.0f;
    for (std::uint32_t t = 0; t < SND_MEM_TAG_COUNT; ++t) {
      const float cx = static_cast<float>(t / 6u) * colW;
      const float cy = static_cast<float>(t % 6u) * 22.0f;
      const std::string id = "prof.mem." + std::to_string(t);
      const std::uint64_t used = prof.hasMemory() ? prof.memory().used[t] : 0u;
      const std::uint64_t budget = prof.hasMemory() ? prof.memory().budget[t] : 0u;
      std::string label = std::string(MemTagName(t)) + " " + FormatBytes(used);
      if (budget != 0u) label += " / " + FormatBytes(budget);
      Text(doc, id, label, cx, cy, colW * 0.62f, 20, 0.5f);
      const float frac = budget != 0u ? static_cast<float>(static_cast<double>(used) / static_cast<double>(budget)) : 0.0f;
      Canvas(doc, id + ".bar", DrawUsageBar(colW * 0.34f, 8, frac, ctx.palette), cx + colW * 0.64f, cy + 7, colW * 0.34f, 8);
    }
  }, 6.0f);
  const float rx = memW + 8.0f, rw = w - memW - 8.0f;
  Panel(doc, "prof.rec", rx, y, rw, kBottomHeight, [&] {
    const RecordingState rs = prof.recordingState();
    const bool recording = rs == RecordingState::Recording || rs == RecordingState::Starting;
    Button(doc, "rec.toggle", recording ? "Stop recording" : "Record", [a] { a->OnRecordToggle(); }, 0, 0, 130, 26);
    Button(doc, "rec.save", "Save", [a] { a->OnSaveRecording(); }, 136, 0, 70, 26);
    Button(doc, "rec.load", "Load", [a] { a->OnLoadRecording(); }, 212, 0, 70, 26);
    Button(doc, "rec.clips", "Reset clips", [a] { a->OnResetClips(); }, 288, 0, 100, 26);
    Text(doc, "rec.state", std::string("recording ") + RecordingStateName(rs) + "  " + FormatBytes(prof.recordedBytes()),
         0, 32, rw - 12, 20, 0.55f);
    Text(doc, "rec.path", ctx.recordingPath, 0, 54, rw - 12, 20, 0.5f);
    const RecordingSummary& s = prof.loadedSummary();
    std::string loaded = "loaded: none";
    if (!prof.loadedPath().empty()) {
      char b[160];
      std::snprintf(b, sizeof b, "loaded: %u arenas, %u LOD, %u stalls, %u banks, blocks %llu..%llu%s", s.arenas, s.lods,
                    s.stalls, s.bankMaps, static_cast<unsigned long long>(s.firstBlock),
                    static_cast<unsigned long long>(s.lastBlock), s.valid ? "" : " (damaged)");
      loaded = b;
    }
    Text(doc, "rec.loaded", loaded, 0, 76, rw - 12, 20, 0.5f);
  }, 6.0f);
}

}  // namespace

void DeclareProfiler(tela::Document& doc, ViewContext& ctx, float width, float height) {
  const float graphW = std::max(200.0f, width - 330.0f);
  DeclareGraph(doc, ctx, graphW);
  DeclareSummary(doc, ctx, graphW + 10.0f, width - graphW - 16.0f);
  const float midY = kGraphHeight + 8.0f;
  const float midH = std::max(60.0f, height - midY - kBottomHeight - 8.0f);
  const float voicesW = width * 0.55f;
  DeclareVoices(doc, ctx, 0, midY, voicesW, midH);
  DeclareLog(doc, ctx, voicesW + 8.0f, midY, width - voicesW - 8.0f, midH);
  DeclareBottom(doc, ctx, midY + midH + 8.0f, width);
}

}  // namespace snd::tool::ui
