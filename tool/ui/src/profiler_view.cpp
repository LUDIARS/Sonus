// Profiler screen (design.md 11.1): block time graph with the budget line and
// a summary, the voice list, the event log, memory per tag and the recording
// controls (11.3).
#include <algorithm>
#include <cstdio>

#include "snd/tool/names.hpp"
#include "snd/tool/profiler_streams.hpp"
#include "snd/tool/ui/app.hpp"
#include "snd/tool/ui/editor_draw.hpp"
#include "snd/tool/ui/kit.hpp"
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
  g.dpi = ctx.dpi;
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
      Canvas(doc, id + ".bar", DrawUsageBar(colW * 0.34f, 8, frac, ctx.palette, ctx.dpi), cx + colW * 0.64f, cy + 7, colW * 0.34f, 8);
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

void DeclareOverview(tela::Document& doc, ViewContext& ctx, float width, float height) {
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

// --- the rest of the profiler (ProfilerStreams, group P's model) ---------------------------------
namespace {

constexpr std::uint32_t kTabViews[] = {0u, kViewCpu | kViewCpuDetail, kViewMemory, kViewStreams, kViewLod, kViewEventLog};

void Unavailable(Kit& kit, const ProfilerStreams& ps, std::uint32_t view, float y, float w) {
  std::string why;
  if (!ps.Available(view, &why)) kit.Label("prof.x.unavailable", why, 0, y, w, 20, 0.55f);
}

std::string Counts(const char* what, const StreamCount& c) {
  return std::string(what) + " " + std::to_string(c.received) + " received, " + std::to_string(c.missed) + " missed";
}

void CpuTab(Kit& kit, const ProfilerStreams& ps, float w, float h) {
  SeriesLook sl;
  sl.dpi = kit.ctx().dpi;
  sl.width = w;
  sl.height = std::min(200.0f, h * 0.4f);
  float mx = 1.0f;
  for (const CpuPoint& p : ps.cpu()) {
    sl.values.push_back(p.load);
    mx = std::max(mx, p.load);
  }
  sl.yMax = mx * 1.1f;
  sl.reference = 1.0f;
  Canvas(kit.doc(), "prof.cpu.graph", DrawSeries(sl, kit.palette()), 0, 0, w, sl.height);
  kit.Label("prof.cpu.graph.title", "load = (logic + render) / budget; dashed: the budget", 4, 2, w - 8, 18, 0.5f);
  char b[200];
  std::snprintf(b, sizeof b, "last %.0f %%  max %.0f %%  over budget %u of %u", ps.lastLoad() * 100.0, ps.maxLoad() * 100.0,
                ps.overBudget(), static_cast<unsigned>(ps.cpu().size()));
  float y = sl.height + 4.0f;
  kit.Label("prof.cpu.sum", b, 0, y, w, 20, 0.55f);
  y += 20.0f;
  kit.Label("prof.cpu.counts", Counts("timings", ps.cpuCount()) + ";  " + Counts("details", ps.detailCount()), 0, y, w, 20, 0.5f);
  y += 22.0f;
  Unavailable(kit, ps, kViewCpuDetail, y, w);
  kit.Label("prof.cpu.head", "block      total   logic  render  budget  voices chunks late held par LOD", 0, y + 20, w, 18, 0.5f);
  y += 40.0f;
  const auto& det = ps.cpuDetail();
  const std::size_t rows = static_cast<std::size_t>(std::max(0.0f, (h - y) / 18.0f));
  for (std::size_t r = 0; r < rows && r < det.size(); ++r) {
    const livelink::BlockDetail& dd = det[det.size() - 1 - r].detail;
    std::snprintf(b, sizeof b, "%-10llu %6.1f  %6.1f  %6.1f  %6.1f  %5u  %5u %4u %4u %3u %3u",
                  static_cast<unsigned long long>(dd.blockIndex), dd.totalMicroseconds, dd.logicMicroseconds,
                  dd.renderMicroseconds, dd.budgetMicroseconds, dd.renderedVoices, dd.chunks, dd.lateChunks, dd.heldVoices,
                  dd.parallel, dd.lodLevel);
    kit.Label("prof.cpu.row." + std::to_string(r), b, 0, y, w, 18, 0.5f);
    y += 18.0f;
  }
}

void MemoryTab(Kit& kit, const ProfilerStreams& ps, float w, float h) {
  const std::vector<MemoryRow> rows = ps.Memory();
  kit.Label("prof.mem.total", std::string("total ") + FormatBytes(ps.totalUsed()) + (ps.hasMemory() ? "" : " (no report yet)"), 0, 0,
            w, 20, 0.6f);
  float y = 24.0f;
  for (std::size_t i = 0; i < rows.size() && y < h - 20.0f; ++i) {
    const MemoryRow& r = rows[i];
    std::string label = std::string(r.name) + "  " + FormatBytes(r.used);
    if (r.budget != 0u) label += " / " + FormatBytes(r.budget);
    kit.Label("prof.mem.row." + std::to_string(r.tag), label, 0, y, w * 0.45f, 22, 0.55f);
    Canvas(kit.doc(), "prof.mem.row." + std::to_string(r.tag) + ".bar",
           DrawUsageBar(w * 0.5f, 10, r.budget != 0u ? r.fraction : 0.0f, kit.palette(), kit.ctx().dpi), w * 0.48f, y + 6, w * 0.5f, 10);
    y += 24.0f;
  }
}

void StreamsTab(Kit& kit, const ProfilerStreams& ps, ProfilerViewState* st, float w, float h) {
  Unavailable(kit, ps, kViewStreams, 0, w);
  kit.Label("prof.st.sum", "streams " + std::to_string(ps.streams().size()) + " slots, " + std::to_string(ps.activeStreams()) +
                               " active;  " + Counts("stalls", ps.stallCount()),
            0, 20, w, 20, 0.55f);
  kit.Label("prof.st.head", "slot  event     state  stalls", 0, 42, w * 0.5f, 18, 0.5f);
  static const char* const kState[] = {"idle", "active"};  // livelink::StreamState
  const auto& streams = ps.streams();
  const std::uint32_t per = static_cast<std::uint32_t>(std::max(1.0f, (h - 90.0f) / 18.0f));
  const std::uint32_t first = static_cast<std::uint32_t>(std::min<std::size_t>(st->page * per, streams.size()));
  Kit& k = kit;
  k.Pager("prof.st.pager", static_cast<std::uint32_t>(streams.size()), per, &st->page, w * 0.5f - 200, 42, 200);
  float y = 62.0f;
  char b[128];
  for (std::uint32_t i = first; i < streams.size() && i < first + per; ++i) {
    const livelink::StreamInfo& s = streams[i];
    std::snprintf(b, sizeof b, "%4u  %s  %-6s %6u", s.slot, Hex32(s.eventId).c_str(), s.state < 2u ? kState[s.state] : "?", s.stalls);
    kit.Label("prof.st.row." + std::to_string(i - first), b, 0, y, w * 0.5f, 18, 0.5f);
    y += 18.0f;
  }
  // Stall history (newest first).
  kit.Label("prof.st.stalls", "stall history (newest first)", w * 0.52f, 42, w * 0.48f, 18, 0.5f);
  const auto& stalls = ps.stalls();
  y = 62.0f;
  for (std::size_t r = 0; r < stalls.size() && y < h - 18.0f; ++r) {
    const livelink::StreamStall& s = stalls[stalls.size() - 1 - r].stall;
    std::snprintf(b, sizeof b, "%10llu  event %s  slot %u", static_cast<unsigned long long>(s.clockSample), Hex32(s.eventId).c_str(),
                  s.streamSlot);
    kit.Label("prof.st.stall." + std::to_string(r), b, w * 0.52f, y, w * 0.48f, 18, 0.5f);
    y += 18.0f;
  }
}

void LodTab(Kit& kit, const ProfilerStreams& ps, float w, float h) {
  kit.Label("prof.lod.level", "load LOD level " + std::to_string(ps.lodLevel()) + ";  " + Counts("changes", ps.lodCount()), 0, 0, w,
            22, 0.65f);
  SeriesLook sl;
  sl.dpi = kit.ctx().dpi;
  sl.width = w;
  sl.height = std::min(140.0f, h * 0.35f);
  sl.steps = true;
  float mx = 1.0f;
  for (const LodPoint& p : ps.lods()) {
    sl.values.push_back(static_cast<float>(p.change.newLevel));
    mx = std::max(mx, static_cast<float>(p.change.newLevel));
  }
  sl.yMax = mx + 0.5f;
  Canvas(kit.doc(), "prof.lod.graph", DrawSeries(sl, kit.palette()), 0, 24, w, sl.height);
  float y = 28.0f + sl.height;
  const auto& lods = ps.lods();
  char b[128];
  for (std::size_t r = 0; r < lods.size() && y < h - 18.0f; ++r) {
    const livelink::LodChange& c = lods[lods.size() - 1 - r].change;
    std::snprintf(b, sizeof b, "block %llu: %u -> %u", static_cast<unsigned long long>(c.blockIndex), c.oldLevel, c.newLevel);
    kit.Label("prof.lod.row." + std::to_string(r), b, 0, y, w, 18, 0.5f);
    y += 18.0f;
  }
}

void LogTab(Kit& kit, const ProfilerStreams& ps, ProfilerViewState* st, float w, float h) {
  kit.Toggle("prof.log.f.cmd", "commands", st->logCommands, [st] { st->logCommands = !st->logCommands; }, 0, 0, 110);
  kit.Toggle("prof.log.f.ev", "events", st->logEvents, [st] { st->logEvents = !st->logEvents; }, 114, 0, 100);
  kit.Toggle("prof.log.f.end", "ends", st->logEnds, [st] { st->logEnds = !st->logEnds; }, 218, 0, 90);
  NumberSpec id;
  id.value = st->logId;
  id.step = 1;
  id.minValue = 0;
  id.maxValue = 4294967295.0;
  id.integer = true;
  id.decimals = 0;
  id.document = false;
  kit.NumberField("prof.log.f.id", "id (0 any)", id, [st](double v, std::uint32_t) {
    st->logId = static_cast<std::uint32_t>(v);
    return true;
  }, 314, 0, 240, 80);
  LogFilter f;
  f.commands = st->logCommands;
  f.events = st->logEvents;
  f.ends = st->logEnds;
  f.id = st->logId;
  const std::vector<LogPoint> rows = ps.Log(f);
  kit.Label("prof.log.counts", Counts("log", ps.logCount()) + ";  shown " + std::to_string(rows.size()) + ";  dropped " +
                                   std::to_string(ps.dropped()),
            0, 26, w, 20, 0.5f);
  const std::uint32_t per = static_cast<std::uint32_t>(std::max(1.0f, (h - 76.0f) / 18.0f));
  const std::uint32_t first = kit.Pager("prof.log.pager", static_cast<std::uint32_t>(rows.size()), per, &st->page, w - 200, 26, 200);
  float y = 50.0f;
  char b[160];
  for (std::uint32_t i = first; i < rows.size() && i < first + per; ++i) {
    const livelink::LogEntry& e = rows[rows.size() - 1 - i].entry;
    std::snprintf(b, sizeof b, "%10llu  %-20s id %s  slot %u  detail %u", static_cast<unsigned long long>(e.clockSample),
                  LogEntryKindName(e.kind).c_str(), Hex32(e.id).c_str(), e.handleSlot, e.detail);
    kit.Label("prof.log.row." + std::to_string(i - first), b, 0, y, w, 18, 0.5f);
    y += 18.0f;
  }
}

}  // namespace

void DeclareProfiler(tela::Document& doc, ViewContext& ctx, float width, float height) {
  App* app = ctx.app;
  if (app == nullptr) {
    DeclareOverview(doc, ctx, width, height);
    return;
  }
  ProfilerViewState* st = &app->view().profiler;
  Kit kit(doc, ctx);
  Session* s = &app->session();
  kit.Tabs("prof.tab", {"Overview", "CPU", "Memory", "Streams", "LOD", "Event log"}, st->tab, [st, s](std::uint32_t i) {
    st->tab = i;
    st->page = 0;
    if (i != 0u) s->EnableProfilerStreams(kTabViews[i]);  // subscribe what the tab shows
  }, 0, 0, 96);
  const float top = 30.0f;
  Panel(doc, "prof.body", 0, top, width, height - top, [&] {
    const float h = height - top;
    const ProfilerStreams& ps = app->session().streams();
    switch (st->tab) {
      case 1: CpuTab(kit, ps, width, h); break;
      case 2: MemoryTab(kit, ps, width, h); break;
      case 3: StreamsTab(kit, ps, st, width, h); break;
      case 4: LodTab(kit, ps, width, h); break;
      case 5: LogTab(kit, ps, st, width, h); break;
      default: DeclareOverview(doc, ctx, width, h); break;
    }
  });
}

}  // namespace snd::tool::ui
