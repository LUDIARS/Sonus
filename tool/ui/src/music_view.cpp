// Music editor screen (design.md 8, 11.1 音楽エディタ): segments (material, bars, entry / exit,
// the tempo / time signature / marker time line with draggable markers, tempo points, markers,
// stems, tap tempo and the grid), playlists, the music event's states and transition rule table
// (edit, reorder, auto sort, unreachable rows), the transition preview (which row, the firing beat,
// fades, the bridge on a time line; the audition through the AuditionRouter) with the minimum
// lookahead, and the stingers. Edits through SegmentEditor / PlaylistEditor / MusicEditor.
#include <algorithm>
#include <cmath>
#include <limits>
#include <cstdio>

#include "editor_util.hpp"
#include "snd/tool/music_editor.hpp"
#include "snd/tool/preview.hpp"
#include "snd/tool/ui/app.hpp"
#include "snd/tool/ui/editor_draw.hpp"
#include "snd/tool/ui/kit.hpp"
#include "snd/tool/ui/layout_metrics.hpp"
#include "snd/tool/ui/views.hpp"

namespace snd::tool::ui {
namespace {

constexpr float kListW = 220.0f;
constexpr float kRow = 26.0f;
const char* const kTiming[] = {"immediate", "nextBeat", "nextBar", "barsAfter", "grid", "nextMarker", "segmentEnd"};
const char* const kStart[] = {"head", "sameBar", "marker", "lastPosition"};
const char* const kLate[] = {"nextGrid", "startLate", "cancel"};
const char* const kMarkerKind[] = {"user", "entry", "exit", "pickup"};
const char* const kMode[] = {"sequential", "random", "shuffle"};

std::vector<PickerItem> Names(const char* const* names, int n) {
  std::vector<PickerItem> v;
  for (int i = 0; i < n; ++i) v.push_back(PickerItem{names[i], std::to_string(i)});
  return v;
}
int ToInt(const std::string& s) { return s.empty() ? 0 : std::atoi(s.c_str()); }

NumberSpec Num(double v, double step, double mn, double mx, int decimals, const char* unit = "", bool integer = false) {
  NumberSpec s;
  s.value = v;
  s.step = step;
  s.minValue = mn;
  s.maxValue = mx;
  s.decimals = decimals;
  s.unit = unit;
  s.integer = integer;
  return s;
}

// --- segments --------------------------------------------------------------------------------
void SegmentLine(Kit& kit, const std::string& g, float x, float y, float w, float h) {
  App* app = &kit.app();
  ProjectDocument* d = app->document();
  MusicView* mv = &app->view().music;
  SegmentEditor se(*d, g);
  const TempoMap map = se.Map();
  const double rate = se.SampleRate();
  const double exitTick = std::max(1.0, se.ExitTick());
  const double entry = NumberAt(d->Find(g)->doc, "entryFrame", 0.0);
  const double seconds = std::max(0.1, map.SecondsAtTick(exitTick));
  const double pxPerSec = w / (seconds * 1.05);
  MarkLook look;
  look.dpi = kit.ctx().dpi;
  look.width = w;
  look.height = h;
  look.spans.push_back(Span{0.0f, static_cast<float>(seconds * pxPerSec), SpanKind::Segment, h * 0.7f, h * 0.3f});
  double t = 0.0;
  for (int n = 0; n < 400 && t <= exitTick; ++n) {
    const double pos = map.BarPosition(t);
    const bool bar = std::fabs(pos - std::round(pos)) < 1e-6;
    look.marks.push_back(Mark{static_cast<float>(map.SecondsAtTick(t) * pxPerSec), bar ? MarkKind::Bar : MarkKind::Beat});
    const double next = map.NextBeat(t + 1e-6);
    if (!(next > t)) break;
    t = next;
  }
  for (const TempoPointDesc& p : map.points()) {
    look.marks.push_back(Mark{static_cast<float>(map.SecondsAtTick(static_cast<double>(p.tick)) * pxPerSec), MarkKind::Tempo});
  }
  const std::vector<MarkerDesc> markers = se.Markers();
  std::vector<std::string> markerNames;
  for (const MarkerDesc& m : markers) markerNames.push_back(m.name);
  const std::vector<std::string> mkeys = NameKeys::Of(markerNames);
  // Marker hits: drag horizontally to move the marker's frame (one undo step).
  for (std::size_t i = 0; i < markers.size() && i < 64u; ++i) {
    const MarkerDesc m = markers[i];
    const float mx = rate > 0.0 ? static_cast<float>((static_cast<double>(m.frame) - entry) / rate * pxPerSec) : 0.0f;
    look.marks.push_back(Mark{mx, MarkKind::Marker, mv->marker == m.name});
    const std::uint32_t mi = static_cast<std::uint32_t>(i);
    const std::string mname = m.name;
    auto hd = DocumentDrag(
        *app, [mv, mname](float, float, std::uint32_t) { mv->marker = mname; },
        [d, g, mi, m, rate, pxPerSec](const DragMotion& mo) {
          MarkerDesc n = m;
          const double f = static_cast<double>(m.frame) + static_cast<double>(mo.dx) / pxPerSec * rate;
          n.frame = SatU64(std::round(f));
          SegmentEditor(*d, g).SetMarker(mi, n);
        });
    kit.Hit("mu.seg.mk." + mkeys[i], hd, x + mx - 5.0f, y, 10.0f, h);
  }
  Canvas(kit.doc(), "mu.seg.line", DrawMarks(look, kit.palette()), x, y, w, h);
  char b[160];
  SnprintfFinite(b, sizeof b, "%.3f s  %s  rate %s  frames %s  exit tick %.0f", seconds, "(bars: lines, beats: short)",
                rate > 0 ? FormatNumberText(rate, 0).c_str() : "?", FormatNumberText(se.Frames(), 0).c_str(), exitTick);
  kit.Label("mu.seg.line.info", b, x, y + h, w, 18, 0.45f);
}

void SegmentsTab(Kit& kit, float width, float height) {
  App* app = &kit.app();
  ProjectDocument* d = app->document();
  MusicView& mv = app->view().music;
  MusicView* mvp = &mv;
  ObjectListSpec ls;
  ls.id = "mu.seg.list";
  ls.kind = "segment";
  ls.title = "Segments";
  ls.selected = &mv.segment;
  ls.page = &mv.listPage;
  ls.create = [d] {
    json::Value init = json::MakeObject();
    const std::string bank = FirstBank(*d);
    if (!bank.empty()) json::Add(init, "bank", String(bank));
    json::Add(init, "bars", Number(1.0));
    const std::string g = d->Create("segment", NewName(*d, "segment", "Segment"), &init);
    const auto assets = d->OfKind("asset");
    if (!g.empty() && !assets.empty()) SegmentEditor(*d, g).SetAsset(assets.front()->guid);
    return g;
  };
  Panel(kit.doc(), "mu.seglist", 0, 0, kListW, height, [&] { ObjectList(kit, ls, 0, 0, kListW - 12, height - 12); }, 6.0f);
  if (mv.segment.empty()) return;
  const std::string g = mv.segment;
  SegmentEditor se(*d, g);
  if (!se.valid()) return;
  const json::Value& j = d->Find(g)->doc;
  const float mx = kListW + 8.0f, mw = width - mx;
  Panel(kit.doc(), "mu.segmain", mx, 0, mw, height, [&] {
    const float w = mw - 12.0f;
    const float cw = (w - 24.0f) / 4.0f;
    float y = 0.0f;
    kit.PickField("mu.seg.asset", "Asset", RefName(*d, "asset", StringAt(j, "asset")), "Material",
                  [d] { return DocItems(*d, "asset"); }, [d, g](const std::string& v) { SegmentEditor(*d, g).SetAsset(v); },
                  0, y, cw, 46);
    kit.NumberField("mu.seg.bars", "Bars", Num(NumberAt(j, "bars", 1.0), 1, 1, 1e6, 0, "", true),
                    [d, g](double v, std::uint32_t) {
                      std::uint32_t n = 0;
                      return WholeU32(v, &n) && SegmentEditor(*d, g).SetBars(n);
                    },
                    cw + 8, y, cw, 46);
    kit.NumberField("mu.seg.entry", "Entry", Num(NumberAt(j, "entryFrame", 0.0), 480, 0, 1e12, 0, "fr", true),
                    [d, g](double v, std::uint32_t) {
                      std::uint64_t n = 0;
                      return WholeU64(v, &n) && SegmentEditor(*d, g).SetEntryFrame(n);
                    },
                    2 * (cw + 8), y, cw, 46);
    kit.NumberField("mu.seg.exit", "Exit", Num(NumberAt(j, "exitFrame", 0.0), 480, 0, 1e12, 0, "fr", true),
                    [d, g](double v, std::uint32_t) {
                      std::uint64_t n = 0;
                      return WholeU64(v, &n) && SegmentEditor(*d, g).SetExitFrame(n);
                    },
                    3 * (cw + 8), y, cw, 46);
    y += kRow;
    const bool pre = BoolAt(j, "entryPreload", false);
    kit.Toggle("mu.seg.preload", "entry preload", pre, [d, g, pre] { SegmentEditor(*d, g).SetEntryPreload(!pre); }, 0, y, 140);
    // Tap tempo and the grid (8.7).
    App* a = app;
    kit.Action("mu.seg.tap", "Tap (" + std::to_string(mv.tap.count()) + ")", [mvp] {
      mvp->tap.Tap(static_cast<double>(Session::NowNs()) * 1e-9);
      mvp->tapBpm = mvp->tap.Bpm();
    }, 150, y, 80);
    kit.Action("mu.seg.tap.reset", "Reset", [mvp] {
      mvp->tap.Reset();
      mvp->tapBpm = 0.0;
    }, 234, y, 54);
    NumberSpec bpm = Num(mv.tapBpm > 0.0 ? mv.tapBpm : 120.0, 1, 1e-3, 1e5, 2, "bpm");
    bpm.document = false;
    kit.NumberField("mu.seg.grid.bpm", "", bpm, [mvp](double v, std::uint32_t) {
      mvp->tapBpm = v;
      return true;
    }, 294, y, 140);
    NumberSpec off = Num(mv.gridOffsetMs, 5, 0, 1e7, 1, "ms");
    off.document = false;
    kit.NumberField("mu.seg.grid.offset", "", off, [mvp](double v, std::uint32_t) {
      mvp->gridOffsetMs = v;
      return true;
    }, 438, y, 130);
    const double rate = se.SampleRate();
    kit.Action("mu.seg.grid.apply", "Apply grid", [d, g, mvp, rate, a] {
      const double b = mvp->tapBpm > 0.0 ? mvp->tapBpm : 120.0;
      if (!SegmentEditor(*d, g).ApplyGrid(b, SatU64(mvp->gridOffsetMs * 1e-3 * rate), 4, 4)) {
        a->Message("refused: grid (no material?)");
      }
    }, 572, y, 90, 22, rate > 0.0);
    y += kRow;
    SegmentLine(kit, g, 0, y, w, 70);
    y += 92.0f;
    // Tempo points | markers | stems.
    const float col = (w - 16.0f) / 3.0f;
    const std::vector<TempoPointDesc> tempo = se.Tempo();
    kit.Label("mu.tp.title", "tempo (" + std::to_string(tempo.size()) + ")", 0, y, col - 70, 22, 0.6f);
    kit.Action("mu.tp.add", "+ Tempo", [d, g] {
      SegmentEditor e(*d, g);
      const std::vector<TempoPointDesc> t = e.Tempo();
      TempoPointDesc p = t.empty() ? TempoPointDesc{} : t.back();
      p.tick = t.empty() ? 0u : t.back().tick + static_cast<std::uint64_t>(e.Map().BarTicks(static_cast<std::uint32_t>(t.size() - 1u)));
      e.AddTempo(p);
    }, col - 66, y, 64);
    const std::vector<MarkerDesc> markers = se.Markers();
    kit.Label("mu.mk.title", "markers (" + std::to_string(markers.size()) + ")", col + 8, y, col - 70, 22, 0.6f);
    kit.Action("mu.mk.add", "+ Marker", [d, g, a] {
      SegmentEditor e(*d, g);
      MarkerDesc m;
      for (std::uint32_t n = 1; n < 10000u; ++n) {
        m.name = "Marker " + std::to_string(n);
        bool taken = false;
        for (const MarkerDesc& o : e.Markers()) taken = taken || o.name == m.name;
        if (!taken) break;
      }
      m.frame = SatU64(NumberAt(a->document()->Find(g)->doc, "entryFrame", 0.0));
      e.AddMarker(m);
    }, 2 * col + 8 - 66, y, 64);
    const json::Value* stems = j.Find("stems");
    const std::uint32_t nst = stems != nullptr && stems->IsArray() ? static_cast<std::uint32_t>(stems->items.size()) : 0u;
    kit.Label("mu.st.title", "stems (" + std::to_string(nst) + ")", 2 * col + 16, y, col - 70, 22, 0.6f);
    kit.Action("mu.st.add", "+ Stem", [d, g] {
      StemDesc s;
      s.channelFirst = 0;
      s.channelCount = 1;
      SegmentEditor(*d, g).AddStem(s);
    }, w - 66, y, 64);
    y += 24.0f;
    const float rowsH = height - y - 60.0f;
    const std::uint32_t per = static_cast<std::uint32_t>(std::max(1.0f, rowsH / (2.0f * kRow)));
    const std::uint32_t first = kit.Pager("mu.rows", static_cast<std::uint32_t>(std::max<std::size_t>({tempo.size(), markers.size(), nst})),
                                          per, &mv.rowPage, 0, y, 200);
    y += 22.0f;
    for (std::uint32_t i = first; i < tempo.size() && i < first + per; ++i) {
      const TempoPointDesc p = tempo[i];
      const float ry = y + static_cast<float>(i - first) * 2.0f * kRow;
      const std::string id = "mu.tp." + std::to_string(i);
      kit.NumberField(id + ".tick", "", Num(static_cast<double>(p.tick), 960, 0, 1e15, 0, "tk", true), [d, g, i, p](double v, std::uint32_t) {
        TempoPointDesc n = p;
        if (!WholeU64(v, &n.tick)) return false;
        return SegmentEditor(*d, g).SetTempo(i, n);
      }, 0, ry, col * 0.5f - 2);
      kit.NumberField(id + ".bpm", "", Num(p.bpm, 1, 1e-3, 1e5, 2, "bpm"), [d, g, i, p](double v, std::uint32_t) {
        TempoPointDesc n = p;
        n.bpm = v;
        return SegmentEditor(*d, g).SetTempo(i, n);
      }, col * 0.5f + 2, ry, col * 0.5f - 30);
      kit.Action(id + ".remove", "x", [d, g, i] { SegmentEditor(*d, g).RemoveTempo(i); }, col - 24, ry, 24);
      kit.NumberField(id + ".beats", "", Num(p.beatsPerBar, 1, 1, 64, 0, "/bar", true), [d, g, i, p](double v, std::uint32_t) {
        TempoPointDesc n = p;
        if (!WholeU32(v, &n.beatsPerBar)) return false;
        return SegmentEditor(*d, g).SetTempo(i, n);
      }, 0, ry + kRow, col * 0.5f - 2);
      kit.PickField(id + ".unit", "", "1/" + std::to_string(p.beatUnit), "Beat unit", [] {
        std::vector<PickerItem> v;
        for (const char* u : {"1", "2", "4", "8", "16", "32"}) v.push_back(PickerItem{std::string("1/") + u, u});
        return v;
      }, [d, g, i, p, a](const std::string& v) {
        TempoPointDesc n = p;
        n.beatUnit = static_cast<std::uint32_t>(ToInt(v));
        if (!SegmentEditor(*d, g).SetTempo(i, n)) a->Message("refused: a signature changes on a bar line only");
      }, col * 0.5f + 2, ry + kRow, col * 0.5f - 2);
    }
    std::vector<std::string> mnames;
    for (const MarkerDesc& m : markers) mnames.push_back(m.name);
    const std::vector<std::string> mkeys = NameKeys::Of(mnames);
    for (std::uint32_t i = first; i < markers.size() && i < first + per; ++i) {
      const MarkerDesc m = markers[i];
      const float ry = y + static_cast<float>(i - first) * 2.0f * kRow;
      const std::string id = "mu.mk." + mkeys[i];
      kit.NameField(id + ".name", mv.marker == m.name ? ">" : "", m.name, [d, g, i, m, mvp](const std::string& s) {
        MarkerDesc n = m;
        n.name = s;
        if (!SegmentEditor(*d, g).SetMarker(i, n)) return false;
        if (mvp->marker == m.name) mvp->marker = s;  // the selection follows the rename
        return true;
      }, col + 8, ry, col * 0.6f, 10);
      kit.PickField(id + ".kind", "", kMarkerKind[static_cast<int>(m.kind)], "Marker kind", [] { return Names(kMarkerKind, 4); },
                    [d, g, i, m](const std::string& v) {
                      MarkerDesc n = m;
                      n.kind = static_cast<MarkerKind>(ToInt(v));
                      SegmentEditor(*d, g).SetMarker(i, n);
                    }, col + 8 + col * 0.6f + 4, ry, col * 0.4f - 32);
      kit.Action(id + ".remove", "x", [d, g, i, mvp, m] {
        if (SegmentEditor(*d, g).RemoveMarker(i) && mvp->marker == m.name) mvp->marker.clear();
      }, 2 * col + 8 - 24, ry, 24);
      kit.NumberField(id + ".frame", "", Num(static_cast<double>(m.frame), 480, 0, 1e15, 0, "fr", true), [d, g, i, m](double v, std::uint32_t) {
        MarkerDesc n = m;
        if (!WholeU64(v, &n.frame)) return false;
        return SegmentEditor(*d, g).SetMarker(i, n);
      }, col + 8, ry + kRow, col);
    }
    for (std::uint32_t i = first; i < nst && i < first + per; ++i) {
      const json::Value& sv = stems->items[i];
      StemDesc s;
      s.channelFirst = SatU32(NumberAt(sv, "channelFirst", 0.0));
      s.channelCount = SatU32(NumberAt(sv, "channelCount", 2.0), 2u);
      s.asset = RefGuid(*d, "asset", StringAt(sv, "asset"));
      const float rx = 2 * col + 16, ry = y + static_cast<float>(i - first) * 2.0f * kRow;
      const std::string id = "mu.st." + std::to_string(i);
      kit.NumberField(id + ".first", "", Num(s.channelFirst, 1, 0, 64, 0, "ch", true), [d, g, i, s](double v, std::uint32_t) {
        StemDesc n = s;
        if (!WholeU32(v, &n.channelFirst)) return false;
        return SegmentEditor(*d, g).SetStem(i, n);
      }, rx, ry, col * 0.5f - 2);
      kit.NumberField(id + ".count", "", Num(s.channelCount, 1, 1, 64, 0, "n", true), [d, g, i, s](double v, std::uint32_t) {
        StemDesc n = s;
        if (!WholeU32(v, &n.channelCount)) return false;
        return SegmentEditor(*d, g).SetStem(i, n);
      }, rx + col * 0.5f + 2, ry, col * 0.5f - 30);
      kit.Action(id + ".remove", "x", [d, g, i] { SegmentEditor(*d, g).RemoveStem(i); }, rx + col - 24, ry, 24);
      kit.PickField(id + ".asset", "", RefName(*d, "asset", s.asset, "(segment's)"), "Stem material",
                    [d] { return DocItems(*d, "asset", "(segment's)"); }, [d, g, i, s](const std::string& v) {
                      StemDesc n = s;
                      n.asset = v;
                      SegmentEditor(*d, g).SetStem(i, n);
                    }, rx, ry + kRow, col);
    }
    const std::vector<std::string> problems = se.Problems();
    for (std::size_t i = 0; i < problems.size() && i < 2u; ++i) {
      kit.Label("mu.seg.problem." + std::to_string(i), "warning: " + problems[i], 0, height - 52.0f + static_cast<float>(i) * 18.0f,
                w, 18, 0.5f);
    }
  }, 6.0f);
}

void PlaylistsTab(Kit& kit, float width, float height) {
  App* app = &kit.app();
  ProjectDocument* d = app->document();
  MusicView& mv = app->view().music;
  ObjectListSpec ls;
  ls.id = "mu.pl.list";
  ls.kind = "playlist";
  ls.title = "Playlists";
  ls.selected = &mv.playlist;
  ls.page = &mv.listPage;
  ls.create = [d] {
    json::Value init = json::MakeObject();
    const std::string bank = FirstBank(*d);
    if (!bank.empty()) json::Add(init, "bank", String(bank));
    return d->Create("playlist", NewName(*d, "playlist", "Playlist"), &init);
  };
  Panel(kit.doc(), "mu.pllist", 0, 0, kListW, height, [&] { ObjectList(kit, ls, 0, 0, kListW - 12, height - 12); }, 6.0f);
  if (mv.playlist.empty() || d->Find(mv.playlist) == nullptr) return;
  const std::string g = mv.playlist;
  const json::Value& j = d->Find(g)->doc;
  const float mx = kListW + 8.0f, mw = std::min(700.0f, width - mx);
  Panel(kit.doc(), "mu.plmain", mx, 0, mw, height, [&] {
    const float w = mw - 12.0f;
    kit.PickField("mu.pl.mode", "Mode", StringAt(j, "mode", "sequential"), "Playlist mode", [] { return Names(kMode, 3); },
                  [d, g](const std::string& v) { PlaylistEditor(*d, g).SetMode(static_cast<PlaylistMode>(ToInt(v))); }, 0, 0,
                  w * 0.5f - 4, 50);
    kit.NumberField("mu.pl.loops", "Loops", Num(NumberAt(j, "loopCount", 0.0), 1, 0, 1e6, 0, "(0=inf)", true),
                    [d, g](double v, std::uint32_t) {
                      std::uint32_t n = 0;
                      return WholeU32(v, &n) && PlaylistEditor(*d, g).SetLoopCount(n);
                    },
                    w * 0.5f + 4, 0, w * 0.5f - 4, 50);
    const json::Value* e = j.Find("entries");
    const std::uint32_t n = e != nullptr && e->IsArray() ? static_cast<std::uint32_t>(e->items.size()) : 0u;
    kit.Label("mu.pl.entries", "entries (" + std::to_string(n) + ")", 0, kRow + 4, w - 110, 22, 0.6f);
    kit.PickField("mu.pl.add", "", "+ Entry", "Segment", [d] { return DocItems(*d, "segment"); },
                  [d, g](const std::string& v) { PlaylistEditor(*d, g).AddEntry(v); }, w - 104, kRow + 4, 104);
    float y = 2.0f * kRow + 8.0f;
    const std::uint32_t per = static_cast<std::uint32_t>(std::max(1.0f, (height - y - 30.0f) / kRow));
    const std::uint32_t first = kit.Pager("mu.pl.pager", n, per, &mv.rowPage, 0, y, 200);
    if (n > per) y += 22.0f;
    for (std::uint32_t i = first; i < n && i < first + per; ++i) {
      const json::Value& en = e->items[i];
      const std::string seg = RefGuid(*d, "segment", StringAt(en, "segment"));
      const double wgt = NumberAt(en, "weight", 1.0);
      const std::uint32_t rep = SatU32(NumberAt(en, "repeat", 1.0), 1u);
      const std::string id = "mu.pl.e" + std::to_string(i);
      kit.PickField(id + ".seg", "", RefName(*d, "segment", seg), "Segment", [d] { return DocItems(*d, "segment"); },
                    [d, g, i, wgt, rep](const std::string& v) { PlaylistEditor(*d, g).SetEntry(i, v, wgt, rep); }, 0, y,
                    w * 0.3f);
      kit.NumberField(id + ".w", "w", Num(wgt, 0.1, 0, 1e6, 2), [d, g, i, seg, rep](double v, std::uint32_t) {
        return PlaylistEditor(*d, g).SetEntry(i, seg, v, rep);
      }, w * 0.3f + 4, y, w * 0.25f, 16);
      kit.NumberField(id + ".r", "x", Num(rep, 1, 1, 1e6, 0, "", true), [d, g, i, seg, wgt](double v, std::uint32_t) {
        std::uint32_t times = 0;
        return WholeU32(v, &times) && PlaylistEditor(*d, g).SetEntry(i, seg, wgt, times);
      }, w * 0.55f + 8, y, w * 0.22f, 16);
      kit.Action(id + ".up", "^", [d, g, i] { PlaylistEditor(*d, g).MoveEntry(i, i - 1u); }, w * 0.77f + 12, y, 24, 22, i > 0u);
      kit.Action(id + ".down", "v", [d, g, i] { PlaylistEditor(*d, g).MoveEntry(i, i + 1u); }, w * 0.77f + 40, y, 24, 22,
                 i + 1u < n);
      kit.Action(id + ".remove", "x", [d, g, i] { PlaylistEditor(*d, g).RemoveEntry(i); }, w - 24, y, 24);
      y += kRow;
    }
  }, 6.0f);
}

// --- the music event: states, rules, preview, lookahead ----------------------------------------
std::vector<PickerItem> StateItems(const ProjectDocument& doc, const std::string& ev, bool wildcard) {
  std::vector<PickerItem> v;
  if (wildcard) v.push_back(PickerItem{"*", "*"});
  for (const std::string& s : MusicEditor(const_cast<ProjectDocument&>(doc), ev).States()) v.push_back(PickerItem{s, s});
  return v;
}

bool MusicEventList(Kit& kit, float height) {
  App* app = &kit.app();
  ProjectDocument* d = app->document();
  MusicView& mv = app->view().music;
  ObjectListSpec ls;
  ls.id = "mu.ev.list";
  ls.kind = "event";
  ls.title = "Music events";
  ls.selected = &mv.event;
  ls.page = &mv.listPage;
  ls.filter = [](const DocObject& o) { return o.doc.Find("music") != nullptr; };
  ls.create = [d] {
    json::Value init = json::MakeObject();
    const std::string bank = FirstBank(*d);
    if (!bank.empty()) json::Add(init, "bank", String(bank));
    json::Add(init, "music", json::MakeObject());
    return d->Create("event", NewName(*d, "event", "Music"), &init);
  };
  Panel(kit.doc(), "mu.evlist", 0, 0, kListW, height, [&] { ObjectList(kit, ls, 0, 0, kListW - 12, height - 12); }, 6.0f);
  return !mv.event.empty();
}

void RulesTab(Kit& kit, float width, float height) {
  App* app = &kit.app();
  ProjectDocument* d = app->document();
  MusicView& mv = app->view().music;
  MusicView* mvp = &mv;
  if (!MusicEventList(kit, height)) return;
  const std::string ev = mv.event;
  MusicEditor me(*d, ev);
  const float mx = kListW + 8.0f, mw = width - mx;
  Panel(kit.doc(), "mu.rules", mx, 0, mw, height, [&] {
    const float w = mw - 12.0f;
    float y = 0.0f;
    // States.
    const std::vector<std::string> states = me.States();
    const std::string initial = StringAt(d->Find(ev)->doc, "music.initialState");
    kit.Label("mu.state.title", "states (" + std::to_string(states.size()) + ")", 0, y, 200, 22, 0.6f);
    kit.Action("mu.state.add", "+ State", [d, ev, app] {
      MusicEditor e(*d, ev);
      const auto pls = d->OfKind("playlist");
      if (pls.empty()) return app->Message("a state needs a playlist: make one first");
      std::string name;
      const std::vector<std::string> taken = e.States();
      for (std::uint32_t n = 1; n < 10000u; ++n) {
        name = "State " + std::to_string(n);
        if (std::find(taken.begin(), taken.end(), name) == taken.end()) break;
      }
      e.AddState(name, pls.front()->guid);
    }, 204, y, 70);
    y += 24.0f;
    const float sw = (w - 8.0f) / 2.0f;
    const std::vector<std::string> skeys = NameKeys::Of(states);
    for (std::size_t i = 0; i < states.size() && i < 8u; ++i) {
      const std::string s = states[i];
      const float sx = static_cast<float>(i % 2u) * (sw + 8.0f), sy = y + static_cast<float>(i / 2u) * kRow;
      const std::string id = "mu.state." + skeys[i];
      kit.NameField(id + ".name", "", s, [d, ev, s](const std::string& n) { return MusicEditor(*d, ev).RenameState(s, n); },
                    sx, sy, sw * 0.35f);
      const std::string pl = StringAt(d->Find(ev)->doc, "music.states[" + std::to_string(i) + "].playlist");
      kit.PickField(id + ".pl", "", RefName(*d, "playlist", pl), "Playlist", [d] { return DocItems(*d, "playlist"); },
                    [d, ev, s](const std::string& v) { MusicEditor(*d, ev).SetStatePlaylist(s, v); }, sx + sw * 0.35f + 4, sy,
                    sw * 0.35f);
      kit.Toggle(id + ".initial", "init", s == initial, [d, ev, s] { MusicEditor(*d, ev).SetInitialState(s); },
                 sx + sw * 0.7f + 8, sy, sw * 0.18f);
      kit.Action(id + ".remove", "x", [d, ev, s, app] {
        if (!MusicEditor(*d, ev).RemoveState(s)) app->Message("refused: a rule or the initial state names it");
      }, sx + sw - 24, sy, 24);
    }
    if (states.size() > 8u) kit.Label("mu.state.more", "+" + std::to_string(states.size() - 8u) + " states", 0, y + 4 * kRow, w, 18, 0.5f);
    y += 4.0f * kRow + 4.0f;
    // The transition rule table.
    const std::vector<TransitionRule> rules = me.Rules();
    const auto unreachable = me.Unreachable();
    kit.Label("mu.rule.title", "transitions (" + std::to_string(rules.size()) + "), first match from the top", 0, y, 300, 22, 0.6f);
    kit.Action("mu.rule.add", "+ Rule", [d, ev, mvp] {
      MusicEditor e(*d, ev);
      TransitionRule r;
      if (e.AddRule(r, static_cast<std::uint32_t>(e.Rules().size()))) mvp->rule = static_cast<std::int32_t>(e.Rules().size()) - 1;
    }, 304, y, 64);
    kit.Action("mu.rule.sort", "Auto sort", [d, ev, app] {
      std::string why;
      if (!MusicEditor(*d, ev).AutoSort(&why)) app->Message("auto sort refused: " + why);
    }, 372, y, 80);
    y += 24.0f;
    static const char* const kHead[] = {"from", "to", "timing", "n", "fade out", "fade in", "bridge", "start", "late", "prep"};
    const float cols[] = {0.0f, 0.09f, 0.18f, 0.28f, 0.35f, 0.47f, 0.59f, 0.69f, 0.78f, 0.87f};
    for (int c = 0; c < 10; ++c) kit.Label("mu.rule.h" + std::to_string(c), kHead[c], cols[c] * w, y, 80, 16, 0.45f);
    y += 16.0f;
    const float tableH = std::max(kRow * 3.0f, height * 0.58f - y);
    const std::uint32_t per = static_cast<std::uint32_t>(std::max(1.0f, tableH / kRow));
    const std::uint32_t first = kit.Pager("mu.rule.pager", static_cast<std::uint32_t>(rules.size()), per, &mv.rulePage, w - 200, y - 40, 200);
    for (std::uint32_t i = first; i < rules.size() && i < first + per; ++i) {
      const TransitionRule r = rules[i];
      const float ry = y + static_cast<float>(i - first) * kRow;
      const std::string id = "mu.rule." + std::to_string(i);
      auto set = [d, ev, i, r, app](auto change) {
        TransitionRule n = r;
        change(n);
        if (!MusicEditor(*d, ev).SetRule(i, n)) app->Message("refused: rule " + std::to_string(i + 1u));
      };
      kit.PickField(id + ".from", "", r.from, "From", [d, ev] { return StateItems(*d, ev, true); },
                    [set](const std::string& v) { set([&](TransitionRule& n) { n.from = v; }); }, 0, ry, cols[1] * w - 2);
      kit.PickField(id + ".to", "", r.to, "To", [d, ev] { return StateItems(*d, ev, true); },
                    [set](const std::string& v) { set([&](TransitionRule& n) { n.to = v; }); }, cols[1] * w, ry,
                    (cols[2] - cols[1]) * w - 2);
      kit.PickField(id + ".timing", "", kTiming[static_cast<int>(r.timing)], "Timing", [] { return Names(kTiming, 7); },
                    [set](const std::string& v) { set([&](TransitionRule& n) { n.timing = static_cast<Timing>(ToInt(v)); }); },
                    cols[2] * w, ry, (cols[3] - cols[2]) * w - 2);
      kit.NumberField(id + ".count", "", Num(r.timingCount, 1, 0, 1e6, 0, "", true), [d, ev, i, r](double v, std::uint32_t) {
        TransitionRule n = r;
        if (!WholeU32(v, &n.timingCount)) return false;
        return MusicEditor(*d, ev).SetRule(i, n);
      }, cols[3] * w, ry, (cols[4] - cols[3]) * w - 2);
      kit.NumberField(id + ".fo", "", Num(r.fadeOutBeats, 0.5, 0, 1e6, 2, "b"), [d, ev, i, r](double v, std::uint32_t) {
        TransitionRule n = r;
        n.fadeOutBeats = v;
        return MusicEditor(*d, ev).SetRule(i, n);
      }, cols[4] * w, ry, (cols[5] - cols[4]) * w - 2);
      kit.NumberField(id + ".fi", "", Num(r.fadeInBeats, 0.5, 0, 1e6, 2, "b"), [d, ev, i, r](double v, std::uint32_t) {
        TransitionRule n = r;
        n.fadeInBeats = v;
        return MusicEditor(*d, ev).SetRule(i, n);
      }, cols[5] * w, ry, (cols[6] - cols[5]) * w - 2);
      kit.PickField(id + ".bridge", "", RefName(*d, "segment", r.bridge, "-"), "Bridge",
                    [d] { return DocItems(*d, "segment", "(none)"); },
                    [set](const std::string& v) { set([&](TransitionRule& n) { n.bridge = v; }); }, cols[6] * w, ry,
                    (cols[7] - cols[6]) * w - 2);
      kit.PickField(id + ".start", "", kStart[static_cast<int>(r.start)], "Start", [] { return Names(kStart, 4); },
                    [set](const std::string& v) { set([&](TransitionRule& n) { n.start = static_cast<StartPosition>(ToInt(v)); }); },
                    cols[7] * w, ry, (cols[8] - cols[7]) * w - 2);
      kit.PickField(id + ".late", "", kLate[static_cast<int>(r.late)], "Late rule", [] { return Names(kLate, 3); },
                    [set](const std::string& v) { set([&](TransitionRule& n) { n.late = static_cast<LateRule>(ToInt(v)); }); },
                    cols[8] * w, ry, (cols[9] - cols[8]) * w - 2);
      kit.Toggle(id + ".prep", "", r.prepare, [d, ev, i, r] {
        TransitionRule n = r;
        n.prepare = !r.prepare;
        MusicEditor(*d, ev).SetRule(i, n);
      }, cols[9] * w, ry, 30);
      kit.Action(id + ".up", "^", [d, ev, i] { MusicEditor(*d, ev).MoveRule(i, i - 1u); }, cols[9] * w + 32, ry, 20, 22, i > 0u);
      kit.Action(id + ".down", "v", [d, ev, i] { MusicEditor(*d, ev).MoveRule(i, i + 1u); }, cols[9] * w + 54, ry, 20, 22,
                 i + 1u < rules.size());
      kit.Action(id + ".remove", "x", [d, ev, i] { MusicEditor(*d, ev).RemoveRule(i); }, w - 22, ry, 22);
      for (const auto& [row, cover] : unreachable) {
        if (row == i) {
          kit.Label(id + ".unreachable", "! row " + std::to_string(cover + 1u), cols[9] * w + 76, ry, w - 22 - (cols[9] * w + 76), 22,
                    0.45f);
        }
      }
    }
    y += static_cast<float>(per) * kRow + 6.0f;
    if (!unreachable.empty()) {
      kit.Label("mu.rule.warn", "warning: " + std::to_string(unreachable.size()) +
                                    " unreachable rows (an earlier row covers them; auto sort is refused until fixed)",
                0, y, w, 18, 0.5f);
      y += 18.0f;
    }
    // Transition preview and audition.
    if (mv.from.empty() && !states.empty()) mv.from = states.front();
    if (mv.to.empty() && states.size() > 1u) mv.to = states[1];
    kit.PickField("mu.pv.from", "From", mv.from, "From state", [d, ev] { return StateItems(*d, ev, false); },
                  [mvp](const std::string& v) { mvp->from = v; }, 0, y, w * 0.2f, 40);
    kit.PickField("mu.pv.to", "To", mv.to, "To state", [d, ev] { return StateItems(*d, ev, false); },
                  [mvp](const std::string& v) { mvp->to = v; }, w * 0.2f + 4, y, w * 0.2f, 30);
    NumberSpec req = Num(mv.requestSeconds, 0.05, 0, MusicEditor::kMaxRequestSeconds, 3, "s");
    req.document = false;
    kit.NumberField("mu.pv.req", "at", req, [mvp](double v, std::uint32_t) {
      // A screen value with a range (D-187): refused outside 0 .. kMaxRequestSeconds.
      if (!std::isfinite(v) || v < 0.0 || v > MusicEditor::kMaxRequestSeconds) return false;
      mvp->requestSeconds = v;
      return true;
    }, w * 0.4f + 8, y, w * 0.2f, 24);
    NumberSpec prime = Num(mv.settings.streamPrimeMs, 5, 0, 1e5, 1, "ms prime");
    prime.document = false;
    kit.NumberField("mu.pv.prime", "", prime, [mvp](double v, std::uint32_t) {
      if (!std::isfinite(v) || v < 0.0 || v > 1e5) return false;
      mvp->settings.streamPrimeMs = v;
      return true;
    }, w * 0.6f + 12, y, w * 0.2f);
    AuditionRouter* router = app->workspace().audition;
    std::string why = "no audition router";
    const bool ready = router != nullptr && router->Ready(&why);
    const std::string evName = d->Find(ev)->name();
    kit.Action("mu.pv.play", "Play transition", [router, mvp, evName, app] {
      TransitionAudition t{HashId(evName), HashId(mvp->from), HashId(mvp->to), mvp->requestSeconds};
      const AuditionResult r = router->PlayTransition(t);
      if (!r.ok) app->Message("transition audition: " + (r.error.empty() ? std::string("refused") : r.error));
      else if (!r.unsupported.empty()) app->Message("transition audition: " + r.unsupported.front());
    }, w * 0.8f + 16, y, w * 0.2f - 16, 22, ready && !mv.from.empty() && !mv.to.empty());
    y += kRow;
    if (!ready) {
      kit.Label("mu.pv.why", "audition: " + why, w * 0.6f, y - 4, w * 0.4f, 16, 0.45f);
    }
    const TransitionPreview p = me.Preview(mv.from, mv.to, mv.requestSeconds, mv.settings);
    char b[256];
    if (!p.valid) {
      kit.Label("mu.pv.text", "choose two states", 0, y, w, 18, 0.5f);
      return;
    }
    // Shown 1-based in 64 bits (D-189: no signed overflow in display arithmetic); a position the model
    // saturated (a broken tempo point far away) has no number to show.
    auto oneBased = [](std::int32_t v) {
      if (v == std::numeric_limits<std::int32_t>::max() || v == std::numeric_limits<std::int32_t>::min()) return std::string("-");
      return std::to_string(static_cast<long long>(v) + 1);
    };
    SnprintfFinite(b, sizeof b, "row %s  fires at %.3f s (bar %s beat %s)%s%s  lookahead %.1f ms  fade out %.3f s in %.3f s",
                  p.rule < 0 ? "default" : std::to_string(static_cast<long long>(p.rule) + 1).c_str(), p.fireSeconds,
                  oneBased(p.fireAt.bar).c_str(), oneBased(p.fireAt.beat).c_str(), p.lateApplied ? " (late rule)" : "",
                  p.cancelled ? " CANCELLED" : "", p.lookaheadMs, p.fadeOutSeconds, p.fadeInSeconds);
    kit.Label("mu.pv.text", b, 0, y, w, 18, 0.5f);
    y += 18.0f;
    SnprintfFinite(b, sizeof b, "bridge %s (%.3f s)  target %s heard at %.3f s from tick %.0f", RefName(*d, "segment", p.bridge, "none").c_str(),
                  p.bridgeSeconds, RefName(*d, "segment", p.targetSegment).c_str(), p.targetStartSeconds, p.targetStartTick);
    kit.Label("mu.pv.text2", b, 0, y, w, 18, 0.5f);
    y += 20.0f;
    // The preview on a time line: request, firing, fades, bridge, target.
    const double end = std::max({p.targetStartSeconds + std::max(0.5, p.fadeInSeconds), p.fireSeconds + p.fadeOutSeconds, 1.0});
    const double pps = w / (end * 1.05);
    MarkLook look;
    look.dpi = kit.ctx().dpi;
    look.width = w;
    look.height = 46.0f;
    const auto X = [pps](double s) { return static_cast<float>(s * pps); };
    look.spans.push_back(Span{X(p.requestSeconds), X(p.requestSeconds + p.lookaheadMs * 1e-3), SpanKind::Lookahead, 0, 8});
    look.spans.push_back(Span{X(p.fireSeconds), X(p.fireSeconds + p.fadeOutSeconds), SpanKind::FadeOut, 10, 16});
    if (!p.bridge.empty()) look.spans.push_back(Span{X(p.fireSeconds), X(p.fireSeconds + p.bridgeSeconds), SpanKind::Bridge, 28, 8});
    look.spans.push_back(Span{X(p.targetStartSeconds), X(p.targetStartSeconds + p.fadeInSeconds), SpanKind::FadeIn, 28, 16});
    look.marks.push_back(Mark{X(p.requestSeconds), MarkKind::Request});
    if (!p.cancelled) look.marks.push_back(Mark{X(p.fireSeconds), MarkKind::Fire});
    look.marks.push_back(Mark{X(p.targetStartSeconds), MarkKind::Target});
    Canvas(kit.doc(), "mu.pv.line", DrawMarks(look, kit.palette()), 0, y, w, 46);
    y += 52.0f;
    // Minimum lookahead per reservation target.
    const std::vector<LookaheadEntry> la = me.MinimumLookahead(mv.settings);
    const std::uint32_t lper = static_cast<std::uint32_t>(std::max(1.0f, (height - y - 24.0f) / 18.0f));
    const std::uint32_t lfirst = kit.Pager("mu.la.pager", static_cast<std::uint32_t>(la.size()), lper, &mv.lookPage, w - 200, y, 200);
    kit.Label("mu.la.title", "minimum lookahead (1 block + prime)", 0, y, w - 210, 18, 0.55f);
    y += 20.0f;
    for (std::uint32_t i = lfirst; i < la.size() && i < lfirst + lper; ++i) {
      const LookaheadEntry& e = la[i];
      SnprintfFinite(b, sizeof b, "%s %s: %s  prime %.1f ms  lookahead %.1f ms", e.kind.c_str(), e.name.c_str(), e.reason.c_str(),
                    e.primeMs, e.lookaheadMs);
      kit.Label("mu.la." + std::to_string(i - lfirst), b, 0, y, w, 18, 0.5f);
      y += 18.0f;
    }
  }, 6.0f);
}

void StingersTab(Kit& kit, float width, float height) {
  App* app = &kit.app();
  ProjectDocument* d = app->document();
  if (!MusicEventList(kit, height)) return;
  MusicView& mv = app->view().music;
  const std::string ev = mv.event;
  const float mx = kListW + 8.0f, mw = std::min(760.0f, width - mx);
  Panel(kit.doc(), "mu.stingers", mx, 0, mw, height, [&] {
    const float w = mw - 12.0f;
    const json::Value* st = FindPath(d->Find(ev)->doc, "music.stingers");
    const std::uint32_t n = st != nullptr && st->IsArray() ? static_cast<std::uint32_t>(st->items.size()) : 0u;
    kit.Label("mu.sg.title", "stingers (" + std::to_string(n) + ")", 0, 0, 200, 22, 0.6f);
    kit.PickField("mu.sg.add", "", "+ Stinger", "Stinger event", [d] { return DocItems(*d, "event"); },
                  [d, ev, n](const std::string& v) {
                    StingerDesc s;
                    s.name = "Stinger " + std::to_string(n + 1u);
                    s.event = v;
                    MusicEditor(*d, ev).AddStinger(s);
                  }, 204, 0, 100);
    float y = 26.0f;
    std::vector<std::string> snames;
    for (std::uint32_t i = 0; i < n; ++i) snames.push_back(StringAt(st->items[i], "name"));
    const std::vector<std::string> skeys = NameKeys::Of(snames);
    for (std::uint32_t i = 0; i < n && y < height - kRow; ++i) {
      const json::Value& sj = st->items[i];
      StingerDesc s;
      s.name = StringAt(sj, "name");
      s.event = RefGuid(*d, "event", StringAt(sj, "event"));
      for (int k = 0; k < 7; ++k) {
        if (StringAt(sj, "timing", "nextBeat") == kTiming[k]) s.timing = static_cast<Timing>(k);
      }
      s.timingCount = SatU32(NumberAt(sj, "timingCount", 0.0));
      s.stack = StringAt(sj, "overlap", "replace") == "stack";
      const std::string id = "mu.sg." + skeys[i];
      kit.NameField(id + ".name", "", s.name, [d, ev, i, s](const std::string& v) {
        StingerDesc x = s;
        x.name = v;
        return MusicEditor(*d, ev).SetStinger(i, x);
      }, 0, y, w * 0.2f);
      kit.PickField(id + ".event", "", RefName(*d, "event", s.event), "Event", [d] { return DocItems(*d, "event"); },
                    [d, ev, i, s](const std::string& v) {
                      StingerDesc x = s;
                      x.event = v;
                      MusicEditor(*d, ev).SetStinger(i, x);
                    }, w * 0.2f + 4, y, w * 0.25f);
      kit.PickField(id + ".timing", "", kTiming[static_cast<int>(s.timing)], "Timing", [] { return Names(kTiming, 7); },
                    [d, ev, i, s](const std::string& v) {
                      StingerDesc x = s;
                      x.timing = static_cast<Timing>(ToInt(v));
                      MusicEditor(*d, ev).SetStinger(i, x);
                    }, w * 0.45f + 8, y, w * 0.18f);
      kit.NumberField(id + ".count", "", Num(s.timingCount, 1, 0, 1e6, 0, "", true), [d, ev, i, s](double v, std::uint32_t) {
        StingerDesc x = s;
        if (!WholeU32(v, &x.timingCount)) return false;
        return MusicEditor(*d, ev).SetStinger(i, x);
      }, w * 0.63f + 12, y, w * 0.15f);
      kit.Toggle(id + ".stack", "stack", s.stack, [d, ev, i, s] {
        StingerDesc x = s;
        x.stack = !s.stack;
        MusicEditor(*d, ev).SetStinger(i, x);
      }, w * 0.78f + 16, y, w * 0.14f);
      kit.Action(id + ".remove", "x", [d, ev, i] { MusicEditor(*d, ev).RemoveStinger(i); }, w - 24, y, 24);
      y += kRow;
    }
  }, 6.0f);
}

}  // namespace

void DeclareMusic(tela::Document& doc, ViewContext& ctx, float width, float height) {
  Kit kit(doc, ctx);
  if (!NeedDocument(kit, "mu.nodoc", width)) return;
  App* app = ctx.app;
  MusicView& mv = app->view().music;
  MusicView* mvp = &mv;
  kit.Tabs("mu.tab", {"Segments", "Playlists", "States & rules", "Stingers"}, mv.tab, [mvp](std::uint32_t i) {
    mvp->tab = i;
    mvp->listPage = 0;
    mvp->rowPage = 0;
  }, 0, 0, 120);
  const float top = 30.0f;
  Panel(doc, "mu.body", 0, top, width, height - top, [&] {
    const float h = height - top;
    switch (mv.tab) {
      case 1: PlaylistsTab(kit, width, h); break;
      case 2: RulesTab(kit, width, h); break;
      case 3: StingersTab(kit, width, h); break;
      default: SegmentsTab(kit, width, h); break;
    }
  });
}

}  // namespace snd::tool::ui
