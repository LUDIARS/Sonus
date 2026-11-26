// Checking, building and diffing the document (doc_build.hpp).
#include "snd/tool/doc_build.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <map>
#include <mutex>
#include <set>

#include "snd/buildtool/patch.hpp"
#include "snd/format/bank_check.hpp"
#include "snd/format/bank_format.hpp"

namespace snd::tool {
namespace {

namespace fs = std::filesystem;
namespace bf = ::snd::bank_format;
namespace ch = bf::chunk;
namespace co = bf::col;
using json::Value;

// --- issue -> member path ----------------------------------------------------------------
bool PathAt(const Value& v, std::uint32_t line, std::uint32_t column, const std::string& path, std::string* out) {
  if (v.line == line && v.column == column) {
    *out = path;
    // A deeper value at the same position (none: containers start with their bracket).
  }
  if (v.IsArray()) {
    for (std::size_t i = 0; i < v.items.size(); ++i) {
      if (PathAt(v.items[i], line, column, path + "[" + std::to_string(i) + "]", out)) return true;
    }
  } else if (v.IsObject()) {
    for (const json::Member& m : v.members) {
      if (PathAt(m.second, line, column, path.empty() ? m.first : path + "." + m.first, out)) return true;
    }
  }
  return v.line == line && v.column == column;
}

// --- live addressing (core/src/live_link.cpp ResolveRow, reversed) ------------------------
struct Owner {
  std::uint32_t table;
  std::uint32_t parent;
  std::uint32_t firstCol;
  std::uint32_t countCol;
};
constexpr Owner kOwners[] = {
    {ch::TRAK, ch::EVNT, co::EVNT_trackFirst, co::EVNT_trackCount},
    {ch::SEND, ch::BUSS, co::BUSS_sendFirst, co::BUSS_sendCount},
    {ch::EFFX, ch::BUSS, co::BUSS_effectFirst, co::BUSS_effectCount},
    {ch::SNPV, ch::SNAP, co::SNAP_valueFirst, co::SNAP_valueCount},
    {ch::MPLE, ch::MPLS, co::MPLS_entryFirst, co::MPLS_entryCount},
    {ch::CURV, ch::EVNT, co::EVNT_curveFirst, co::EVNT_curveCount},
};
constexpr Owner kOwners2[] = {
    {ch::CLIP, ch::TRAK, co::TRAK_clipFirst, co::TRAK_clipCount},
    {ch::ACTN, ch::TRAK, co::TRAK_actionFirst, co::TRAK_actionCount},
    {ch::EFPV, ch::EFFX, co::EFFX_paramFirst, co::EFFX_paramCount},
    {ch::CPNT, ch::CURV, co::CURV_pointFirst, co::CURV_pointCount},
};
const Owner* FindOwner(const Owner* list, std::size_t n, std::uint32_t table) {
  for (std::size_t i = 0; i < n; ++i) {
    if (list[i].table == table) return &list[i];
  }
  return nullptr;
}

// Live-writable u32 columns (core CheckEdit kU32Rules); every f32 column is writable.
struct U32Rule {
  std::uint32_t chunk;
  std::uint32_t column;
};
constexpr U32Rule kU32Live[] = {
    {ch::EVNT, co::EVNT_maxInstances}, {ch::EVNT, co::EVNT_virtualReturn}, {ch::TRAK, co::TRAK_layerParam},
    {ch::CLIP, co::CLIP_switchValue},  {ch::CLIP, co::CLIP_loopCount},     {ch::ACTN, co::ACTN_target},
    {ch::SEND, co::SEND_preFader},     {ch::CURV, co::CURV_input},         {ch::PARM, co::PARM_scopeMask},
    {ch::VLGR, co::VLGR_maxVoices},    {ch::VLGR, co::VLGR_stealMode},     {ch::MPLS, co::MPLS_mode},
    {ch::MPLS, co::MPLS_loopCount},    {ch::MPLE, co::MPLE_repeat},        {ch::MSTG, co::MSTG_overlap},
};
constexpr U32Rule kIdColumns[] = {
    {ch::TRAK, co::TRAK_layerParam}, {ch::CLIP, co::CLIP_switchValue}, {ch::ACTN, co::ACTN_target},
    {ch::CURV, co::CURV_input},
};

bool HasIdColumn(std::uint32_t fourcc) {
  const bf::ColumnInfo* info = bf::check::FindColumnInfo(fourcc, 0);
  if (info == nullptr || info->type != bf::ColumnType::U32) return false;
  const std::size_t len = std::strlen(info->name);
  return len >= 3 && std::strcmp(info->name + len - 3, ".id") == 0;
}

std::uint32_t U32At(const ParsedBank& v, std::uint32_t fourcc, std::uint32_t column, std::uint32_t row) {
  return v.U32(fourcc, column, row);
}

// Row of `ownerChunk` whose [first, first + count) holds `row`, kNone if none.
std::uint32_t OwnerRow(const ParsedBank& v, const Owner& o, std::uint32_t row, std::uint32_t* index) {
  const std::uint32_t rows = v.Rows(o.parent);
  for (std::uint32_t r = 0; r < rows; ++r) {
    const std::uint32_t first = U32At(v, o.parent, o.firstCol, r);
    const std::uint32_t count = U32At(v, o.parent, o.countCol, r);
    if (first != bf::kNone && row >= first && row - first < count) {
      *index = row - first;
      return r;
    }
  }
  return bf::kNone;
}

void Address(const ParsedBank& v, CellRef* c) {
  c->addressable = false;
  std::uint32_t idRow = bf::kNone;
  std::uint32_t idChunk = 0;
  if (HasIdColumn(c->chunk)) {
    idRow = c->row;
    idChunk = c->chunk;
    c->index = 0;
  } else if (const Owner* o = FindOwner(kOwners, std::size(kOwners), c->chunk)) {
    std::uint32_t idx = 0;
    idRow = OwnerRow(v, *o, c->row, &idx);
    idChunk = o->parent;
    c->index = idx;
  } else if (const Owner* o2 = FindOwner(kOwners2, std::size(kOwners2), c->chunk)) {
    const Owner* o1 = FindOwner(kOwners, std::size(kOwners), o2->parent);
    std::uint32_t lo = 0, hi = 0;
    const std::uint32_t mid = OwnerRow(v, *o2, c->row, &lo);
    if (o1 != nullptr && mid != bf::kNone && lo <= 0xFFFFu) {
      idRow = OwnerRow(v, *o1, mid, &hi);
      idChunk = o1->parent;
      c->index = (hi << 16) | lo;
      if (hi > 0xFFFFu) idRow = bf::kNone;
    }
  } else if (c->chunk == ch::DUCK) {
    const std::uint32_t bus = U32At(v, ch::DUCK, co::DUCK_targetBus, c->row);
    if (bus != bf::kNone && bus < v.Rows(ch::BUSS)) {
      std::uint32_t n = 0;
      for (std::uint32_t r = 0; r < c->row; ++r) n += U32At(v, ch::DUCK, co::DUCK_targetBus, r) == bus ? 1u : 0u;
      idRow = bus;
      idChunk = ch::BUSS;
      c->index = n;
    }
  }
  if (idRow == bf::kNone || idRow >= v.Rows(idChunk)) return;
  c->runtimeId = U32At(v, idChunk, 0, idRow);
  c->addressable = true;
  std::uint64_t lo = 0, hi = 0;
  if (v.Guid(c->runtimeId, &lo, &hi)) c->guid = Guid{lo, hi};
}

void Classify(const ParsedBank& v, CellRef* c) {
  const bf::ColumnInfo* info = bf::check::FindColumnInfo(c->chunk, c->column);
  c->liveWritable = false;
  if (info == nullptr) return;
  switch (info->type) {
    case bf::ColumnType::F32:
      c->type = CellType::F32;
      c->liveWritable = true;
      break;
    case bf::ColumnType::U32:
      c->type = CellType::U32;
      for (const U32Rule& r : kIdColumns) {
        if (r.chunk == c->chunk && r.column == c->column) c->type = CellType::Id;
      }
      for (const U32Rule& r : kU32Live) {
        if (r.chunk == c->chunk && r.column == c->column) c->liveWritable = true;
      }
      break;
    default:
      c->type = CellType::U64;
      break;
  }
  // D-178c: a column the build left out of the table cannot be written live (NOT_FOUND).
  c->liveWritable = c->liveWritable && c->addressable && v.HasColumn(c->chunk, c->column);
}

// --- field table --------------------------------------------------------------------------
std::vector<FieldColumn> MakeFields() {
  using E = FieldEffect;
  using R = FieldRow;
  std::vector<FieldColumn> f;
  auto C = [&](const char* k, const char* p, std::uint32_t chunk, std::uint16_t col, R r, std::uint32_t aux = 0) {
    f.push_back(FieldColumn{k, p, E::Column, chunk, col, r, aux});
  };
  auto N = [&](const char* k, const char* p) { f.push_back(FieldColumn{k, p, E::NotInBank, 0, 0, R::None, 0}); };
  auto S = [&](const char* k, const char* p) { f.push_back(FieldColumn{k, p, E::Structure, 0, 0, R::None, 0}); };
  // Every kind.
  for (const char* p : {"type", "guid", "name", "aliases", "aliases[#]", "bank"}) S("*", p);
  // project, bank
  for (const char* p : {"formatVersion", "languages", "languages[#]", "platforms", "platforms.**"}) S("project", p);
  for (const char* p : {"dependsOn", "dependsOn[#]", "language"}) S("bank", p);
  // bus
  for (const char* p : {"parent", "master", "sends", "sends[#]", "sends[#].bus", "effects", "effects[#]", "effects[#].type",
                        "effects[#].params", "effects[#].blob"}) {
    S("bus", p);
  }
  for (const char* p : {"output", "lodBypassable", "meter"}) C("bus", p, ch::BUSS, co::BUSS_flags, R::Id);
  C("bus", "volumeDb", ch::BUSS, co::BUSS_volumeDb, R::Id);
  C("bus", "channels", ch::BUSS, co::BUSS_channels, R::Id);
  C("bus", "lodPriority", ch::BUSS, co::BUSS_lodPriority, R::Id);
  C("bus", "sends[#].levelDb", ch::SEND, co::SEND_levelDb, R::Owner1);
  C("bus", "sends[#].preFader", ch::SEND, co::SEND_preFader, R::Owner1);
  C("bus", "effects[#].params[#]", ch::EFPV, co::EFPV_value, R::Owner2);
  C("bus", "effects[#].flags", ch::EFFX, co::EFFX_flags, R::Owner1);
  // category, parameter
  S("category", "parent");
  for (const char* p : {"ignorePause", "muteOnFocusLoss"}) C("category", p, ch::CATG, co::CATG_flags, R::Id);
  C("category", "volumeDb", ch::CATG, co::CATG_volumeDb, R::Id);
  C("parameter", "kind", ch::PARM, co::PARM_kind, R::Id);
  C("parameter", "scope", ch::PARM, co::PARM_scopeMask, R::Id);
  C("parameter", "scope[#]", ch::PARM, co::PARM_scopeMask, R::Id);
  C("parameter", "min", ch::PARM, co::PARM_minValue, R::Id);
  C("parameter", "max", ch::PARM, co::PARM_maxValue, R::Id);
  C("parameter", "default", ch::PARM, co::PARM_defaultValue, R::Id);
  C("parameter", "rampRate", ch::PARM, co::PARM_rampRate, R::Id);
  // snapshot, voiceLimitGroup, ducking
  for (const char* p : {"values", "values[#]", "values[#].kind", "values[#].bus", "values[#].effect", "values[#].send"}) S("snapshot", p);
  C("snapshot", "values[#].value", ch::SNPV, co::SNPV_value, R::Owner1);
  C("snapshot", "values[#].param", ch::SNPV, co::SNPV_paramIndex, R::Owner1);
  C("voiceLimitGroup", "maxVoices", ch::VLGR, co::VLGR_maxVoices, R::Id);
  C("voiceLimitGroup", "steal", ch::VLGR, co::VLGR_stealMode, R::Id);
  for (const char* p : {"kind", "source", "target"}) S("ducking", p);
  C("ducking", "amountDb", ch::DUCK, co::DUCK_amountDb, R::Duck);
  C("ducking", "attackMs", ch::DUCK, co::DUCK_attackMs, R::Duck);
  C("ducking", "releaseMs", ch::DUCK, co::DUCK_releaseMs, R::Duck);
  C("ducking", "thresholdDb", ch::DUCK, co::DUCK_thresholdDb, R::Duck);
  // asset: the material (WAVE / streams) is rebuilt; only the flags are cells.
  for (const char* p : {"file", "languages", "languages.**", "loadForm", "loop", "loop.**", "convert", "convert.**", "preloadMs",
                        "seekIntervalMs"}) {
    S("asset", p);
  }
  for (const char* p : {"stallPolicy", "virtualReturn", "ioPriority"}) C("asset", p, ch::ASST, co::ASST_flags, R::Id);
  // event
  for (const char* p : {"bus", "category", "limitGroup", "cone", "distanceCurves", "curves", "curves[#]", "curves[#].points",
                        "curves[#].points[#]", "tracks", "tracks[#]", "tracks[#].bus", "tracks[#].clips", "tracks[#].actions",
                        "tracks[#].clips[#]", "tracks[#].clips[#].asset", "tracks[#].clips[#].event",
                        "tracks[#].clips[#].synth", "tracks[#].clips[#].synth.kind", "tracks[#].clips[#].synth.asset",
                        "tracks[#].clips[#].synth.patch", "tracks[#].clips[#].random", "tracks[#].actions[#]", "music",
                        "music.initialState", "music.states", "music.states[#]", "music.states[#].name",
                        "music.states[#].playlist", "music.transitions", "music.transitions[#]", "music.transitions[#].bridge",
                        "music.stingers", "music.stingers[#]", "music.stingers[#].name", "music.stingers[#].event"}) {
    S("event", p);
  }
  C("event", "sequence", ch::EVNT, co::EVNT_sequenceKind, R::Id);
  C("event", "priority", ch::EVNT, co::EVNT_priority, R::Id);
  C("event", "maxInstances", ch::EVNT, co::EVNT_maxInstances, R::Id);
  C("event", "retriggerMs", ch::EVNT, co::EVNT_retriggerMs, R::Id);
  C("event", "fadeOutMs", ch::EVNT, co::EVNT_fadeOutMs, R::Id);
  C("event", "comboWindowMs", ch::EVNT, co::EVNT_comboWindowMs, R::Id);
  C("event", "maxDistance", ch::EVNT, co::EVNT_maxDistance, R::Id);
  C("event", "cone.innerDeg", ch::EVNT, co::EVNT_coneInnerDeg, R::Id);
  C("event", "cone.outerDeg", ch::EVNT, co::EVNT_coneOuterDeg, R::Id);
  C("event", "cone.outerDb", ch::EVNT, co::EVNT_coneOuterDb, R::Id);
  C("event", "virtualReturn", ch::EVNT, co::EVNT_virtualReturn, R::Id);
  for (const char* p : {"is3D", "ignorePause", "stinger"}) C("event", p, ch::EVNT, co::EVNT_flags, R::Id);
  struct Dist {
    const char* key;
    std::uint16_t col;
  };
  static const Dist kDist[] = {{"volume", co::EVNT_curveVolume},       {"lowpass", co::EVNT_curveLowpass},
                               {"spread", co::EVNT_curveSpread},       {"reverbSend", co::EVNT_curveReverbSend},
                               {"blend3D", co::EVNT_curveBlend3D}};
  static std::vector<std::string> names;  // storage for the generated patterns
  if (names.empty()) {
    for (const Dist& d : kDist) {
      const std::string b = std::string("distanceCurves.") + d.key;
      for (const char* tail : {"", ".parameter", ".target", ".compose", ".points", ".points[#]", ".points[#][0]",
                               ".points[#][1]", ".points[#][2]"}) {
        names.push_back(b + tail);
      }
    }
  }
  for (std::size_t k = 0; k < 5; ++k) {
    const std::string* n = &names[k * 9];
    S("event", n[0].c_str());
    N("event", n[1].c_str());  // a distance curve's input is the distance and its target its key
    N("event", n[2].c_str());
    C("event", n[3].c_str(), ch::CURV, co::CURV_compose, R::DistanceCurve, kDist[k].col);
    S("event", n[4].c_str());
    S("event", n[5].c_str());
    C("event", n[6].c_str(), ch::CPNT, co::CPNT_x, R::DistancePoint, kDist[k].col);
    C("event", n[7].c_str(), ch::CPNT, co::CPNT_y, R::DistancePoint, kDist[k].col);
    C("event", n[8].c_str(), ch::CPNT, co::CPNT_shape, R::DistancePoint, kDist[k].col);
  }
  C("event", "curves[#].parameter", ch::CURV, co::CURV_input, R::Owner1);
  C("event", "curves[#].target", ch::CURV, co::CURV_target, R::Owner1);
  C("event", "curves[#].compose", ch::CURV, co::CURV_compose, R::Owner1);
  C("event", "curves[#].points[#][0]", ch::CPNT, co::CPNT_x, R::Owner2);
  C("event", "curves[#].points[#][1]", ch::CPNT, co::CPNT_y, R::Owner2);
  C("event", "curves[#].points[#][2]", ch::CPNT, co::CPNT_shape, R::Owner2);
  C("event", "tracks[#].flags", ch::TRAK, co::TRAK_flags, R::Owner1);
  C("event", "tracks[#].layerParameter", ch::TRAK, co::TRAK_layerParam, R::Owner1);
  const char* clip = "tracks[#].clips[#].";
  struct ClipCol {
    const char* m;
    std::uint16_t col;
  };
  static const ClipCol kClip[] = {{"startMs", co::CLIP_startMs},          {"weight", co::CLIP_weight},
                                  {"volumeDb", co::CLIP_volumeDb},        {"pitchCents", co::CLIP_pitchCents},
                                  {"random.volumeDb", co::CLIP_randVolumeDb}, {"random.pitchCents", co::CLIP_randPitchCents},
                                  {"random.pan", co::CLIP_randPan},       {"random.startMs", co::CLIP_randStartMs},
                                  {"random.filterHz", co::CLIP_randFilterHz}, {"switchValue", co::CLIP_switchValue},
                                  {"layer", co::CLIP_layerMin},           {"layer", co::CLIP_layerMax},
                                  {"layer[0]", co::CLIP_layerMin},        {"layer[1]", co::CLIP_layerMax},
                                  {"fadeInMs", co::CLIP_fadeInMs},        {"fadeOutMs", co::CLIP_fadeOutMs},
                                  {"loopCount", co::CLIP_loopCount}};
  static const ClipCol kSynth[] = {{"synth.waveform", co::SYNP_waveform},   {"synth.frequencyHz", co::SYNP_frequencyHz},
                                   {"synth.duty", co::SYNP_duty},           {"synth.noiseColor", co::SYNP_noiseColor},
                                   {"synth.channels", co::SYNP_channels}};
  static const ClipCol kAction[] = {{"timeMs", co::ACTN_timeMs}, {"action", co::ACTN_action}, {"target", co::ACTN_target},
                                    {"value", co::ACTN_value}};
  static std::vector<std::string> clipNames;
  if (clipNames.empty()) {
    for (const ClipCol& c : kClip) clipNames.push_back(std::string(clip) + c.m);
    for (const ClipCol& c : kSynth) clipNames.push_back(std::string(clip) + c.m);
    for (const ClipCol& c : kAction) clipNames.push_back(std::string("tracks[#].actions[#].") + c.m);
  }
  std::size_t n = 0;
  for (const ClipCol& c : kClip) C("event", clipNames[n++].c_str(), ch::CLIP, c.col, R::Owner2);
  S("event", "tracks[#].clips[#].layer[#]");  // other indices (the two above are the cells)
  for (const ClipCol& c : kSynth) C("event", clipNames[n++].c_str(), ch::SYNP, c.col, R::SynpOfClip);
  // The remaining synth fields are rebuilt into immutable descriptors and PTCH.
  S("event", "tracks[#].clips[#].synth.**");
  for (const ClipCol& c : kAction) C("event", clipNames[n++].c_str(), ch::ACTN, c.col, R::Owner2);
  struct TrCol {
    const char* m;
    std::uint16_t col;
  };
  static const TrCol kTr[] = {{"music.transitions[#].from", co::MTRN_fromState},
                              {"music.transitions[#].to", co::MTRN_toState},
                              {"music.transitions[#].timing", co::MTRN_timing},
                              {"music.transitions[#].timingCount", co::MTRN_timingCount},
                              {"music.transitions[#].fadeOutBeats", co::MTRN_fadeOutBeats},
                              {"music.transitions[#].fadeInBeats", co::MTRN_fadeInBeats},
                              {"music.transitions[#].start", co::MTRN_startPosition},
                              {"music.transitions[#].startMarker", co::MTRN_startMarker},
                              {"music.transitions[#].interrupt", co::MTRN_interrupt},
                              {"music.transitions[#].late", co::MTRN_lateRule},
                              {"music.transitions[#].prepare", co::MTRN_flags}};
  for (const TrCol& t : kTr) C("event", t.m, ch::MTRN, t.col, R::Transition);
  C("event", "music.stingers[#].timing", ch::MSTG, co::MSTG_timing, R::Stinger);
  C("event", "music.stingers[#].timingCount", ch::MSTG, co::MSTG_timingCount, R::Stinger);
  C("event", "music.stingers[#].overlap", ch::MSTG, co::MSTG_overlap, R::Stinger);
  // segment
  for (const char* p : {"asset", "tempo", "tempo[#]", "markers", "markers[#]", "stems", "stems[#]", "stems[#].volumeCurve",
                        "stems[#].volumeCurve.points", "stems[#].volumeCurve.points[#]", "stems[#].asset", "entryPreload"}) {
    S("segment", p);
  }
  C("segment", "bars", ch::MSEG, co::MSEG_bars, R::Id);
  C("segment", "entryFrame", ch::MSEG, co::MSEG_entryFrame, R::Id);
  C("segment", "exitFrame", ch::MSEG, co::MSEG_exitFrame, R::Id);
  C("segment", "tempo[#].tick", ch::MTMP, co::MTMP_tick, R::SegmentRange, co::MSEG_tempoFirst);
  C("segment", "tempo[#].bpm", ch::MTMP, co::MTMP_bpm, R::SegmentRange, co::MSEG_tempoFirst);
  C("segment", "tempo[#].beatsPerBar", ch::MTMP, co::MTMP_beatsPerBar, R::SegmentRange, co::MSEG_tempoFirst);
  C("segment", "tempo[#].beatUnit", ch::MTMP, co::MTMP_beatUnit, R::SegmentRange, co::MSEG_tempoFirst);
  C("segment", "markers[#].name", ch::MMRK, co::MMRK_id, R::SegmentRange, co::MSEG_markerFirst);
  C("segment", "markers[#].frame", ch::MMRK, co::MMRK_frame, R::SegmentRange, co::MSEG_markerFirst);
  C("segment", "markers[#].kind", ch::MMRK, co::MMRK_kind, R::SegmentRange, co::MSEG_markerFirst);
  C("segment", "stems[#].channelFirst", ch::MSTM, co::MSTM_channelFirst, R::Stem);
  C("segment", "stems[#].channelCount", ch::MSTM, co::MSTM_channelCount, R::Stem);
  C("segment", "stems[#].volumeCurve.parameter", ch::CURV, co::CURV_input, R::StemCurve);
  C("segment", "stems[#].volumeCurve.target", ch::CURV, co::CURV_target, R::StemCurve);
  C("segment", "stems[#].volumeCurve.compose", ch::CURV, co::CURV_compose, R::StemCurve);
  C("segment", "stems[#].volumeCurve.points[#][0]", ch::CPNT, co::CPNT_x, R::StemPoint);
  C("segment", "stems[#].volumeCurve.points[#][1]", ch::CPNT, co::CPNT_y, R::StemPoint);
  C("segment", "stems[#].volumeCurve.points[#][2]", ch::CPNT, co::CPNT_shape, R::StemPoint);
  // playlist
  for (const char* p : {"entries", "entries[#]", "entries[#].segment"}) S("playlist", p);
  C("playlist", "mode", ch::MPLS, co::MPLS_mode, R::Id);
  C("playlist", "loopCount", ch::MPLS, co::MPLS_loopCount, R::Id);
  C("playlist", "entries[#].weight", ch::MPLE, co::MPLE_weight, R::Owner1);
  C("playlist", "entries[#].repeat", ch::MPLE, co::MPLE_repeat, R::Owner1);
  // materialTable: the PHYS blob.
  S("materialTable", "**");
  // patch: the XPCH blob, except where the editor draws the nodes.
  N("patch", "nodes[#].position");
  N("patch", "nodes[#].position[#]");
  S("patch", "**");
  return f;
}

// Segment-wise match: "[#]" (or "[]" in the path) any index, "*" any member name, "**" the rest.
bool MatchSegments(const std::string& path, const std::string& pattern, std::vector<std::uint32_t>* idx) {
  auto split = [](const std::string& s) {
    std::vector<std::string> out;
    std::string cur;
    for (std::size_t i = 0; i < s.size(); ++i) {
      if (s[i] == '.') {
        if (!cur.empty()) out.push_back(cur);
        cur.clear();
      } else if (s[i] == '[') {
        if (!cur.empty()) out.push_back(cur);
        const std::size_t e = s.find(']', i);
        out.push_back(s.substr(i, e == std::string::npos ? std::string::npos : e - i + 1));
        cur.clear();
        if (e == std::string::npos) break;
        i = e;
      } else {
        cur.push_back(s[i]);
      }
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
  };
  const std::vector<std::string> a = split(path), b = split(pattern);
  idx->clear();
  std::size_t i = 0;
  for (; i < b.size(); ++i) {
    if (b[i] == "**") return i < a.size();
    if (i >= a.size()) return false;
    const bool aIdx = a[i].front() == '[';
    if (b[i] == "[#]") {
      if (!aIdx) return false;
      std::uint32_t v = 0;
      for (std::size_t k = 1; k + 1 < a[i].size(); ++k) {
        if (a[i][k] < '0' || a[i][k] > '9') return false;
        v = v * 10u + static_cast<std::uint32_t>(a[i][k] - '0');
      }
      idx->push_back(v);
      continue;
    }
    if (b[i].front() == '[') {  // a fixed index: "[0]"
      if (a[i] != b[i]) return false;
      continue;
    }
    if (aIdx) return false;
    if (b[i] != "*" && a[i] != b[i]) return false;
  }
  return i == a.size();
}

std::uint32_t FindIdRow(const ParsedBank& v, std::uint32_t fourcc, Id id) {
  for (std::uint32_t r = 0, n = v.Rows(fourcc); r < n; ++r) {
    if (U32At(v, fourcc, 0, r) == id) return r;
  }
  return bf::kNone;
}
std::uint32_t InRange(const ParsedBank& v, std::uint32_t chunk, std::uint32_t row, std::uint32_t firstCol,
                      std::uint32_t countCol, std::uint32_t index) {
  if (row == bf::kNone) return bf::kNone;
  const std::uint32_t first = U32At(v, chunk, firstCol, row);
  const std::uint32_t count = U32At(v, chunk, countCol, row);
  return (first == bf::kNone || index >= count) ? bf::kNone : first + index;
}

}  // namespace

// --- BankImage ------------------------------------------------------------------------------
struct ParsedBank::Checked {
  bf::check::CheckedBank bank;
};

bool ParsedBank::Parse(const std::vector<std::uint8_t>& bytes) {
  storage_.assign(bytes.size() + 64, 0);
  const std::uintptr_t a = reinterpret_cast<std::uintptr_t>(storage_.data());
  const std::size_t off = static_cast<std::size_t>((64u - (a & 63u)) & 63u);
  if (!bytes.empty()) std::memcpy(storage_.data() + off, bytes.data(), bytes.size());
  data_ = storage_.data() + off;
  bytes_ = bytes.size();
  auto c = std::make_shared<Checked>();
  bf::check::CheckOptions opt;
  bf::check::CheckError err;
  std::uint32_t plat = bf::kPlatformAny;
  if (bytes.size() >= 12) std::memcpy(&plat, bytes.data() + 8, 4);
  opt.platform = plat;  // a bank built for one platform reads as that platform
  ok_ = bf::check::CheckBank(data_, bytes_, opt, &c->bank, &err) == SND_OK;
  checked_ = c;
  return ok_;
}
std::uint32_t ParsedBank::bankId() const { return ok_ ? checked_->bank.header.bankId : 0u; }
std::uint32_t ParsedBank::languageId() const {
  return ok_ && (checked_->bank.header.flags & bf::kFileLanguageBank) != 0u ? checked_->bank.header.languageId : 0u;
}
std::uint32_t ParsedBank::dependencyCount() const { return ok_ ? checked_->bank.header.dependencyCount : 0u; }
std::uint32_t ParsedBank::dependencyBankId(std::uint32_t i) const {
  if (!ok_ || i >= dependencyCount()) return 0;
  bf::Dependency d{};
  std::memcpy(&d, data_ + checked_->bank.header.dependencyOffset + std::uint64_t{i} * sizeof(d), sizeof(d));
  return d.bankId;
}
std::uint32_t ParsedBank::Rows(std::uint32_t fourcc) const {
  const int i = bf::check::KnownChunkIndex(fourcc);
  return ok_ && i >= 0 ? checked_->bank.tables[i].rows : 0u;
}
bool ParsedBank::HasColumn(std::uint32_t fourcc, std::uint32_t column) const {
  const int i = bf::check::KnownChunkIndex(fourcc);
  return ok_ && i >= 0 && column < bf::check::kMaxTableColumns && checked_->bank.tables[i].columns[column] != nullptr;
}
bool ParsedBank::Cell(std::uint32_t fourcc, std::uint32_t column, std::uint32_t row, std::uint64_t* bits) const {
  const int i = bf::check::KnownChunkIndex(fourcc);
  const bf::ColumnInfo* info = bf::check::FindColumnInfo(fourcc, column);
  if (!ok_ || i < 0 || info == nullptr || column >= bf::check::kMaxTableColumns) return false;
  const bf::check::TableView& t = checked_->bank.tables[i];
  if (row >= t.rows) return false;
  const std::size_t elem = bf::ColumnTypeBytes(info->type);
  std::uint64_t b = 0;
  if (t.columns[column] == nullptr) {
    switch (info->type) {
      case bf::ColumnType::F32: {
        const float f = static_cast<float>(info->defaultValue);
        std::uint32_t u = 0;
        std::memcpy(&u, &f, 4);
        b = u;
        break;
      }
      case bf::ColumnType::U32: b = static_cast<std::uint32_t>(info->defaultValue); break;
      case bf::ColumnType::U64: b = static_cast<std::uint64_t>(info->defaultValue); break;
      case bf::ColumnType::I64: b = static_cast<std::uint64_t>(static_cast<std::int64_t>(info->defaultValue)); break;
    }
  } else {
    std::memcpy(&b, static_cast<const std::uint8_t*>(t.columns[column]) + std::size_t{row} * elem, elem);
  }
  *bits = b;
  return true;
}
std::uint32_t ParsedBank::U32(std::uint32_t fourcc, std::uint32_t column, std::uint32_t row) const {
  std::uint64_t b = 0;
  return Cell(fourcc, column, row, &b) ? static_cast<std::uint32_t>(b) : bf::kNone;
}
float ParsedBank::F32(std::uint32_t fourcc, std::uint32_t column, std::uint32_t row) const {
  std::uint64_t b = 0;
  return Cell(fourcc, column, row, &b) ? BitsToFloat(b) : 0.0f;
}
const std::uint8_t* ParsedBank::Chunk(std::uint32_t fourcc, std::uint64_t* bytes) const {
  *bytes = 0;
  if (!ok_) return nullptr;
  const bf::FileHeader& h = checked_->bank.header;
  for (std::uint32_t i = 0; i < h.chunkCount; ++i) {
    bf::ChunkEntry e{};
    std::memcpy(&e, data_ + h.chunkTableOffset + std::uint64_t{i} * sizeof(e), sizeof(e));
    if (e.fourcc == fourcc && e.offset <= bytes_ && e.bytes <= bytes_ - e.offset) {
      *bytes = e.bytes;
      return data_ + e.offset;
    }
  }
  return nullptr;
}
bool ParsedBank::Guid(Id id, std::uint64_t* lo, std::uint64_t* hi) const {
  for (std::uint32_t r = 0, n = Rows(ch::GUID); r < n; ++r) {
    if (U32(ch::GUID, co::GUID_id, r) != id) continue;
    std::uint64_t a = 0, b = 0;
    Cell(ch::GUID, co::GUID_guidLo, r, &a);
    Cell(ch::GUID, co::GUID_guidHi, r, &b);
    *lo = a;
    *hi = b;
    return true;
  }
  return false;
}
std::uint32_t ParsedBank::FindId(std::uint32_t fourcc, Id id) const {
  for (std::uint32_t r = 0, n = Rows(fourcc); r < n; ++r) {
    if (U32(fourcc, 0, r) == id) return r;
  }
  return bf::kNone;
}

BankImage::BankImage() = default;
BankImage::~BankImage() = default;
namespace {
std::mutex& ViewMutex() {
  static std::mutex m;
  return m;
}
}  // namespace

// Copies share the parse (the bytes are the same; a ParsedBank is read-only once made).
BankImage::BankImage(const BankImage& o)
    : name(o.name), language(o.language), file(o.file), bankId(o.bankId), contentHash(o.contentHash), bytes(o.bytes) {
  std::lock_guard<std::mutex> lock(ViewMutex());
  parsed_ = o.parsed_;
}
BankImage& BankImage::operator=(const BankImage& o) {
  if (this != &o) {
    name = o.name;
    language = o.language;
    file = o.file;
    bankId = o.bankId;
    contentHash = o.contentHash;
    bytes = o.bytes;
    std::lock_guard<std::mutex> lock(ViewMutex());
    parsed_ = o.parsed_;
  }
  return *this;
}
BankImage::BankImage(BankImage&&) noexcept = default;
BankImage& BankImage::operator=(BankImage&&) noexcept = default;

const ParsedBank* BankImage::view() const {
  std::lock_guard<std::mutex> lock(ViewMutex());
  if (!parsed_) {
    auto p = std::make_shared<ParsedBank>();
    p->Parse(bytes);
    parsed_ = std::move(p);
  }
  return parsed_->ok() ? parsed_.get() : nullptr;
}

// --- DocumentSource -------------------------------------------------------------------------
void DocumentSource::ListObjectFiles(std::vector<std::string>* out) const {
  out->clear();
  for (const DocObject& o : doc_.objects()) out->push_back(o.file);
  for (const std::string& f : doc_.unreadFiles()) out->push_back(f);
}

bool DocumentSource::ReadFile(const std::string& path, std::vector<std::uint8_t>* out) const {
  for (const DocObject& o : doc_.objects()) {
    if (o.file == path) {
      const std::string t = doc_.TextOf(o);
      out->assign(t.begin(), t.end());
      return true;
    }
  }
  return doc_.ReadBinaryFile(path, out);
}

// --- build ----------------------------------------------------------------------------------
const BankImage* DocBuild::FindBank(const std::string& name, const std::string& language) const {
  for (const BankImage& b : banks) {
    if (b.name == name && b.language == language) return &b;
  }
  return nullptr;
}
std::uint32_t DocBuild::errorCount() const {
  std::uint32_t n = 0;
  for (const DocIssue& i : issues) n += i.error ? 1u : 0u;
  return n;
}

DocBuild BuildDocument(const ProjectDocument& doc, const DocBuildOptions& options) {
  DocBuild out;
  out.documentRevision = doc.revision();
  DocumentSource src(doc);
  buildtool::BuildOptions o;
  o.projectDir = doc.directory();
  o.source = &src;
  std::vector<buildtool::OutputFile> files;
  o.memoryOutput = &files;
  o.cacheDir = options.cacheDir;
  o.platform = options.platform;
  o.devBuild = options.devBuild;
  o.allowEditorOnlyPatches = options.devBuild;
  o.warningsAsErrors = options.warningsAsErrors;
  o.dspSampleRate = options.dspSampleRate;
  o.dspMaxFrames = options.dspMaxFrames;
  o.encoders = options.encoders;
  buildtool::Diagnostics diag;
  buildtool::BuildResult r;
  out.ok = buildtool::Build(o, diag, &r);
  // Issues, tied to objects and members.
  std::map<std::string, Value> reparsed;
  for (const buildtool::Diagnostic& d : diag.items()) {
    DocIssue is;
    is.error = d.severity == buildtool::Severity::Error;
    is.file = d.file;
    is.line = d.line;
    is.column = d.column;
    is.message = d.message;
    is.text = buildtool::Diagnostics::Format(d);
    for (const DocObject& ob : doc.objects()) {
      if (ob.file != d.file) continue;
      is.guid = ob.guid;
      if (d.line > 0) {
        auto it = reparsed.find(ob.file);
        if (it == reparsed.end()) {
          Value v;
          json::ParseError pe;
          json::Parse(doc.TextOf(ob), &v, &pe);
          it = reparsed.emplace(ob.file, std::move(v)).first;
        }
        std::string path;
        PathAt(it->second, d.line, d.column, std::string(), &path);
        is.path = path;
      }
      break;
    }
    out.issues.push_back(std::move(is));
  }
  if (!out.ok) return out;
  out.buildHash = r.projectBuildHash;
  for (buildtool::OutputFile& f : files) {
    bool isBank = false;
    for (const buildtool::BuiltBank& b : r.banks) {
      if (b.path != f.path) continue;
      BankImage bi;
      bi.name = b.name;
      bi.language = b.language;
      bi.file = b.path;
      bi.bankId = b.bankId;
      bi.contentHash = b.contentHash;
      bi.bytes = f.bytes;
      bi.view();  // parsed now (D-178d): readers on other threads find it made
      out.banks.push_back(std::move(bi));
      isBank = true;
      break;
    }
    if (!isBank) out.files.push_back(std::move(f));
  }
  if (!options.outDir.empty()) {
    std::error_code ec;
    auto write = [&](const std::string& rel, const std::vector<std::uint8_t>& bytes) {
      const fs::path p = fs::path(options.outDir) / rel;
      fs::create_directories(p.parent_path(), ec);
      std::vector<std::uint8_t> old;
      std::FILE* f = std::fopen(p.string().c_str(), "rb");
      if (f != nullptr) {
        std::uint8_t buf[65536];
        for (std::size_t n; (n = std::fread(buf, 1, sizeof buf, f)) > 0;) old.insert(old.end(), buf, buf + n);
        std::fclose(f);
        if (old == bytes) return true;
      }
      f = std::fopen(p.string().c_str(), "wb");
      if (f == nullptr) return false;
      const bool ok = bytes.empty() || std::fwrite(bytes.data(), 1, bytes.size(), f) == bytes.size();
      return std::fclose(f) == 0 && ok;
    };
    bool okAll = true;
    for (const BankImage& b : out.banks) okAll = write(b.file, b.bytes) && okAll;
    for (const buildtool::OutputFile& f : out.files) okAll = write(f.path, f.bytes) && okAll;
    if (!okAll) {
      out.ok = false;
      DocIssue is;
      is.message = "cannot write the build output to " + options.outDir;
      is.text = "<project>: error: " + is.message;
      out.issues.push_back(std::move(is));
    }
  }
  return out;
}

std::vector<DocIssue> CheckDocument(const ProjectDocument& doc) { return BuildDocument(doc).issues; }

std::vector<DocIssue> ToolIssues(const ProjectDocument& doc) {
  std::vector<DocIssue> out;
  for (const DocObject& o : doc.objects()) {
    if (o.kind() != "segment") continue;
    const json::Value* tempo = o.doc.Find("tempo");
    if (tempo == nullptr || !tempo->IsArray()) continue;
    for (std::size_t i = 0; i < tempo->items.size(); ++i) {
      const json::Value& p = tempo->items[i];
      auto whole = [](double v, double lo, double hi) { return std::isfinite(v) && v == std::floor(v) && v >= lo && v <= hi; };
      const double bpm = NumberAt(p, "bpm", 120.0), bpb = NumberAt(p, "beatsPerBar", 4.0);
      const double unit = NumberAt(p, "beatUnit", 4.0), tick = NumberAt(p, "tick", 0.0);
      const char* what = nullptr;
      const char* member = nullptr;
      if (!(bpm >= 1e-3 && bpm <= 1e5)) {
        what = "bpm must be 0.001 .. 100000";
        member = "bpm";
      } else if (!whole(bpb, 1.0, 4294967295.0)) {
        what = "beatsPerBar must be a whole number 1 .. 4294967295";
        member = "beatsPerBar";
      } else if (!(unit == 1.0 || unit == 2.0 || unit == 4.0 || unit == 8.0 || unit == 16.0 || unit == 32.0)) {
        what = "beatUnit must be 1, 2, 4, 8, 16 or 32";
        member = "beatUnit";
      } else if (!whole(tick, 0.0, 18446744073709551615.0)) {
        what = "tick must be a whole number >= 0";
        member = "tick";
      }
      if (what == nullptr) continue;
      DocIssue d;
      d.error = true;
      d.file = o.file;
      d.guid = o.guid;
      d.path = "tempo[" + std::to_string(i) + "]." + member;
      const json::Value* at = p.Find(member);
      d.line = at != nullptr ? at->line : p.line;
      d.column = at != nullptr ? at->column : p.column;
      d.message = "tempo point " + std::to_string(i) + ": " + what + " (the tool reads it as 120 BPM 4/4)";
      d.text = d.file + ":" + std::to_string(d.line) + ":" + std::to_string(d.column) + ": error: " + d.message;
      out.push_back(std::move(d));
    }
  }
  return out;
}

std::vector<DocIssue> DistinctIssues(const std::vector<DocIssue>& issues) {
  std::vector<DocIssue> out;
  for (const DocIssue& i : issues) {
    bool seen = false;
    for (const DocIssue& o : out) {
      seen = seen || (o.error == i.error && o.file == i.file && o.line == i.line && o.column == i.column &&
                      o.message == i.message);
    }
    if (!seen) out.push_back(i);
  }
  return out;
}

std::vector<DocIssue> IssuesOf(const std::vector<DocIssue>& issues, const std::string& guid) {
  std::vector<DocIssue> out;
  const std::string g = NormalizeGuid(guid);
  for (const DocIssue& i : issues) {
    if (i.guid == g) out.push_back(i);
  }
  return out;
}

// --- cells ------------------------------------------------------------------------------------
float BitsToFloat(std::uint64_t bits) {
  const std::uint32_t lo = static_cast<std::uint32_t>(bits);
  float f = 0;
  std::memcpy(&f, &lo, 4);
  return f;
}

const std::vector<FieldColumn>& MemberTable() {
  static const std::vector<FieldColumn> f = MakeFields();
  return f;
}
const std::vector<FieldColumn>& FieldColumns() {
  static const std::vector<FieldColumn> f = [] {
    std::vector<FieldColumn> c;
    for (const FieldColumn& x : MemberTable()) {
      if (x.effect == FieldEffect::Column) c.push_back(x);
    }
    return c;
  }();
  return f;
}

FieldEffect ClassifyMember(const std::string& kind, const std::string& path) {
  // "notes" at any level is the tool's (project-format.md §1).
  if (path == "notes" || path.rfind("notes.", 0) == 0 || path.rfind("notes[", 0) == 0 ||
      path.find(".notes") != std::string::npos) {
    const std::size_t at = path.find(".notes");
    if (at == std::string::npos || at + 6 == path.size() || path[at + 6] == '.' || path[at + 6] == '[') {
      return FieldEffect::NotInBank;
    }
  }
  std::vector<std::uint32_t> idx;
  for (const FieldColumn& f : MemberTable()) {
    if ((std::string_view(f.kind) == "*" || kind == f.kind) && MatchSegments(path, f.pattern, &idx)) return f.effect;
  }
  return FieldEffect::Unknown;
}

std::vector<CellRef> LocateField(const DocBuild& build, const ProjectDocument& doc, const std::string& guid,
                                 const std::string& path) {
  std::vector<CellRef> out;
  const DocObject* o = doc.Find(guid);
  if (o == nullptr || ClassifyMember(o->kind(), path) != FieldEffect::Column) return out;
  const std::string kind = o->kind();
  const Id id = HashId(o->name());
  std::vector<std::uint32_t> idx;
  for (const FieldColumn& f : FieldColumns()) {
    if (f.effect != FieldEffect::Column || kind != f.kind || !MatchSegments(path, f.pattern, &idx)) continue;
    const FieldColumn* field = &f;
    for (const BankImage& b : build.banks) {
      const ParsedBank* v = b.view();
      if (v == nullptr) continue;
      std::uint32_t row = bf::kNone;
      auto nth = [&](std::uint32_t chunk, std::uint32_t col, std::uint32_t value, std::uint32_t n) {
        for (std::uint32_t r = 0, rows = v->Rows(chunk); r < rows; ++r) {
          if (U32At(*v, chunk, col, r) == value && n-- == 0u) return r;
        }
        return bf::kNone;
      };
      auto curvePoint = [&](std::uint32_t curve, std::uint32_t p) {
        return curve == bf::kNone || curve >= v->Rows(ch::CURV)
                   ? bf::kNone
                   : InRange(*v, ch::CURV, curve, co::CURV_pointFirst, co::CURV_pointCount, p);
      };
      switch (field->row) {
        case FieldRow::Id: row = FindIdRow(*v, field->chunk, id); break;
        case FieldRow::Owner1: {
          const Owner* o1 = FindOwner(kOwners, std::size(kOwners), field->chunk);
          if (o1 != nullptr && !idx.empty()) {
            row = InRange(*v, o1->parent, FindIdRow(*v, o1->parent, id), o1->firstCol, o1->countCol, idx[0]);
          }
          break;
        }
        case FieldRow::Owner2:
        case FieldRow::SynpOfClip: {
          const std::uint32_t chunk2 = field->row == FieldRow::SynpOfClip ? ch::CLIP : field->chunk;
          const Owner* o2 = FindOwner(kOwners2, std::size(kOwners2), chunk2);
          const Owner* p1 = o2 != nullptr ? FindOwner(kOwners, std::size(kOwners), o2->parent) : nullptr;
          if (p1 != nullptr && idx.size() >= 2) {
            const std::uint32_t mid = InRange(*v, p1->parent, FindIdRow(*v, p1->parent, id), p1->firstCol, p1->countCol, idx[0]);
            row = InRange(*v, o2->parent, mid, o2->firstCol, o2->countCol, idx[1]);
          }
          if (field->row == FieldRow::SynpOfClip && row != bf::kNone) {
            row = U32At(*v, ch::CLIP, co::CLIP_kind, row) == 2u ? U32At(*v, ch::CLIP, co::CLIP_target, row) : bf::kNone;
          }
          break;
        }
        case FieldRow::Duck: {
          // The duckings of the bank in name order (build.cpp).
          std::uint32_t r = 0;
          for (const DocObject* d : doc.OfKind("ducking")) {
            const DocObject* bk = doc.Resolve("bank", StringAt(d->doc, "bank"));
            if (bk == nullptr || bk->name() != b.name) continue;
            if (d->guid == o->guid) {
              row = r;
              break;
            }
            ++r;
          }
          break;
        }
        case FieldRow::Transition: {
          const std::uint32_t ev = FindIdRow(*v, ch::EVNT, id);
          if (ev != bf::kNone && !idx.empty()) row = nth(ch::MTRN, co::MTRN_musicEvent, ev, idx[0]);
          break;
        }
        case FieldRow::Stinger:
          if (!idx.empty()) {
            const std::string name = StringAt(o->doc, "music.stingers[" + std::to_string(idx[0]) + "].name");
            if (!name.empty()) row = FindIdRow(*v, ch::MSTG, HashId(name));
          }
          break;
        case FieldRow::SegmentRange:
          if (!idx.empty()) {
            const std::uint32_t countCol = field->aux == co::MSEG_tempoFirst ? co::MSEG_tempoCount : co::MSEG_markerCount;
            row = InRange(*v, ch::MSEG, FindIdRow(*v, ch::MSEG, id), field->aux, countCol, idx[0]);
          }
          break;
        case FieldRow::Stem:
        case FieldRow::StemCurve:
        case FieldRow::StemPoint: {
          const std::uint32_t seg = FindIdRow(*v, ch::MSEG, id);
          const std::uint32_t stem = seg != bf::kNone && !idx.empty() ? nth(ch::MSTM, co::MSTM_segment, seg, idx[0]) : bf::kNone;
          if (field->row == FieldRow::Stem) {
            row = stem;
          } else if (stem != bf::kNone) {
            const std::uint32_t curve = U32At(*v, ch::MSTM, co::MSTM_volumeCurve, stem);
            row = field->row == FieldRow::StemCurve ? (curve < v->Rows(ch::CURV) ? curve : bf::kNone)
                                                    : (idx.size() >= 2 ? curvePoint(curve, idx[1]) : bf::kNone);
          }
          break;
        }
        case FieldRow::DistanceCurve:
        case FieldRow::DistancePoint: {
          const std::uint32_t ev = FindIdRow(*v, ch::EVNT, id);
          const std::uint32_t curve = ev != bf::kNone ? U32At(*v, ch::EVNT, field->aux, ev) : bf::kNone;
          if (field->row == FieldRow::DistanceCurve) {
            row = curve < v->Rows(ch::CURV) ? curve : bf::kNone;
          } else if (!idx.empty()) {
            row = curvePoint(curve, idx[0]);
          }
          break;
        }
        case FieldRow::None: break;
      }
      if (row == bf::kNone || row >= v->Rows(field->chunk)) continue;
      CellRef c;
      c.bank = b.name;
      c.language = b.language;
      c.bankId = b.bankId;
      c.chunk = field->chunk;
      c.column = field->column;
      c.row = row;
      if (!v->Cell(c.chunk, c.column, row, &c.bits)) continue;
      Address(*v, &c);
      Classify(*v, &c);
      out.push_back(c);
    }
  }
  return out;
}

// --- diff ---------------------------------------------------------------------------------------
LiveDelta DiffBuilds(const DocBuild& before, const DocBuild& after) {
  LiveDelta d;
  auto key = [](const BankImage& b) { return b.file; };
  std::map<std::string, const BankImage*> old;
  for (const BankImage& b : before.banks) old[key(b)] = &b;
  std::set<std::string> seen;
  for (const BankImage& nb : after.banks) {
    seen.insert(key(nb));
    auto it = old.find(key(nb));
    if (it == old.end()) {
      d.banksAdded.push_back(nb.file);
      continue;
    }
    const BankImage& ob = *it->second;
    const ParsedBank* ov = ob.view();
    const ParsedBank* nv = nb.view();
    if (ov == nullptr || nv == nullptr) {
      d.banksRelinked.push_back(nb.file);
      continue;
    }
    // Identity and dependencies.
    bool relink = ov->bankId() != nv->bankId() || ov->languageId() != nv->languageId() ||
                  ov->dependencyCount() != nv->dependencyCount();
    for (std::uint32_t i = 0; !relink && i < ov->dependencyCount(); ++i) {
      relink = ov->dependencyBankId(i) != nv->dependencyBankId(i);
    }
    if (relink) d.banksRelinked.push_back(nb.file);
    auto chunkEdit = [&](std::uint32_t fourcc, const std::uint8_t* data, std::uint64_t n) {
      ChunkEdit e;
      e.bank = nb.name;
      e.language = nb.language;
      e.bankId = nb.bankId;
      e.fourcc = fourcc;
      e.removed = data == nullptr;
      if (!e.removed) e.bytes.assign(data, data + n);
      d.chunks.push_back(std::move(e));
    };
    std::vector<std::uint32_t> fourccs;
    for (const bf::ChunkInfo& ci : bf::kChunks) fourccs.push_back(ci.fourcc);
    fourccs.push_back(buildtool::patch::kChunkFourcc);
    for (std::uint32_t fourcc : fourccs) {
      std::uint64_t on = 0, nn = 0;
      const std::uint8_t* oc = ov->Chunk(fourcc, &on);
      const std::uint8_t* nc = nv->Chunk(fourcc, &nn);
      if (on == nn && (oc == nullptr) == (nc == nullptr) && (on == 0 || std::memcmp(oc, nc, on) == 0)) continue;
      bool isTable = false;
      for (const bf::ChunkInfo& ci : bf::kChunks) {
        if (ci.fourcc == fourcc) isTable = ci.kind == bf::ChunkKind::Table;
      }
      // Cells only for tables that keep their shape (rows and present columns); the
      // strings / names / GUIDs are replaced whole.
      const bool cellwise = isTable && oc != nullptr && nc != nullptr && ov->Rows(fourcc) == nv->Rows(fourcc) &&
                            nv->Rows(fourcc) > 0 && fourcc != ch::NAME && fourcc != ch::GUID && fourcc != ch::ALIS;
      bool sameColumns = true;
      for (std::uint32_t c = 0; cellwise && c < bf::check::kMaxTableColumns; ++c) {
        sameColumns = sameColumns && ov->HasColumn(fourcc, c) == nv->HasColumn(fourcc, c);
      }
      if (!cellwise || !sameColumns) {
        chunkEdit(fourcc, nc, nn);
        continue;
      }
      const std::uint32_t rows = nv->Rows(fourcc);
      std::vector<ValueEdit> cells;
      bool allLive = true;
      for (std::uint32_t c = 0; c < bf::check::kMaxTableColumns; ++c) {
        if (bf::check::FindColumnInfo(fourcc, c) == nullptr) continue;
        for (std::uint32_t r = 0; r < rows; ++r) {
          std::uint64_t a = 0, b = 0;
          if (!ov->Cell(fourcc, c, r, &a) || !nv->Cell(fourcc, c, r, &b) || a == b) continue;
          ValueEdit e;
          e.cell.bank = nb.name;
          e.cell.language = nb.language;
          e.cell.bankId = nb.bankId;
          e.cell.chunk = fourcc;
          e.cell.column = c;
          e.cell.row = r;
          e.cell.bits = b;
          e.oldBits = a;
          Address(*nv, &e.cell);
          Classify(*nv, &e.cell);
          allLive = allLive && e.cell.liveWritable;
          cells.push_back(e);
        }
      }
      if (cells.empty() || !allLive) {
        chunkEdit(fourcc, nc, nn);
      } else {
        for (ValueEdit& e : cells) d.values.push_back(std::move(e));
      }
    }
  }
  for (const BankImage& ob : before.banks) {
    if (seen.count(key(ob)) == 0u) d.banksRemoved.push_back(ob.file);
  }
  if (!d.chunks.empty() || !d.banksAdded.empty() || !d.banksRemoved.empty() || !d.banksRelinked.empty()) {
    d.kind = EditClass::Structure;
  } else if (!d.values.empty()) {
    d.kind = EditClass::Value;
  }
  return d;
}

EditClass ClassifyEdit(const ProjectDocument& before, const ProjectDocument& after, LiveDelta* delta) {
  const DocBuild a = BuildDocument(before);
  const DocBuild b = BuildDocument(after);
  LiveDelta d;
  if (!a.ok || !b.ok) {
    d.kind = EditClass::Structure;
  } else {
    d = DiffBuilds(a, b);
  }
  if (delta != nullptr) *delta = d;
  return d.kind;
}

}  // namespace snd::tool
