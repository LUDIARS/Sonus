// Event editor screen (design.md 4.2, 11.1 Event エディタ): the event list, the event's members,
// the time line of tracks and clips (drag a clip to move its start, up / down to another track;
// drag the background to scroll, wheel to zoom, double click to add the chosen asset there),
// the sequence kind, the selected clip's members (random widths, fades, weight, switch, layer,
// loop, synth) and the track's actions. Every edit goes through EventEditor (one undo step each).
#include <algorithm>
#include <cmath>
#include <cstdio>

#include "editor_util.hpp"
#include "snd/tool/event_editor.hpp"
#include "snd/tool/ui/app.hpp"
#include "snd/tool/ui/editor_draw.hpp"
#include "snd/tool/ui/kit.hpp"
#include "snd/tool/ui/layout_metrics.hpp"
#include "snd/tool/ui/views.hpp"

namespace snd::tool::ui {
namespace {

constexpr float kListW = 220.0f;
constexpr float kSideW = 300.0f;
constexpr float kRow = 26.0f;
constexpr float kLaneH = 44.0f;
constexpr float kLaneGap = 4.0f;
constexpr float kLabelW = 74.0f;

const char* const kSeq[] = {"polyphonic", "random", "randomNoRepeat", "sequential",
                            "shuffle",    "switch", "comboSequential", "layerCrossfade"};
const char* const kAct[] = {"play", "stop", "setParameter", "applySnapshot"};
const char* const kVirtual[] = {"continue", "restart", "never"};

std::vector<PickerItem> Names(const char* const* names, int n) {
  std::vector<PickerItem> v;
  for (int i = 0; i < n; ++i) v.push_back(PickerItem{names[i], std::to_string(i)});
  return v;
}

int ToInt(const std::string& s) { return s.empty() ? 0 : std::atoi(s.c_str()); }

std::string ClipKey(std::uint32_t t, std::uint32_t c) { return "t" + std::to_string(t) + ".c" + std::to_string(c); }

void EventMembers(Kit& kit, const std::string& g, float x, float y, float w) {
  App* app = &kit.app();
  ProjectDocument* d = app->document();
  EventEditor ed(*d, g);
  const json::Value& e = *ed.event();
  const float cw = (w - 8.0f) * 0.5f, x2 = x + cw + 8.0f;
  const std::string id = "ev.f";
  // row 0: sequence, bus
  kit.PickField(id + ".seq", "Sequence", SequenceName(ed.sequence()), "Sequence kind",
                [] { return Names(kSeq, 8); },
                [d, g](const std::string& v) { EventEditor(*d, g).SetSequence(static_cast<SequenceKind>(ToInt(v))); },
                x, y, cw);
  kit.PickField(id + ".bus", "Bus", RefName(*d, "bus", StringAt(e, "bus"), "master"), "Output bus",
                [d] { return DocItems(*d, "bus", "(master)"); },
                [d, g](const std::string& v) { EventEditor(*d, g).SetBus(v); }, x2, y, cw);
  y += kRow;
  kit.PickField(id + ".cat", "Category", RefName(*d, "category", StringAt(e, "category")), "Category",
                [d] { return DocItems(*d, "category", "(none)"); },
                [d, g](const std::string& v) { EventEditor(*d, g).SetCategory(v); }, x, y, cw);
  kit.PickField(id + ".limit", "Limit group", RefName(*d, "voiceLimitGroup", StringAt(e, "voiceLimitGroup")),
                "Voice limit group", [d] { return DocItems(*d, "voiceLimitGroup", "(none)"); },
                [d, g](const std::string& v) { EventEditor(*d, g).SetLimitGroup(v); }, x2, y, cw);
  y += kRow;
  struct N {
    EventNumber n;
    const char* label;
    double step, mn, mx;
    bool integer;
    const char* unit;
  };
  const SequenceUse use = UseOf(ed.sequence());
  const N nums[] = {{EventNumber::MaxInstances, "Max inst.", 1, 0, 1e6, true, ""},
                    {EventNumber::Priority, "Priority", 1, 0, 255, true, ""},
                    {EventNumber::RetriggerMs, "Retrigger", 10, 0, 1e9, false, "ms"},
                    {EventNumber::FadeOutMs, "Fade out", 10, 0, 1e9, false, "ms"},
                    {EventNumber::MaxDistance, "Max dist.", 1, 0, 1e9, false, "m"},
                    {EventNumber::ComboWindowMs, "Combo win.", 10, 0, 1e9, false, "ms"}};
  for (int i = 0; i < 6; ++i) {
    const N& n = nums[i];
    NumberSpec sp;
    sp.value = ed.NumberOf(n.n);
    sp.step = n.step;
    sp.minValue = n.mn;
    sp.maxValue = n.mx;
    sp.integer = n.integer;
    sp.unit = n.unit;
    sp.decimals = n.integer ? 0 : 2;
    sp.enabled = n.n != EventNumber::ComboWindowMs || use.comboWindowMs;
    const EventNumber which = n.n;
    kit.NumberField(id + ".n" + std::to_string(i), n.label, sp,
                    [d, g, which](double v, std::uint32_t key) { return EventEditor(*d, g).SetNumber(which, v, key); },
                    i % 2 == 0 ? x : x2, y, cw);
    if (i % 2 == 1) y += kRow;
  }
  const float tw = (cw - 8.0f) / 3.0f;
  const bool is3D = ed.Flag(EventFlag::Is3D);
  kit.Toggle(id + ".3d", "3D", is3D, [d, g, is3D] { EventEditor(*d, g).SetFlag(EventFlag::Is3D, !is3D); }, x, y, tw);
  const bool ip = ed.Flag(EventFlag::IgnorePause);
  kit.Toggle(id + ".ipause", "ign. pause", ip, [d, g, ip] { EventEditor(*d, g).SetFlag(EventFlag::IgnorePause, !ip); },
             x + tw + 4, y, tw);
  const bool st = ed.Flag(EventFlag::Stinger);
  kit.Toggle(id + ".stinger", "stinger", st, [d, g, st] { EventEditor(*d, g).SetFlag(EventFlag::Stinger, !st); },
             x + 2 * (tw + 4), y, tw);
  kit.PickField(id + ".virtual", "Virtual ret.", StringAt(e, "virtualReturn", "continue"), "Virtual return",
                [] { return Names(kVirtual, 3); },
                [d, g](const std::string& v) { EventEditor(*d, g).SetVirtualReturn(static_cast<VirtualReturn>(ToInt(v))); },
                x2, y, cw);
  y += kRow;
  if (is3D) {
    const double in = NumberAt(e, "cone.innerDeg", 360.0), out = NumberAt(e, "cone.outerDeg", 360.0),
                 odb = NumberAt(e, "cone.outerDb", 0.0);
    const float w3 = (w - 16.0f) / 3.0f;
    NumberSpec sp;
    sp.step = 5;
    sp.minValue = 0;
    sp.maxValue = 360;
    sp.decimals = 1;
    sp.unit = "deg";
    sp.value = in;
    kit.NumberField(id + ".cone.in", "Cone in", sp,
                    [d, g, out, odb](double v, std::uint32_t) { return EventEditor(*d, g).SetCone(v, out, odb); }, x, y,
                    w3, 60);
    sp.value = out;
    kit.NumberField(id + ".cone.out", "Cone out", sp,
                    [d, g, in, odb](double v, std::uint32_t) { return EventEditor(*d, g).SetCone(in, v, odb); },
                    x + w3 + 8, y, w3, 60);
    sp.value = odb;
    sp.unit = "dB";
    sp.step = 1;
    sp.minValue = -144;
    sp.maxValue = 0;
    kit.NumberField(id + ".cone.db", "Outer", sp,
                    [d, g, in, out](double v, std::uint32_t) { return EventEditor(*d, g).SetCone(in, out, v); },
                    x + 2 * (w3 + 8), y, w3, 60);
  }
}

// The time line: toolbar, ruler, lanes with clip hits.
float Timeline(Kit& kit, const std::string& g, float x, float y, float w, float h) {
  App* app = &kit.app();
  ProjectDocument* d = app->document();
  EventView& ev = app->view().event;
  EventEditor ed(*d, g);
  const snd::tool::Timeline tl = ed.ComputeTimeline();
  const std::uint32_t tracks = ed.TrackCount();
  if (ev.track >= tracks && tracks > 0u) ev.track = tracks - 1u;
  if (ev.clip >= 0 && static_cast<std::uint32_t>(ev.clip) >= ed.ClipCount(ev.track)) ev.clip = -1;
  // Toolbar.
  EventView* evp = &ev;
  kit.Action("ev.track.add", "+ Track", [d, g, evp] {
    EventEditor e(*d, g);
    if (e.AddTrack(e.TrackCount())) {
      evp->track = e.TrackCount() - 1u;
      evp->clip = -1;
    }
  }, x, y, 64);
  kit.Action("ev.track.remove", "- Track", [d, g, evp] {
    if (EventEditor(*d, g).RemoveTrack(evp->track)) {
      evp->clip = -1;
      if (evp->track > 0u) --evp->track;
    }
  }, x + 68, y, 64, 22, tracks > 0u);
  kit.Action("ev.track.up", "Up", [d, g, evp] {
    if (evp->track > 0u && EventEditor(*d, g).MoveTrack(evp->track, evp->track - 1u)) --evp->track;
  }, x + 136, y, 40, 22, ev.track > 0u);
  kit.Action("ev.track.down", "Down", [d, g, evp] {
    if (EventEditor(*d, g).MoveTrack(evp->track, evp->track + 1u)) ++evp->track;
  }, x + 180, y, 44, 22, ev.track + 1u < tracks);
  kit.PickField("ev.addasset", "", RefName(*d, "asset", ev.addAsset, "(asset)"), "Asset to add",
                [d] { return DocItems(*d, "asset"); }, [evp](const std::string& v) { evp->addAsset = v; }, x + 232, y,
                130);
  const double scroll = ev.scrollMs;
  kit.Action("ev.clip.add", "+ Clip", [d, g, evp, app, scroll] {
    if (evp->addAsset.empty()) return app->Message("choose an asset to add first");
    EventEditor e(*d, g);
    if (e.TrackCount() == 0u) e.AddTrack(0);
    if (e.AddAssetClip(evp->track, evp->addAsset, std::max(0.0, scroll))) {
      evp->clip = static_cast<std::int32_t>(e.ClipCount(evp->track)) - 1;
    }
  }, x + 366, y, 56, 22, tracks > 0u || !ev.addAsset.empty());
  kit.PickField("ev.clip.addevent", "", "+ Event clip", "Nested event", [d, g] {
    std::vector<PickerItem> v;
    for (const PickerItem& it : DocItems(*d, "event")) {
      if (it.value != g) v.push_back(it);
    }
    return v;
  }, [d, g, evp, app, scroll](const std::string& v) {
    EventEditor e(*d, g);
    if (e.TrackCount() == 0u) e.AddTrack(0);
    if (e.AddEventClip(evp->track, v, std::max(0.0, scroll))) {
      evp->clip = static_cast<std::int32_t>(e.ClipCount(evp->track)) - 1;
    } else {
      app->Message("refused: the nested event would make a cycle");
    }
  }, x + 426, y, 96);
  kit.Action("ev.clip.addsynth", "+ Synth", [d, g, evp, scroll] {
    EventEditor e(*d, g);
    if (e.TrackCount() == 0u) e.AddTrack(0);
    if (e.AddSynthClip(evp->track, SynthDesc{}, std::max(0.0, scroll))) {
      evp->clip = static_cast<std::int32_t>(e.ClipCount(evp->track)) - 1;
    }
  }, x + 526, y, 60);
  // Zoom and scroll.
  const float laneW = std::max(60.0f, w - kLabelW - 4.0f);
  kit.Action("ev.zoom.in", "+", [evp, laneW] {
    const TimeAxis a = TimeAxis{evp->scrollMs, evp->msPerPx}.Zoomed(0.5, laneW * 0.5f);
    evp->scrollMs = std::max(0.0, a.scrollMs);
    evp->msPerPx = a.msPerPx;
  }, x + w - 132, y, 24);
  kit.Action("ev.zoom.out", "-", [evp, laneW] {
    const TimeAxis a = TimeAxis{evp->scrollMs, evp->msPerPx}.Zoomed(2.0, laneW * 0.5f);
    evp->scrollMs = std::max(0.0, a.scrollMs);
    evp->msPerPx = a.msPerPx;
  }, x + w - 104, y, 24);
  const double length = std::max(100.0, tl.lengthMs);
  kit.Action("ev.zoom.fit", "Fit", [evp, laneW, length] {
    evp->scrollMs = 0.0;
    evp->msPerPx = std::clamp(length * 1.1 / laneW, kMinMsPerPx, kMaxMsPerPx);
  }, x + w - 76, y, 36);
  kit.Action("ev.scroll.left", "<", [evp, laneW] { evp->scrollMs = std::max(0.0, evp->scrollMs - laneW * 0.5 * evp->msPerPx); },
             x + w - 36, y, 16);
  kit.Action("ev.scroll.right", ">", [evp, laneW] { evp->scrollMs += laneW * 0.5 * evp->msPerPx; }, x + w - 18, y, 16);
  y += 26.0f;
  // Ruler and its labels.
  const TimeAxis axis{ev.scrollMs, ev.msPerPx};
  std::vector<double> ticks;
  Canvas(kit.doc(), "ev.ruler", DrawRuler(axis, laneW, 18.0f, kit.palette(), kit.ctx().dpi, &ticks), x + kLabelW + 4, y,
         laneW, 18);
  for (std::size_t i = 0; i < ticks.size() && i < 30u; ++i) {
    kit.Label("ev.ruler." + std::to_string(i), FormatNumberText(ticks[i], 1) + "ms", x + kLabelW + 4 + axis.X(ticks[i]) + 2,
              y - 2, 64, 14, 0.4f);
  }
  y += 20.0f;
  // Lanes (paged).
  const std::uint32_t perPage = static_cast<std::uint32_t>(std::max(1.0f, (h - 26.0f - 20.0f - 44.0f) / (kLaneH + kLaneGap)));
  std::uint32_t* top = &ev.trackTop;
  if (ev.track < *top) *top = ev.track;
  if (ev.track >= *top + perPage) *top = ev.track + 1u - perPage;
  if (*top + perPage > tracks) *top = tracks > perPage ? tracks - perPage : 0u;
  for (std::uint32_t k = 0; k < perPage && *top + k < tracks; ++k) {
    const std::uint32_t t = *top + k;
    const float ly = y + static_cast<float>(k) * (kLaneH + kLaneGap);
    const std::string tid = "ev.t" + std::to_string(t);
    const json::Value* tr = FindPath(*ed.event(), "tracks[" + std::to_string(t) + "]");
    const std::string tbus = tr != nullptr ? RefName(*d, "bus", StringAt(*tr, "bus"), "") : std::string();
    Button(kit.doc(), tid + ".select", (t == ev.track ? "> T" : "  T") + std::to_string(t + 1u) + (tbus.empty() ? "" : " " + Clip(tbus, 6)),
           [evp, t] {
             evp->track = t;
             evp->clip = -1;
           },
           x, ly, kLabelW, kLaneH, 0.5f);
    // Background: select the track, drag to scroll, wheel to zoom (Shift: scroll), double click adds.
    auto bg = std::make_shared<DragHandler>();
    auto anchor = std::make_shared<double>(0.0);
    bg->press = [evp, t, anchor](float, float, std::uint32_t) {
      if (evp->track != t) evp->clip = -1;
      evp->track = t;
      *anchor = evp->scrollMs;
    };
    bg->drag = [evp, anchor](const DragMotion& m) { evp->scrollMs = std::max(0.0, *anchor - m.dx * evp->msPerPx); };
    bg->wheel = [evp, laneW](float notches, std::uint32_t mods, std::uint32_t) {
      if ((mods & kModShift) != 0u) {
        evp->scrollMs = std::max(0.0, evp->scrollMs - notches * 40.0 * evp->msPerPx);
      } else {
        const TimeAxis a = TimeAxis{evp->scrollMs, evp->msPerPx}.Zoomed(std::pow(0.8, notches), laneW * 0.5f);
        evp->scrollMs = std::max(0.0, a.scrollMs);
        evp->msPerPx = a.msPerPx;
      }
    };
    bg->doubleClick = [d, g, evp, t, app](float lx, float) {
      if (evp->addAsset.empty()) return app->Message("choose an asset to add first");
      EventEditor e(*d, g);
      const double ms = std::max(0.0, TimeAxis{evp->scrollMs, evp->msPerPx}.Ms(lx));
      if (e.AddAssetClip(t, evp->addAsset, ms)) {
        evp->track = t;
        evp->clip = static_cast<std::int32_t>(e.ClipCount(t)) - 1;
        app->Edited();
      }
    };
    const float lx = x + kLabelW + 4.0f;
    kit.Hit(tid + ".lane.hit", bg, lx, ly, laneW, kLaneH);
    LaneLook look;
    look.dpi = kit.ctx().dpi;
    look.width = laneW;
    look.height = kLaneH;
    look.axis = axis;
    look.selected = t == ev.track;
    for (const TimelineClip& c : tl.clips) {
      if (c.track != t) continue;
      LaneClip lc{c.startMs, c.endMs, c.passMs, c.fadeInMs, c.fadeOutMs, c.infinite, c.lengthKnown,
                  t == ev.track && static_cast<std::int32_t>(c.clip) == ev.clip};
      look.clips.push_back(lc);
    }
    for (const TimelineAction& a : tl.actions) {
      if (a.track == t) look.actionsMs.push_back(a.timeMs);
    }
    for (const TimelineOverlap& o : tl.overlaps) {
      if (o.track == t) look.overlapsMs.emplace_back(o.fromMs, o.toMs);
    }
    // Clip hits (only the visible ones; the drawing above them).
    std::uint32_t ci = 0;
    for (const TimelineClip& c : tl.clips) {
      if (c.track != t) continue;
      const tela::Rect r = LaneClipRect(look, look.clips[ci++]);
      if (r.width <= 0.0f) continue;
      const std::uint32_t clip = c.clip;
      const double start = c.startMs;
      auto moved = std::make_shared<std::uint32_t>(t);
      const float laneStep = kLaneH + kLaneGap;
      auto handler = DocumentDrag(
          *app,
          [evp, t, clip](float, float, std::uint32_t) {
            evp->track = t;
            evp->clip = static_cast<std::int32_t>(clip);
          },
          [d, g, t, clip, start, evp](const DragMotion& m) {
            const double ms = std::max(0.0, start + static_cast<double>(m.dx) * evp->msPerPx);
            EventEditor(*d, g).MoveClip(t, clip, ms);
          },
          [d, g, t, clip, evp, laneStep, moved](bool wasMoved, float, float ly2) {
            if (!wasMoved) return;
            // Vertical: another track (rounded to lanes), at the clip's current start.
            const int delta = static_cast<int>(std::floor(ly2 / laneStep));
            EventEditor e(*d, g);
            const int to = static_cast<int>(t) + delta;
            if (delta == 0 || to < 0 || static_cast<std::uint32_t>(to) >= e.TrackCount()) return;
            const double ms = e.ClipNumberOf(t, clip, ClipNumber::StartMs);
            if (e.MoveClipToTrack(t, clip, static_cast<std::uint32_t>(to), ms)) {
              evp->track = static_cast<std::uint32_t>(to);
              evp->clip = static_cast<std::int32_t>(e.ClipCount(static_cast<std::uint32_t>(to))) - 1;
              *moved = static_cast<std::uint32_t>(to);
            }
          });
      kit.Hit(tid + ".c" + std::to_string(clip), handler, lx + r.x, ly + r.y, r.width, r.height);
    }
    Canvas(kit.doc(), tid + ".lane", DrawLane(look, kit.palette()), lx, ly, laneW, kLaneH);
  }
  const float below = y + static_cast<float>(std::min(perPage, tracks)) * (kLaneH + kLaneGap);
  if (tracks > perPage) {
    kit.Pager("ev.lanes", tracks, perPage, top, x, below, 200);
  }
  if (tracks == 0u) kit.Label("ev.notracks", "no tracks: + Track", x, y, w);
  char b[160];
  std::snprintf(b, sizeof b, "length %s%s  clips %u  overlaps %u%s", tl.infinite ? "infinite / " : "",
                (FormatNumberText(tl.lengthMs, 1) + " ms").c_str(), static_cast<unsigned>(tl.clips.size()),
                static_cast<unsigned>(tl.overlaps.size()), tl.oneClipPerPlay ? "  (one clip per play)" : "");
  kit.Label("ev.info", b, x, below + 24.0f, w, 20, 0.55f);
  const std::vector<std::string> unused = ed.UnusedMembers();
  if (!unused.empty()) {
    std::string u = "not used by " + std::string(SequenceName(ed.sequence())) + ":";
    for (std::size_t i = 0; i < unused.size() && i < 6u; ++i) u += " " + unused[i];
    if (unused.size() > 6u) u += " +" + std::to_string(unused.size() - 6u);
    kit.Label("ev.unused", u, x, below + 44.0f, w, 20, 0.5f);
  }
  return below + 64.0f;
}

void ClipPanel(Kit& kit, const std::string& g, float x, float y, float w, float h) {
  App* app = &kit.app();
  ProjectDocument* d = app->document();
  EventView& ev = app->view().event;
  EventView* evp = &ev;
  EventEditor ed(*d, g);
  const float bottom = y + h;
  if (ev.clip < 0 || ed.TrackCount() == 0u) {
    kit.Label("ev.clip.none", "select a clip in the time line", x, y, w);
    y += 24.0f;
  } else {
    const std::uint32_t t = ev.track, c = static_cast<std::uint32_t>(ev.clip);
    const std::string base = "ev.clip." + ClipKey(t, c);
    const json::Value* cl = FindPath(*ed.event(), "tracks[" + std::to_string(t) + "].clips[" + std::to_string(c) + "]");
    std::string src = "-";
    if (cl != nullptr) {
      if (!StringAt(*cl, "asset").empty()) src = "asset " + RefName(*d, "asset", StringAt(*cl, "asset"));
      else if (!StringAt(*cl, "event").empty()) src = "event " + RefName(*d, "event", StringAt(*cl, "event"));
      else if (cl->Find("synth") != nullptr) src = "synth " + StringAt(*cl, "synth.kind", "oscillator");
    }
    kit.Title(base + ".title", "Clip T" + std::to_string(t + 1u) + " / " + std::to_string(c + 1u) + ": " + src, x, y, w - 70);
    kit.Action(base + ".remove", "Remove", [d, g, t, c, evp] {
      if (EventEditor(*d, g).RemoveClip(t, c)) evp->clip = -1;
    }, x + w - 66, y, 64);
    y += 26.0f;
    const SequenceUse use = UseOf(ed.sequence());
    struct F {
      ClipNumber n;
      const char* label;
      double step, mn, mx;
      bool integer;
      const char* unit;
      bool enabled;
    };
    const F fs[] = {{ClipNumber::StartMs, "Start", 10, 0, 1e9, false, "ms", true},
                    {ClipNumber::VolumeDb, "Volume", 0.5, -144, 24, false, "dB", true},
                    {ClipNumber::PitchCents, "Pitch", 10, -4800, 4800, false, "ct", true},
                    {ClipNumber::FadeInMs, "Fade in", 5, 0, 1e9, false, "ms", true},
                    {ClipNumber::FadeOutMs, "Fade out", 5, 0, 1e9, false, "ms", true},
                    {ClipNumber::LoopCount, "Loops (0=inf)", 1, 0, 1e6, true, "", true},
                    {ClipNumber::Weight, "Weight", 0.1, 0, 1e6, false, "", use.weight}};
    for (int i = 0; i < 7; ++i) {
      NumberSpec sp;
      sp.value = ed.ClipNumberOf(t, c, fs[i].n);
      sp.step = fs[i].step;
      sp.minValue = fs[i].mn;
      sp.maxValue = fs[i].mx;
      sp.integer = fs[i].integer;
      sp.unit = fs[i].unit;
      sp.decimals = fs[i].integer ? 0 : 2;
      sp.enabled = fs[i].enabled;
      const ClipNumber n = fs[i].n;
      kit.NumberField(base + ".n" + std::to_string(i), fs[i].label, sp,
                      [d, g, t, c, n](double v, std::uint32_t key) { return EventEditor(*d, g).SetClipNumber(t, c, n, v, key); },
                      x, y, w, 96);
      y += kRow;
    }
    kit.Label(base + ".rand", "random widths (+-)", x, y, w, 20, 0.55f);
    y += 20.0f;
    struct R {
      RandomField f;
      const char* label;
      double step;
      const char* unit;
    };
    const R rs[] = {{RandomField::VolumeDb, "Volume", 0.5, "dB"}, {RandomField::PitchCents, "Pitch", 10, "ct"},
                    {RandomField::Pan, "Pan", 0.05, ""},          {RandomField::StartMs, "Start", 5, "ms"},
                    {RandomField::FilterHz, "Filter", 100, "Hz"}};
    for (int i = 0; i < 5; ++i) {
      NumberSpec sp;
      sp.value = ed.ClipRandom(t, c, rs[i].f);
      sp.step = rs[i].step;
      sp.minValue = 0;
      sp.unit = rs[i].unit;
      sp.decimals = 2;
      const RandomField f = rs[i].f;
      kit.NumberField(base + ".r" + std::to_string(i), rs[i].label, sp,
                      [d, g, t, c, f](double v, std::uint32_t key) { return EventEditor(*d, g).SetClipRandom(t, c, f, v, key); },
                      x + 12, y, w - 12, 84);
      y += kRow;
    }
    if (use.switchValue && cl != nullptr) {
      const json::Value* sv = cl->Find("switchValue");
      const std::string text = sv == nullptr ? "-" : sv->IsString() ? sv->text : sv->text;
      kit.NameField(base + ".switch", "Switch", text, [d, g, t, c](const std::string& s) {
        double v = 0.0;
        if (ParseNumber(s, &v) && v >= 0.0 && v == std::floor(v) && v < 4294967296.0) {
          return EventEditor(*d, g).SetSwitchValue(t, c, static_cast<std::uint32_t>(v));
        }
        return EventEditor(*d, g).SetSwitchValue(t, c, s);
      }, x, y, w, 96);
      y += kRow;
    }
    if (use.layer && cl != nullptr) {
      const std::string lp = "layer";
      const double lo = NumberAt(*cl, "layer[0]", 0.0), hi = NumberAt(*cl, "layer[1]", 1.0);
      NumberSpec sp;
      sp.step = 0.05;
      sp.decimals = 3;
      sp.value = lo;
      kit.NumberField(base + ".layer.min", "Layer min", sp,
                      [d, g, t, c, hi](double v, std::uint32_t) { return EventEditor(*d, g).SetLayer(t, c, v, hi); }, x, y,
                      w, 96);
      y += kRow;
      sp.value = hi;
      kit.NumberField(base + ".layer.max", "Layer max", sp,
                      [d, g, t, c, lo](double v, std::uint32_t) { return EventEditor(*d, g).SetLayer(t, c, lo, v); }, x, y,
                      w, 96);
      y += kRow;
    }
    if (cl != nullptr && cl->Find("synth") != nullptr && y + 4 * kRow < bottom) {
      SynthDesc s;
      s.kind = StringAt(*cl, "synth.kind", "oscillator");
      s.waveform = StringAt(*cl, "synth.waveform", "sine");
      s.frequencyHz = NumberAt(*cl, "synth.frequencyHz", 440.0);
      s.duty = NumberAt(*cl, "synth.duty", 0.5);
      s.noiseColor = StringAt(*cl, "synth.noiseColor", "white");
      s.asset = RefGuid(*d, "asset", StringAt(*cl, "synth.asset"));
      s.patch = RefGuid(*d, "patch", StringAt(*cl, "synth.patch"));
      s.channels = static_cast<std::uint32_t>(NumberAt(*cl, "synth.channels", 1.0));
      static const char* const kKinds[] = {"oscillator", "noise", "granular", "patch", "wavetable"};
      static const char* const kWaves[] = {"sine", "saw", "square", "triangle", "pulse"};
      kit.PickField(base + ".synth.kind", "Synth", s.kind, "Synth kind", [] {
        std::vector<PickerItem> v;
        for (const char* k : kKinds) v.push_back(PickerItem{k, k});
        return v;
      }, [d, g, t, c, s](const std::string& v) {
        SynthDesc n = s;
        n.kind = v;
        EventEditor(*d, g).SetSynth(t, c, n);
      }, x, y, w, 96);
      y += kRow;
      if (s.kind == "oscillator") {
        kit.PickField(base + ".synth.wave", "Waveform", s.waveform, "Waveform", [] {
          std::vector<PickerItem> v;
          for (const char* k : kWaves) v.push_back(PickerItem{k, k});
          return v;
        }, [d, g, t, c, s](const std::string& v) {
          SynthDesc n = s;
          n.waveform = v;
          EventEditor(*d, g).SetSynth(t, c, n);
        }, x, y, w, 96);
        y += kRow;
        NumberSpec sp;
        sp.value = s.frequencyHz;
        sp.step = 10;
        sp.minValue = 0.01;
        sp.maxValue = 24000;
        sp.unit = "Hz";
        sp.decimals = 2;
        kit.NumberField(base + ".synth.freq", "Frequency", sp, [d, g, t, c, s](double v, std::uint32_t) {
          SynthDesc n = s;
          n.frequencyHz = v;
          return EventEditor(*d, g).SetSynth(t, c, n);
        }, x, y, w, 96);
        y += kRow;
      } else if (s.kind == "patch") {
        kit.PickField(base + ".synth.patch", "Patch", RefName(*d, "patch", s.patch), "Patch",
                      [d] { return DocItems(*d, "patch"); }, [d, g, t, c, s](const std::string& v) {
                        SynthDesc n = s;
                        n.patch = v;
                        EventEditor(*d, g).SetSynth(t, c, n);
                      }, x, y, w, 96);
        y += kRow;
      } else if (s.kind == "granular" || s.kind == "wavetable") {
        kit.PickField(base + ".synth.asset", "Source", RefName(*d, "asset", s.asset), "Asset",
                      [d] { return DocItems(*d, "asset"); }, [d, g, t, c, s](const std::string& v) {
                        SynthDesc n = s;
                        n.asset = v;
                        EventEditor(*d, g).SetSynth(t, c, n);
                      }, x, y, w, 96);
        y += kRow;
      }
    }
  }
  // The selected track's bus and its actions.
  if (ed.TrackCount() == 0u || y + 3 * kRow > bottom) return;
  const std::uint32_t t = ev.track;
  const std::string tb = "ev.track." + std::to_string(t);
  const json::Value* tr = FindPath(*ed.event(), "tracks[" + std::to_string(t) + "]");
  y += 6.0f;
  kit.Title(tb + ".title", "Track " + std::to_string(t + 1u), x, y, w);
  y += 24.0f;
  kit.PickField(tb + ".bus", "Bus", RefName(*d, "bus", tr != nullptr ? StringAt(*tr, "bus") : "", "(event's)"),
                "Track bus", [d] { return DocItems(*d, "bus", "(event's)"); },
                [d, g, t](const std::string& v) { EventEditor(*d, g).SetTrackBus(t, v); }, x, y, w, 96);
  y += kRow;
  if (UseOf(ed.sequence()).layer) {
    kit.PickField(tb + ".layerpar", "Layer param",
                  RefName(*d, "parameter", tr != nullptr ? StringAt(*tr, "layerParameter") : ""), "Layer parameter",
                  [d] { return DocItems(*d, "parameter", "(none)"); },
                  [d, g, t](const std::string& v) { EventEditor(*d, g).SetTrackLayerParameter(t, v); }, x, y, w, 96);
    y += kRow;
  }
  const std::uint32_t na = ed.ActionCount(t);
  kit.Label(tb + ".actions", "actions (" + std::to_string(na) + ")", x, y, w - 70, 22, 0.6f);
  kit.Action(tb + ".action.add", "+ Action", [d, g, t, evp] {
    ActionDesc a;
    a.action = ActionKind::Stop;
    a.target = g;
    if (EventEditor(*d, g).AddAction(t, a)) evp->action = static_cast<std::int32_t>(EventEditor(*d, g).ActionCount(t)) - 1;
  }, x + w - 66, y, 64);
  y += 24.0f;
  const std::uint32_t perPage = 4;
  const std::uint32_t first = kit.Pager(tb + ".apager", na, perPage, &ev.actionPage, x, y, w);
  if (na > perPage) y += 22.0f;
  for (std::uint32_t i = first; i < na && i < first + perPage; ++i) {
    const ActionDesc a = ed.Action(t, i);
    const char* kind = a.action == ActionKind::SetParameter ? "parameter" : a.action == ActionKind::ApplySnapshot ? "snapshot" : "event";
    std::string label = FormatNumberText(a.timeMs, 1) + " ms " + kAct[static_cast<int>(a.action)] + " " +
                        RefName(*d, kind, a.target);
    if (a.action == ActionKind::SetParameter || a.action == ActionKind::ApplySnapshot) {
      label += " = " + FormatNumberText(a.value, 3);
    }
    const std::int32_t ai = static_cast<std::int32_t>(i);
    Button(kit.doc(), tb + ".a" + std::to_string(i), (ev.action == ai ? "> " : "  ") + Clip(label, 44),
           [evp, ai] { evp->action = ai; }, x, y, w, 22, 0.5f);
    y += 24.0f;
  }
  if (ev.action < 0 || static_cast<std::uint32_t>(ev.action) >= na || y + 4 * kRow > bottom) return;
  const std::uint32_t ai = static_cast<std::uint32_t>(ev.action);
  const ActionDesc a = ed.Action(t, ai);
  const std::string ab = tb + ".action";
  NumberSpec sp;
  sp.value = a.timeMs;
  sp.step = 10;
  sp.minValue = 0;
  sp.unit = "ms";
  sp.decimals = 1;
  kit.NumberField(ab + ".time", "Time", sp, [d, g, t, ai, a](double v, std::uint32_t) {
    ActionDesc n = a;
    n.timeMs = v;
    return EventEditor(*d, g).SetAction(t, ai, n);
  }, x, y, w - 70, 60);
  kit.Action(ab + ".remove", "Remove", [d, g, t, ai, evp] {
    if (EventEditor(*d, g).RemoveAction(t, ai)) evp->action = -1;
  }, x + w - 66, y, 64);
  y += kRow;
  kit.PickField(ab + ".kind", "Action", kAct[static_cast<int>(a.action)], "Action", [] { return Names(kAct, 4); },
                [d, g, t, ai, a](const std::string& v) {
                  ActionDesc n = a;
                  n.action = static_cast<ActionKind>(ToInt(v));
                  if (n.action != a.action) n.target.clear();
                  EventEditor e(*d, g);
                  if (n.target.empty()) {  // a target of the new kind: the first one
                    const char* k = n.action == ActionKind::SetParameter ? "parameter"
                                    : n.action == ActionKind::ApplySnapshot ? "snapshot" : "event";
                    const auto all = d->OfKind(k);
                    if (!all.empty()) n.target = all.front()->guid;
                  }
                  e.SetAction(t, ai, n);
                }, x, y, w, 60);
  y += kRow;
  const char* tk = a.action == ActionKind::SetParameter ? "parameter" : a.action == ActionKind::ApplySnapshot ? "snapshot" : "event";
  kit.PickField(ab + ".target", "Target", RefName(*d, tk, a.target), "Target", [d, tk] { return DocItems(*d, tk); },
                [d, g, t, ai, a](const std::string& v) {
                  ActionDesc n = a;
                  n.target = v;
                  EventEditor(*d, g).SetAction(t, ai, n);
                }, x, y, w, 60);
  y += kRow;
  if (a.action == ActionKind::SetParameter || a.action == ActionKind::ApplySnapshot) {
    NumberSpec vs;
    vs.value = a.value;
    vs.step = 0.05;
    vs.decimals = 3;
    kit.NumberField(ab + ".value", "Value", vs, [d, g, t, ai, a](double v, std::uint32_t) {
      ActionDesc n = a;
      n.value = v;
      return EventEditor(*d, g).SetAction(t, ai, n);
    }, x, y, w, 60);
  }
}

}  // namespace

void DeclareEvent(tela::Document& doc, ViewContext& ctx, float width, float height) {
  Kit kit(doc, ctx);
  if (!NeedDocument(kit, "ev.nodoc", width)) return;
  App* app = ctx.app;
  ProjectDocument* d = app->document();
  EventView& ev = app->view().event;
  // Music events have their own screen; the list shows every event (music ones marked).
  ObjectListSpec ls;
  ls.id = "ev.list";
  ls.kind = "event";
  ls.title = "Events";
  ls.selected = &ev.event;
  ls.page = &ev.listPage;
  ls.create = [d] {
    const std::string g = d->Create("event", NewName(*d, "event", "Event"));
    if (!g.empty()) {
      const std::string bank = FirstBank(*d);
      if (!bank.empty()) d->Set(g, "bank", String(bank));
      EventEditor(*d, g).AddTrack(0);
    }
    return g;
  };
  ls.label = [](const DocObject& o) { return o.doc.Find("music") != nullptr ? o.name() + " (music)" : o.name(); };
  const std::string before = ev.event;
  Panel(doc, "ev.listpanel", 0, 0, kListW, height, [&] { ObjectList(kit, ls, 0, 0, kListW - 12, height - 12); }, 6.0f);
  if (ev.event != before) {
    ev.clip = -1;
    ev.track = 0;
    ev.action = -1;
    ev.trackTop = 0;
  }
  if (ev.event.empty()) {
    kit.Label("ev.empty", "no events: New", kListW + 8, 0, 300);
    return;
  }
  if (ev.addAsset.empty() || d->Find(ev.addAsset) == nullptr) {
    const auto assets = d->OfKind("asset");
    ev.addAsset = assets.empty() ? std::string() : assets.front()->guid;
  }
  const std::string g = ev.event;
  const float mx = kListW + 8.0f, mw = std::max(300.0f, width - kListW - kSideW - 16.0f);
  Panel(doc, "ev.main", mx, 0, mw, height, [&] {
    EventMembers(kit, g, 0, 0, mw - 12);
    const float top = 7.0f * kRow + 4.0f;
    Timeline(kit, g, 0, top, mw - 12, height - top - 12.0f);
  }, 6.0f);
  Panel(doc, "ev.side", width - kSideW, 0, kSideW, height, [&] { ClipPanel(kit, g, 0, 0, kSideW - 12, height - 12); },
        6.0f);
}

}  // namespace snd::tool::ui
