// Music editor model (music_editor.hpp).
#include "snd/tool/music_editor.hpp"

#include <algorithm>
#include <cmath>
#include <set>

#include "snd/buildtool/wav.hpp"
#include "snd/tool/event_editor.hpp"

namespace snd::tool {
namespace {

using json::Value;
constexpr double kEps = 1e-7;  // music/src/timeline.cpp
const char* const kTimingNames[] = {"immediate", "nextBeat", "nextBar", "barsAfter", "grid", "nextMarker", "segmentEnd"};
const char* const kStartNames[] = {"head", "sameBar", "marker", "lastPosition"};
const char* const kLateNames[] = {"nextGrid", "startLate", "cancel"};
const char* const kMarkerKinds[] = {"user", "entry", "exit", "pickup"};
const char* const kModes[] = {"sequential", "random", "shuffle"};

template <class E, std::size_t N>
E ParseEnum(const std::string& s, const char* const (&names)[N], E def) {
  for (std::size_t i = 0; i < N; ++i) {
    if (s == names[i]) return static_cast<E>(i);
  }
  return def;
}

bool ValidUnit(std::uint32_t u) { return u == 1 || u == 2 || u == 4 || u == 8 || u == 16 || u == 32; }

std::string Idx(const char* a, std::uint32_t i, const char* rest = "") {
  return std::string(a) + "[" + std::to_string(i) + "]" + rest;
}

}  // namespace

// --- TempoMap ------------------------------------------------------------------------------
TempoMap::TempoMap(std::vector<TempoPointDesc> points) : points_(std::move(points)) {
  if (points_.empty()) points_.push_back(TempoPointDesc{});
  std::stable_sort(points_.begin(), points_.end(), [](const TempoPointDesc& a, const TempoPointDesc& b) { return a.tick < b.tick; });
  for (TempoPointDesc& p : points_) {
    if (!(p.bpm > 0.0)) p.bpm = 120.0;
    if (p.beatsPerBar == 0u) p.beatsPerBar = 4;
    if (p.beatUnit == 0u) p.beatUnit = 4;
  }
  bars_.assign(points_.size(), 0.0);
  bars_[0] = static_cast<double>(points_[0].tick) / BarTicks(0);
  for (std::size_t j = 1; j < points_.size(); ++j) {
    bars_[j] = bars_[j - 1] + static_cast<double>(points_[j].tick - points_[j - 1].tick) / BarTicks(static_cast<std::uint32_t>(j - 1));
  }
}
std::uint32_t TempoMap::PointAt(double tick) const {
  std::uint32_t i = 0;
  while (i + 1u < points_.size() && static_cast<double>(points_[i + 1u].tick) <= tick + kEps) ++i;
  return i;
}
double TempoMap::BeatTicks(std::uint32_t p) const { return 4.0 * kTicksPerQuarter / static_cast<double>(points_[p].beatUnit); }
double TempoMap::BarTicks(std::uint32_t p) const { return BeatTicks(p) * static_cast<double>(points_[p].beatsPerBar); }
double TempoMap::BpmAt(double tick) const { return points_[PointAt(tick)].bpm; }
double TempoMap::SecondsAtTick(double tick) const {
  double s = 0.0;
  double t = 0.0;
  std::uint32_t i = 0;
  if (tick < 0.0) return tick / (points_[0].bpm / 60.0 * kTicksPerQuarter);
  while (i + 1u < points_.size() && static_cast<double>(points_[i + 1u].tick) < tick) {
    const double next = static_cast<double>(points_[i + 1u].tick);
    if (next > t) {
      s += (next - t) / (points_[i].bpm / 60.0 * kTicksPerQuarter);
      t = next;
    }
    ++i;
  }
  return s + (tick - t) / (points_[i].bpm / 60.0 * kTicksPerQuarter);
}
double TempoMap::TickAtSeconds(double seconds) const {
  double s = 0.0, t = 0.0;
  std::uint32_t i = 0;
  if (seconds < 0.0) return seconds * points_[0].bpm / 60.0 * kTicksPerQuarter;
  while (i + 1u < points_.size()) {
    const double next = static_cast<double>(points_[i + 1u].tick);
    const double need = next > t ? (next - t) / (points_[i].bpm / 60.0 * kTicksPerQuarter) : 0.0;
    if (s + need > seconds) break;
    s += need;
    t = std::max(t, next);
    ++i;
  }
  return t + (seconds - s) * points_[i].bpm / 60.0 * kTicksPerQuarter;
}
double TempoMap::BarPosition(double tick) const {
  const std::uint32_t i = PointAt(tick);
  return bars_[i] + (tick - static_cast<double>(points_[i].tick)) / BarTicks(i);
}
MusicalPosition TempoMap::Musical(double tick) const {
  const std::uint32_t i = PointAt(tick);
  const double pos = BarPosition(tick);
  const double bar = std::floor(pos + kEps);
  double inBar = (pos - bar) * BarTicks(i);
  if (inBar < 0.0) inBar = 0.0;
  const double bt = BeatTicks(i);
  double beat = std::floor(inBar / bt + kEps);
  const double bpb = static_cast<double>(points_[i].beatsPerBar);
  if (beat >= bpb) beat = bpb - 1.0;
  double rest = std::floor(inBar - beat * bt + 0.5);
  if (rest < 0.0) rest = 0.0;
  if (rest >= kTicksPerQuarter) rest = kTicksPerQuarter - 1.0;
  return MusicalPosition{static_cast<std::int32_t>(bar), static_cast<std::int32_t>(beat), static_cast<std::int32_t>(rest)};
}
bool TempoMap::MusicalIsApproximate(double tick) const { return BeatTicks(PointAt(tick)) > kTicksPerQuarter; }
double TempoMap::TickOfMusical(const MusicalPosition& m) const {
  std::uint32_t i = 0;
  while (i + 1u < points_.size() && bars_[i + 1u] <= static_cast<double>(m.bar) + kEps) ++i;
  return static_cast<double>(points_[i].tick) + (static_cast<double>(m.bar) - bars_[i]) * BarTicks(i) +
         static_cast<double>(m.beat) * BeatTicks(i) + static_cast<double>(m.tick);
}
double TempoMap::TickAtBar(double bar) const {
  std::uint32_t i = 0;
  while (i + 1u < points_.size() && bars_[i + 1u] <= bar + kEps) ++i;
  return static_cast<double>(points_[i].tick) + (bar - bars_[i]) * BarTicks(i);
}
double TempoMap::NextBar(double from) const { return TickAtBar(std::ceil(BarPosition(from) - kEps)); }
double TempoMap::BarsAfter(double from, std::uint32_t n) const {
  return TickAtBar(std::floor(BarPosition(from) + kEps) + static_cast<double>(n == 0u ? 1u : n));
}
double TempoMap::NextGrid(double from, std::uint32_t division) const {
  const double grid = 4.0 * kTicksPerQuarter / static_cast<double>(division == 0u ? 4u : division);
  const double barStart = TickAtBar(std::floor(BarPosition(from) + kEps));
  const double k = std::ceil((from - barStart) / grid - kEps);
  return barStart + (k < 0.0 ? 0.0 : k) * grid;
}
double TempoMap::NextBeat(double from) const {
  const double bt = BeatTicks(PointAt(from));
  const double barStart = TickAtBar(std::floor(BarPosition(from) + kEps));
  const double k = std::ceil((from - barStart) / bt - kEps);
  return barStart + (k < 0.0 ? 0.0 : k) * bt;
}

// --- tap tempo and grid -------------------------------------------------------------------------
double TapTempo::Bpm() const {
  const std::size_t n = taps_.size();
  if (n < 2) return 0.0;
  double sx = 0, sy = 0, sxx = 0, sxy = 0;
  for (std::size_t i = 0; i < n; ++i) {
    const double x = static_cast<double>(i), y = taps_[i];
    sx += x;
    sy += y;
    sxx += x * x;
    sxy += x * y;
  }
  const double den = static_cast<double>(n) * sxx - sx * sx;
  const double slope = (static_cast<double>(n) * sxy - sx * sy) / den;  // seconds per beat
  return slope > 0.0 ? 60.0 / slope : 0.0;
}
double TapTempo::OffsetSeconds() const {
  const std::size_t n = taps_.size();
  const double bpm = Bpm();
  if (n < 2 || bpm <= 0.0) return n == 1 ? taps_[0] : 0.0;
  const double beat = 60.0 / bpm;
  double sx = 0, sy = 0;
  for (std::size_t i = 0; i < n; ++i) {
    sx += static_cast<double>(i);
    sy += taps_[i];
  }
  const double intercept = (sy - beat * sx) / static_cast<double>(n);
  double off = std::fmod(intercept, beat);
  if (off < 0.0) off += beat;
  return off;
}

std::vector<GridLine> BeatGrid(double bpm, double offsetSeconds, std::uint32_t beatsPerBar, std::uint32_t beatUnit,
                               double durationSeconds, double sampleRate) {
  std::vector<GridLine> out;
  if (!(bpm > 0.0) || beatsPerBar == 0u || !ValidUnit(beatUnit) || !(durationSeconds > 0.0)) return out;
  // bpm counts quarter notes; one beat is a note of the beat unit.
  const double beatSeconds = 60.0 / bpm * 4.0 / static_cast<double>(beatUnit);
  for (std::uint64_t k = 0;; ++k) {
    const double s = offsetSeconds + static_cast<double>(k) * beatSeconds;
    if (s > durationSeconds + 1e-12 || k > 1000000u) break;
    GridLine g;
    g.seconds = s;
    g.frame = s * sampleRate;
    g.bar = static_cast<std::uint32_t>(k / beatsPerBar);
    g.beat = static_cast<std::uint32_t>(k % beatsPerBar);
    g.barLine = g.beat == 0u;
    out.push_back(g);
  }
  return out;
}

// --- SegmentEditor -------------------------------------------------------------------------------
SegmentEditor::SegmentEditor(ProjectDocument& doc, std::string guid) : doc_(doc), guid_(NormalizeGuid(guid)) {}
bool SegmentEditor::valid() const {
  const DocObject* o = doc_.Find(guid_);
  return o != nullptr && o->kind() == "segment";
}
static bool MaterialOf(const ProjectDocument& doc, const DocObject& seg, AssetLength* len, double* channels) {
  const DocObject* a = doc.Resolve("asset", StringAt(seg.doc, "asset"));
  if (a == nullptr || !ReadAssetLength(doc, *a, len)) return false;
  if (channels != nullptr) {
    std::vector<std::uint8_t> b;
    buildtool::PcmData pcm;
    std::string err;
    *channels = doc.ReadBinaryFile(StringAt(a->doc, "file"), &b) && buildtool::ReadWav(b, &pcm, &err) ? pcm.channels : 0.0;
  }
  return true;
}
double SegmentEditor::SampleRate() const {
  AssetLength l;
  return valid() && MaterialOf(doc_, *doc_.Find(guid_), &l, nullptr) ? l.sampleRate : 0.0;
}
double SegmentEditor::Frames() const {
  AssetLength l;
  return valid() && MaterialOf(doc_, *doc_.Find(guid_), &l, nullptr) ? l.frames : 0.0;
}
bool SegmentEditor::SetAsset(const std::string& g) {
  const DocObject* a = doc_.Find(g);
  if (!valid() || a == nullptr || a->kind() != "asset") return false;
  const DocObject* cur = doc_.Resolve("asset", StringAt(doc_.Find(guid_)->doc, "asset"));
  if (cur != nullptr && cur->guid == a->guid) return true;  // D-177b
  return doc_.Set(guid_, "asset", String(a->guid));
}
bool SegmentEditor::SetBars(std::uint32_t bars) {
  return valid() && bars >= 1u && doc_.SetDefaulted(guid_, "bars", Number(bars), Number(1));
}
bool SegmentEditor::SetEntryFrame(std::uint64_t f) {
  if (!valid()) return false;
  const json::Value& d = doc_.Find(guid_)->doc;
  const double exit = NumberAt(d, "exitFrame", 0);
  if (exit > 0 && static_cast<double>(f) >= exit) return false;
  const double frames = Frames();
  if (frames > 0 && static_cast<double>(f) >= frames) return false;
  return doc_.SetDefaulted(guid_, "entryFrame", Number(static_cast<double>(f)), Number(0));
}
bool SegmentEditor::SetExitFrame(std::uint64_t f) {
  if (!valid()) return false;
  const double entry = NumberAt(doc_.Find(guid_)->doc, "entryFrame", 0);
  const double frames = Frames();
  if (static_cast<double>(f) <= entry || (frames > 0 && static_cast<double>(f) > frames)) return false;
  return doc_.SetNumber(guid_, "exitFrame", static_cast<double>(f));
}
bool SegmentEditor::SetEntryPreload(bool on) {
  if (!valid()) return false;
  if (BoolAt(doc_.Find(guid_)->doc, "entryPreload", false) == on) return true;  // D-177b
  if (!on) return doc_.Erase(guid_, "entryPreload");
  return doc_.Set(guid_, "entryPreload", Bool(true));
}
std::vector<TempoPointDesc> SegmentEditor::Tempo() const {
  std::vector<TempoPointDesc> out;
  if (!valid()) return out;
  const Value* t = doc_.Find(guid_)->doc.Find("tempo");
  if (t == nullptr || !t->IsArray()) return out;
  for (const Value& p : t->items) {
    TempoPointDesc d;
    d.tick = static_cast<std::uint64_t>(NumberAt(p, "tick", 0));
    d.bpm = NumberAt(p, "bpm", 120);
    d.beatsPerBar = static_cast<std::uint32_t>(NumberAt(p, "beatsPerBar", 4));
    d.beatUnit = static_cast<std::uint32_t>(NumberAt(p, "beatUnit", 4));
    out.push_back(d);
  }
  return out;
}
bool SegmentEditor::ValidTempo(const std::vector<TempoPointDesc>& t) const {
  for (std::size_t i = 0; i < t.size(); ++i) {
    if (!(t[i].bpm >= 1e-3 && t[i].bpm <= 1e5) || t[i].beatsPerBar == 0u || !ValidUnit(t[i].beatUnit)) return false;
    if (i > 0 && t[i].tick <= t[i - 1].tick) return false;
  }
  return true;
}
bool SegmentEditor::WriteTempo(const std::vector<TempoPointDesc>& t) {
  if (!valid() || !ValidTempo(t)) return false;
  Value a = json::MakeArray();
  for (const TempoPointDesc& p : t) {
    Value v = json::MakeObject();
    json::Add(v, "tick", Number(static_cast<double>(p.tick)));
    json::Add(v, "bpm", Number(p.bpm));
    json::Add(v, "beatsPerBar", Number(p.beatsPerBar));
    json::Add(v, "beatUnit", Number(p.beatUnit));
    a.items.push_back(v);
  }
  return doc_.Set(guid_, "tempo", a);
}
// D-179c: a change of time signature at point i must sit on a bar line of the signature
// before it (points that keep the signature may sit anywhere).
static bool SignatureOnBarLine(const std::vector<TempoPointDesc>& t, std::size_t i) {
  if (i == 0 || i >= t.size()) return true;
  if (t[i].beatsPerBar == t[i - 1].beatsPerBar && t[i].beatUnit == t[i - 1].beatUnit) return true;
  const TempoMap before(std::vector<TempoPointDesc>(t.begin(), t.begin() + static_cast<std::ptrdiff_t>(i)));
  const double pos = before.BarPosition(static_cast<double>(t[i].tick));
  return std::fabs(pos - std::round(pos)) < 1e-6;
}
bool SegmentEditor::AddTempo(const TempoPointDesc& p) {
  std::vector<TempoPointDesc> t = Tempo();
  std::size_t at = 0;
  while (at < t.size() && t[at].tick < p.tick) ++at;
  t.insert(t.begin() + static_cast<std::ptrdiff_t>(at), p);
  if (!SignatureOnBarLine(t, at) || !SignatureOnBarLine(t, at + 1)) return false;
  return WriteTempo(t);
}
bool SegmentEditor::SetTempo(std::uint32_t i, const TempoPointDesc& p) {
  std::vector<TempoPointDesc> t = Tempo();
  if (i >= t.size()) return false;
  if (t[i].tick == p.tick && t[i].bpm == p.bpm && t[i].beatsPerBar == p.beatsPerBar && t[i].beatUnit == p.beatUnit) {
    return true;  // D-177b
  }
  t[i] = p;
  if (!SignatureOnBarLine(t, i) || !SignatureOnBarLine(t, i + 1)) return false;
  return WriteTempo(t);
}
std::vector<std::string> SegmentEditor::Problems() const {
  std::vector<std::string> out;
  if (!valid()) return out;
  const std::vector<TempoPointDesc> t = Tempo();
  for (std::size_t i = 1; i < t.size(); ++i) {
    if (!SignatureOnBarLine(t, i)) {
      out.push_back(Idx("tempo", static_cast<std::uint32_t>(i)) + ": the time signature changes inside a bar");
    }
  }
  const Value* stems = doc_.Find(guid_)->doc.Find("stems");
  for (std::size_t i = 0; stems != nullptr && stems->IsArray() && i < stems->items.size(); ++i) {
    const std::string asset = StringAt(stems->items[i], "asset");
    if (StemChannels(asset) < 0) {
      out.push_back(Idx("stems", static_cast<std::uint32_t>(i)) + ": the material cannot be read (channels not checked)");
    }
  }
  return out;
}
int SegmentEditor::StemChannels(const std::string& assetRef) const {
  if (!valid()) return -1;
  const DocObject* a = doc_.Resolve("asset", assetRef.empty() ? StringAt(doc_.Find(guid_)->doc, "asset") : assetRef);
  if (a == nullptr) return -1;
  std::vector<std::uint8_t> b;
  buildtool::PcmData pcm;
  std::string err;
  if (!doc_.ReadBinaryFile(StringAt(a->doc, "file"), &b) || !buildtool::ReadWav(b, &pcm, &err)) return -1;
  return static_cast<int>(pcm.channels);
}
bool SegmentEditor::RemoveTempo(std::uint32_t i) {
  std::vector<TempoPointDesc> t = Tempo();
  if (i >= t.size()) return false;
  t.erase(t.begin() + static_cast<std::ptrdiff_t>(i));
  if (t.empty()) return doc_.Erase(guid_, "tempo");
  return WriteTempo(t);
}
std::vector<MarkerDesc> SegmentEditor::Markers() const {
  std::vector<MarkerDesc> out;
  if (!valid()) return out;
  const Value* m = doc_.Find(guid_)->doc.Find("markers");
  if (m == nullptr || !m->IsArray()) return out;
  for (const Value& v : m->items) {
    out.push_back(MarkerDesc{StringAt(v, "name"), static_cast<std::uint64_t>(NumberAt(v, "frame", 0)),
                             ParseEnum<MarkerKind>(StringAt(v, "kind", "user"), kMarkerKinds, MarkerKind::User)});
  }
  return out;
}
static Value MarkerJson(const MarkerDesc& m) {
  Value v = json::MakeObject();
  json::Add(v, "name", String(m.name));
  json::Add(v, "frame", Number(static_cast<double>(m.frame)));
  if (m.kind != MarkerKind::User) json::Add(v, "kind", String(kMarkerKinds[static_cast<int>(m.kind)]));
  return v;
}
bool SegmentEditor::AddMarker(const MarkerDesc& m) {
  if (!valid() || m.name.empty()) return false;
  const double frames = Frames();
  if (frames > 0 && static_cast<double>(m.frame) > frames) return false;
  const std::vector<MarkerDesc> have = Markers();
  for (const MarkerDesc& h : have) {
    if (h.name == m.name) return false;
  }
  return doc_.Insert(guid_, "markers", have.size(), MarkerJson(m));
}
bool SegmentEditor::SetMarker(std::uint32_t i, const MarkerDesc& m) {
  const std::vector<MarkerDesc> have = Markers();
  if (i >= have.size() || m.name.empty()) return false;
  if (have[i].name == m.name && have[i].frame == m.frame && have[i].kind == m.kind) return true;  // D-177b
  const double frames = Frames();
  if (frames > 0 && static_cast<double>(m.frame) > frames) return false;
  for (std::size_t k = 0; k < have.size(); ++k) {
    if (k != i && have[k].name == m.name) return false;
  }
  return doc_.Set(guid_, Idx("markers", i), MarkerJson(m));
}
bool SegmentEditor::RemoveMarker(std::uint32_t i) { return i < Markers().size() && doc_.Erase(guid_, Idx("markers", i)); }
bool SegmentEditor::AddStem(const StemDesc& s) {
  if (!valid() || s.channelCount == 0u) return false;
  const DocObject* mat = s.asset.empty() ? nullptr : doc_.Find(s.asset);
  if (!s.asset.empty() && (mat == nullptr || mat->kind() != "asset")) return false;
  // D-177d: inside the stem's material (its own file, else the segment's); unreadable: allowed,
  // reported by Problems().
  const int ch = StemChannels(mat != nullptr ? mat->guid : std::string());
  if (ch >= 0 && std::uint64_t{s.channelFirst} + s.channelCount > static_cast<std::uint64_t>(ch)) return false;
  Value v = json::MakeObject();
  json::Add(v, "channelFirst", Number(s.channelFirst));
  json::Add(v, "channelCount", Number(s.channelCount));
  if (mat != nullptr) json::Add(v, "asset", String(mat->guid));
  const Value* stems = doc_.Find(guid_)->doc.Find("stems");
  return doc_.Insert(guid_, "stems", stems != nullptr && stems->IsArray() ? stems->items.size() : 0u, v);
}
bool SegmentEditor::SetStem(std::uint32_t i, const StemDesc& s) {
  if (!valid() || s.channelCount == 0u) return false;
  const Value* cur = FindPath(doc_.Find(guid_)->doc, Idx("stems", i));
  if (cur == nullptr) return false;
  {
    const DocObject* ca = doc_.Resolve("asset", StringAt(*cur, "asset"));
    const std::string curAsset = ca != nullptr ? ca->guid : StringAt(*cur, "asset");
    if (NumberAt(*cur, "channelFirst", 0) == s.channelFirst && NumberAt(*cur, "channelCount", 2) == s.channelCount &&
        (s.asset.empty() || NormalizeGuid(s.asset) == curAsset)) {
      return true;  // D-177b
    }
  }
  Value v = *cur;  // keeps volumeCurve and unknown members
  const char* const* order = SpecMemberOrder("segment", "stems[0]");
  SetMember(v, "channelFirst", Number(s.channelFirst), order);
  SetMember(v, "channelCount", Number(s.channelCount), order);
  if (!s.asset.empty()) {
    const DocObject* a = doc_.Find(s.asset);
    if (a == nullptr || a->kind() != "asset") return false;
    SetMember(v, "asset", String(a->guid), order);
  }
  const int ch = StemChannels(StringAt(v, "asset"));
  if (ch >= 0 && std::uint64_t{s.channelFirst} + s.channelCount > static_cast<std::uint64_t>(ch)) return false;
  return doc_.Set(guid_, Idx("stems", i), v);
}
bool SegmentEditor::RemoveStem(std::uint32_t i) {
  return valid() && FindPath(doc_.Find(guid_)->doc, Idx("stems", i)) != nullptr && doc_.Erase(guid_, Idx("stems", i));
}
TempoMap SegmentEditor::Map() const { return TempoMap(Tempo()); }
double SegmentEditor::ExitTick() const {
  if (!valid()) return 0.0;
  const json::Value& d = doc_.Find(guid_)->doc;
  const TempoMap m = Map();
  const double exit = NumberAt(d, "exitFrame", 0);
  const double rate = SampleRate();
  if (exit > 0 && rate > 0) return m.TickAtFrame(exit - NumberAt(d, "entryFrame", 0), rate);
  return m.TickAtBar(NumberAt(d, "bars", 1));
}
bool SegmentEditor::ApplyGrid(double bpm, std::uint64_t offsetFrame, std::uint32_t beatsPerBar, std::uint32_t beatUnit) {
  if (!valid() || !(bpm >= 1e-3 && bpm <= 1e5) || beatsPerBar == 0u || !ValidUnit(beatUnit)) return false;
  const double rate = SampleRate(), frames = Frames();
  if (rate <= 0.0 || static_cast<double>(offsetFrame) >= frames) return false;
  const double barSeconds = 60.0 / bpm * 4.0 / static_cast<double>(beatUnit) * static_cast<double>(beatsPerBar);
  const double bars = std::max(1.0, std::floor((frames - static_cast<double>(offsetFrame)) / rate / barSeconds + 1e-9));
  doc_.BeginGroup();
  const bool ok = WriteTempo({TempoPointDesc{0, bpm, beatsPerBar, beatUnit}}) || !Tempo().empty();
  if (offsetFrame > 0) {
    doc_.SetNumber(guid_, "entryFrame", static_cast<double>(offsetFrame));
  } else if (doc_.Find(guid_)->doc.Find("entryFrame") != nullptr) {
    doc_.Erase(guid_, "entryFrame");
  }
  doc_.SetNumber(guid_, "bars", bars);
  doc_.EndGroup();
  return ok;
}

// --- PlaylistEditor --------------------------------------------------------------------------------
PlaylistEditor::PlaylistEditor(ProjectDocument& doc, std::string guid) : doc_(doc), guid_(NormalizeGuid(guid)) {}
bool PlaylistEditor::valid() const {
  const DocObject* o = doc_.Find(guid_);
  return o != nullptr && o->kind() == "playlist";
}
bool PlaylistEditor::SetMode(PlaylistMode m) {
  return valid() && doc_.SetDefaulted(guid_, "mode", String(kModes[static_cast<int>(m)]), String("sequential"));
}
bool PlaylistEditor::SetLoopCount(std::uint32_t n) { return valid() && doc_.SetDefaulted(guid_, "loopCount", Number(n), Number(0)); }
std::uint32_t PlaylistEditor::EntryCount() const {
  const Value* e = valid() ? doc_.Find(guid_)->doc.Find("entries") : nullptr;
  return e != nullptr && e->IsArray() ? static_cast<std::uint32_t>(e->items.size()) : 0u;
}
static bool EntryJson(const ProjectDocument& doc, const std::string& seg, double w, std::uint32_t repeat, Value* out) {
  const DocObject* s = doc.Find(seg);
  if (s == nullptr || s->kind() != "segment" || !std::isfinite(w) || w < 0 || repeat == 0u) return false;
  *out = json::MakeObject();
  json::Add(*out, "segment", String(s->guid));
  if (w != 1.0) json::Add(*out, "weight", Number(w));
  if (repeat != 1u) json::Add(*out, "repeat", Number(repeat));
  return true;
}
bool PlaylistEditor::AddEntry(const std::string& seg, double w, std::uint32_t repeat) {
  Value v;
  return valid() && EntryJson(doc_, seg, w, repeat, &v) && doc_.Insert(guid_, "entries", EntryCount(), v);
}
bool PlaylistEditor::SetEntry(std::uint32_t i, const std::string& seg, double w, std::uint32_t repeat) {
  Value v;
  if (i >= EntryCount() || !EntryJson(doc_, seg, w, repeat, &v)) return false;
  const Value& cur = *FindPath(doc_.Find(guid_)->doc, Idx("entries", i));
  const DocObject* cs = doc_.Resolve("segment", StringAt(cur, "segment"));
  if (cs != nullptr && cs->guid == NormalizeGuid(seg) && NumberAt(cur, "weight", 1) == w && NumberAt(cur, "repeat", 1) == repeat) {
    return true;  // D-177b
  }
  return doc_.Set(guid_, Idx("entries", i), v);
}
bool PlaylistEditor::RemoveEntry(std::uint32_t i) { return i < EntryCount() && doc_.Erase(guid_, Idx("entries", i)); }
bool PlaylistEditor::MoveEntry(std::uint32_t from, std::uint32_t to) { return doc_.MoveElement(guid_, "entries", from, to); }

// --- MusicEditor -------------------------------------------------------------------------------------
MusicEditor::MusicEditor(ProjectDocument& doc, std::string eventGuid) : doc_(doc), guid_(NormalizeGuid(eventGuid)) {}
bool MusicEditor::valid() const {
  const DocObject* o = doc_.Find(guid_);
  return o != nullptr && o->kind() == "event";
}
const Value* MusicEditor::Music() const {
  const DocObject* o = doc_.Find(guid_);
  return o != nullptr ? o->doc.Find("music") : nullptr;
}
std::vector<std::string> MusicEditor::States() const {
  std::vector<std::string> out;
  const Value* m = Music();
  const Value* s = m != nullptr ? m->Find("states") : nullptr;
  if (s == nullptr || !s->IsArray()) return out;
  for (const Value& v : s->items) out.push_back(StringAt(v, "name"));
  return out;
}
static int IndexOf(const std::vector<std::string>& v, const std::string& s) {
  for (std::size_t i = 0; i < v.size(); ++i) {
    if (v[i] == s) return static_cast<int>(i);
  }
  return -1;
}
bool MusicEditor::AddState(const std::string& name, const std::string& playlistGuid) {
  const DocObject* pl = doc_.Find(playlistGuid);
  if (!valid() || name.empty() || name == "*" || IndexOf(States(), name) >= 0 || pl == nullptr || pl->kind() != "playlist") {
    return false;
  }
  Value v = json::MakeObject();
  json::Add(v, "name", String(name));
  json::Add(v, "playlist", String(pl->guid));
  return doc_.Insert(guid_, "music.states", States().size(), v);
}
bool MusicEditor::RemoveState(const std::string& name) {
  const int i = IndexOf(States(), name);
  if (i < 0) return false;
  for (const TransitionRule& r : Rules()) {
    if (r.from == name || r.to == name) return false;
  }
  if (StringAt(*Music(), "initialState") == name) return false;
  return doc_.Erase(guid_, Idx("music.states", static_cast<std::uint32_t>(i)));
}
bool MusicEditor::RenameState(const std::string& from, const std::string& to) {
  const std::vector<std::string> st = States();
  const int i = IndexOf(st, from);
  if (i >= 0 && from == to) return true;  // D-177b
  if (i < 0 || to.empty() || to == "*" || IndexOf(st, to) >= 0) return false;
  return doc_.Modify(guid_, [&](Value& e) {
    Value* m = FindPath(e, "music");
    if (m == nullptr) return false;
    if (Value* n = FindPath(*m, Idx("states", static_cast<std::uint32_t>(i), ".name"))) *n = String(to);
    if (Value* init = FindPath(*m, "initialState"); init != nullptr && init->IsString() && init->text == from) *init = String(to);
    if (Value* trs = FindPath(*m, "transitions"); trs != nullptr && trs->IsArray()) {
      for (Value& t : trs->items) {
        for (const char* k : {"from", "to"}) {
          if (Value* x = FindPath(t, k); x != nullptr && x->IsString() && x->text == from) *x = String(to);
        }
      }
    }
    return true;
  });
}
bool MusicEditor::SetStatePlaylist(const std::string& name, const std::string& playlistGuid) {
  const int i = IndexOf(States(), name);
  const DocObject* pl = doc_.Find(playlistGuid);
  if (i < 0 || pl == nullptr || pl->kind() != "playlist") return false;
  const std::string path = Idx("music.states", static_cast<std::uint32_t>(i), ".playlist");
  const DocObject* cur = doc_.Resolve("playlist", StringAt(doc_.Find(guid_)->doc, path));
  if (cur != nullptr && cur->guid == pl->guid) return true;  // D-177b
  return doc_.Set(guid_, path, String(pl->guid));
}
bool MusicEditor::SetInitialState(const std::string& name) {
  return IndexOf(States(), name) >= 0 && doc_.Set(guid_, "music.initialState", String(name));
}

std::vector<TransitionRule> MusicEditor::Rules() const {
  std::vector<TransitionRule> out;
  const Value* m = Music();
  const Value* t = m != nullptr ? m->Find("transitions") : nullptr;
  if (t == nullptr || !t->IsArray()) return out;
  for (const Value& v : t->items) {
    TransitionRule r;
    r.from = StringAt(v, "from", "*");
    r.to = StringAt(v, "to", "*");
    r.timing = ParseEnum<Timing>(StringAt(v, "timing", "nextBar"), kTimingNames, Timing::NextBar);
    r.timingCount = static_cast<std::uint32_t>(NumberAt(v, "timingCount", 0));
    r.fadeOutBeats = NumberAt(v, "fadeOutBeats", 0);
    r.fadeInBeats = NumberAt(v, "fadeInBeats", 0);
    const DocObject* b = doc_.Resolve("segment", StringAt(v, "bridge"));
    r.bridge = b != nullptr ? b->guid : StringAt(v, "bridge");
    r.start = ParseEnum<StartPosition>(StringAt(v, "start", "head"), kStartNames, StartPosition::Head);
    r.startMarker = StringAt(v, "startMarker");
    r.interruptAfterBridge = StringAt(v, "interrupt") == "afterBridge";
    r.late = ParseEnum<LateRule>(StringAt(v, "late", "nextGrid"), kLateNames, LateRule::NextGrid);
    r.prepare = BoolAt(v, "prepare", false);
    out.push_back(r);
  }
  return out;
}
bool MusicEditor::ValidRule(const TransitionRule& r) const {
  const std::vector<std::string> st = States();
  for (const std::string* s : {&r.from, &r.to}) {
    if (*s != "*" && IndexOf(st, *s) < 0) return false;
  }
  if (!std::isfinite(r.fadeOutBeats) || !std::isfinite(r.fadeInBeats) || r.fadeOutBeats < 0 || r.fadeInBeats < 0) return false;
  if (!r.bridge.empty()) {
    const DocObject* b = doc_.Find(r.bridge);
    if (b == nullptr || b->kind() != "segment") return false;
  }
  if (r.start == StartPosition::Marker && r.startMarker.empty()) return false;
  if ((r.timing == Timing::BarsAfter || r.timing == Timing::Grid) && r.timingCount == 0u) return false;
  return true;
}
Value MusicEditor::RuleJson(const TransitionRule& r, const Value* keep) const {
  Value v = keep != nullptr && keep->IsObject() ? *keep : json::MakeObject();
  const char* const* order = SpecMemberOrder("event", "music.transitions[0]");
  auto set = [&](const char* k, Value x, bool present) {
    if (present) {
      SetMember(v, k, std::move(x), order);
    } else {
      for (std::size_t i = 0; i < v.members.size(); ++i) {
        if (v.members[i].first == k) {
          v.members.erase(v.members.begin() + static_cast<std::ptrdiff_t>(i));
          break;
        }
      }
    }
  };
  set("from", String(r.from), true);
  set("to", String(r.to), true);
  set("timing", String(kTimingNames[static_cast<int>(r.timing)]), true);
  set("timingCount", Number(r.timingCount), r.timingCount != 0u);
  set("fadeOutBeats", Number(r.fadeOutBeats), r.fadeOutBeats != 0.0);
  set("fadeInBeats", Number(r.fadeInBeats), r.fadeInBeats != 0.0);
  set("bridge", String(NormalizeGuid(r.bridge)), !r.bridge.empty());
  set("start", String(kStartNames[static_cast<int>(r.start)]), r.start != StartPosition::Head);
  set("startMarker", String(r.startMarker), !r.startMarker.empty());
  set("interrupt", String("afterBridge"), r.interruptAfterBridge);
  set("late", String(kLateNames[static_cast<int>(r.late)]), r.late != LateRule::NextGrid);
  set("prepare", Bool(true), r.prepare);
  return v;
}
bool MusicEditor::AddRule(const TransitionRule& r, std::uint32_t at) {
  if (!valid() || !ValidRule(r) || at > Rules().size()) return false;
  return doc_.Insert(guid_, "music.transitions", at, RuleJson(r, nullptr));
}
bool MusicEditor::SetRule(std::uint32_t i, const TransitionRule& r) {
  const Value* m = Music();
  const Value* cur = m != nullptr ? FindPath(*m, Idx("transitions", i)) : nullptr;
  if (cur == nullptr || !ValidRule(r)) return false;
  {
    const TransitionRule c = Rules()[i];
    if (c.from == r.from && c.to == r.to && c.timing == r.timing && c.timingCount == r.timingCount &&
        c.fadeOutBeats == r.fadeOutBeats && c.fadeInBeats == r.fadeInBeats && c.bridge == NormalizeGuid(r.bridge) &&
        c.start == r.start && c.startMarker == r.startMarker && c.interruptAfterBridge == r.interruptAfterBridge &&
        c.late == r.late && c.prepare == r.prepare) {
      return true;  // D-177b: explicit defaults stay as written
    }
  }
  return doc_.Set(guid_, Idx("music.transitions", i), RuleJson(r, cur));
}
bool MusicEditor::RemoveRule(std::uint32_t i) { return i < Rules().size() && doc_.Erase(guid_, Idx("music.transitions", i)); }
bool MusicEditor::MoveRule(std::uint32_t from, std::uint32_t to) { return doc_.MoveElement(guid_, "music.transitions", from, to); }
bool MusicEditor::AutoSort(std::string* why) {
  const Value* m = Music();
  const Value* t = m != nullptr ? m->Find("transitions") : nullptr;
  if (t == nullptr || !t->IsArray()) {
    if (why != nullptr) *why = "no transition rules";
    return false;
  }
  // D-179e: sorting could raise a row nothing reaches and change which rule matches.
  if (const auto un = Unreachable(); !un.empty()) {
    if (why != nullptr) {
      *why = "transitions[" + std::to_string(un[0].first) + "] can never match (covered by transitions[" +
             std::to_string(un[0].second) + "]): fix or remove the unreachable rows first";
    }
    return false;
  }
  std::vector<Value> rows = t->items;
  auto score = [](const Value& v) {
    return (StringAt(v, "from", "*") != "*" ? 1 : 0) + (StringAt(v, "to", "*") != "*" ? 1 : 0);
  };
  std::stable_sort(rows.begin(), rows.end(), [&](const Value& a, const Value& b) { return score(a) > score(b); });
  Value arr = json::MakeArray();
  arr.items = std::move(rows);
  return doc_.Set(guid_, "music.transitions", arr);
}
std::vector<std::pair<std::uint32_t, std::uint32_t>> MusicEditor::Unreachable() const {
  std::vector<std::pair<std::uint32_t, std::uint32_t>> out;
  const std::vector<TransitionRule> rs = Rules();
  for (std::uint32_t j = 0; j < rs.size(); ++j) {
    for (std::uint32_t i = 0; i < j; ++i) {
      if ((rs[i].from == "*" || rs[i].from == rs[j].from) && (rs[i].to == "*" || rs[i].to == rs[j].to)) {
        out.emplace_back(j, i);
        break;
      }
    }
  }
  return out;
}
int MusicEditor::Match(const std::string& from, const std::string& to) const {
  const std::vector<TransitionRule> rs = Rules();
  for (std::size_t i = 0; i < rs.size(); ++i) {
    // The runtime compares ids (snd_hash_id); "*" is 0.
    const Id f = rs[i].from == "*" ? 0u : HashId(rs[i].from);
    const Id t = rs[i].to == "*" ? 0u : HashId(rs[i].to);
    if ((f != 0u && f != HashId(from)) || (t != 0u && t != HashId(to))) continue;
    return static_cast<int>(i);
  }
  return -1;
}

static Value StingerJson(const ProjectDocument& doc, const StingerDesc& s, bool* ok) {
  const DocObject* e = doc.Find(s.event);
  *ok = !s.name.empty() && e != nullptr && e->kind() == "event";
  Value v = json::MakeObject();
  if (!*ok) return v;
  json::Add(v, "name", String(s.name));
  json::Add(v, "event", String(e->guid));
  json::Add(v, "timing", String(kTimingNames[static_cast<int>(s.timing)]));
  if (s.timingCount != 0u) json::Add(v, "timingCount", Number(s.timingCount));
  if (s.stack) json::Add(v, "overlap", String("stack"));
  return v;
}
bool MusicEditor::AddStinger(const StingerDesc& s) {
  bool ok = false;
  const Value v = StingerJson(doc_, s, &ok);
  const Value* m = Music();
  const Value* st = m != nullptr ? m->Find("stingers") : nullptr;
  const std::size_t n = st != nullptr && st->IsArray() ? st->items.size() : 0u;
  for (std::size_t i = 0; i < n; ++i) {
    if (StringAt(st->items[i], "name") == s.name) return false;
  }
  return ok && valid() && doc_.Insert(guid_, "music.stingers", n, v);
}
bool MusicEditor::SetStinger(std::uint32_t i, const StingerDesc& s) {
  bool ok = false;
  const Value v = StingerJson(doc_, s, &ok);
  const Value* m = Music();
  const Value* cur = m != nullptr ? FindPath(*m, Idx("stingers", i)) : nullptr;
  if (!ok || cur == nullptr) return false;
  const DocObject* ce = doc_.Resolve("event", StringAt(*cur, "event"));
  if (StringAt(*cur, "name") == s.name && ce != nullptr && ce->guid == NormalizeGuid(s.event) &&
      StringAt(*cur, "timing", "nextBeat") == kTimingNames[static_cast<int>(s.timing)] &&
      NumberAt(*cur, "timingCount", 0) == s.timingCount && (StringAt(*cur, "overlap", "replace") == "stack") == s.stack) {
    return true;  // D-177b
  }
  return doc_.Set(guid_, Idx("music.stingers", i), v);
}
bool MusicEditor::RemoveStinger(std::uint32_t i) {
  const Value* m = Music();
  return m != nullptr && FindPath(*m, Idx("stingers", i)) != nullptr && doc_.Erase(guid_, Idx("music.stingers", i));
}

std::string MusicEditor::FirstSegmentOf(const std::string& state) const {
  const Value* m = Music();
  const Value* sts = m != nullptr ? m->Find("states") : nullptr;
  if (sts == nullptr || !sts->IsArray()) return std::string();
  for (const Value& s : sts->items) {
    if (StringAt(s, "name") != state) continue;
    const DocObject* pl = doc_.Resolve("playlist", StringAt(s, "playlist"));
    if (pl == nullptr) return std::string();
    const DocObject* seg = doc_.Resolve("segment", StringAt(pl->doc, "entries[0].segment"));
    return seg != nullptr ? seg->guid : std::string();
  }
  return std::string();
}

namespace {
// Prime time of a material started at `start` (see the header).
void PrimeOf(const ProjectDocument& doc, const DocObject* asset, bool head, bool prepare, bool entryPreload,
             const MusicSettings& s, LookaheadEntry* e) {
  if (asset == nullptr) {
    e->reason = "none";
    return;
  }
  e->assetGuid = asset->guid;
  const std::string form = StringAt(asset->doc, "loadForm", "pcm");
  if (form != "stream") {
    e->reason = "memory";
  } else if (prepare) {
    e->reason = "prepare";
  } else if (NumberAt(asset->doc, "preloadMs", 100) <= 0) {
    e->reason = "stream";
    e->primeMs = s.streamPrimeMs;
  } else if (head) {
    e->reason = "preload";
  } else if (entryPreload) {
    e->reason = "entryPreload";
  } else {
    e->reason = "stream";
    e->primeMs = s.streamPrimeMs;
  }
  (void)doc;
}
}  // namespace

std::vector<LookaheadEntry> MusicEditor::MinimumLookahead(const MusicSettings& s) const {
  std::vector<LookaheadEntry> out;
  const double blockMs = 1000.0 * s.blockFrames / (s.sampleRate == 0u ? 48000.0 : s.sampleRate);
  const std::vector<TransitionRule> rules = Rules();
  for (const std::string& st : States()) {
    // The worst case over the rules that lead to the state (and the default rule).
    LookaheadEntry best;
    best.kind = "state";
    best.name = st;
    bool any = false;
    const DocObject* seg = doc_.Find(FirstSegmentOf(st));
    const DocObject* asset = seg != nullptr ? doc_.Resolve("asset", StringAt(seg->doc, "asset")) : nullptr;
    std::vector<TransitionRule> to;
    for (const TransitionRule& r : rules) {
      if (r.to == "*" || r.to == st) to.push_back(r);
    }
    if (to.empty()) to.push_back(TransitionRule{});
    for (const TransitionRule& r : to) {
      LookaheadEntry e = best;
      PrimeOf(doc_, asset, r.start == StartPosition::Head, r.prepare, seg != nullptr && BoolAt(seg->doc, "entryPreload", false),
              s, &e);
      if (!any || e.primeMs > best.primeMs) best = e;
      any = true;
    }
    best.lookaheadMs = blockMs + best.primeMs;
    out.push_back(best);
  }
  std::set<std::string> bridges;
  for (const TransitionRule& r : rules) {
    const DocObject* seg = doc_.Find(r.bridge);
    if (seg == nullptr || !bridges.insert(seg->guid).second) continue;
    LookaheadEntry e;
    e.kind = "bridge";
    e.name = seg->name();
    PrimeOf(doc_, doc_.Resolve("asset", StringAt(seg->doc, "asset")), true, r.prepare, false, s, &e);
    e.lookaheadMs = blockMs + e.primeMs;
    out.push_back(e);
  }
  const Value* m = Music();
  const Value* sts = m != nullptr ? m->Find("stingers") : nullptr;
  for (const Value& sv : (sts != nullptr && sts->IsArray()) ? sts->items : std::vector<Value>{}) {
    LookaheadEntry e;
    e.kind = "stinger";
    e.name = StringAt(sv, "name");
    e.reason = "none";
    const DocObject* ev = doc_.Resolve("event", StringAt(sv, "event"));
    // The stinger event's clips: the slowest asset.
    if (ev != nullptr) {
      if (const Value* tr = ev->doc.Find("tracks"); tr != nullptr && tr->IsArray()) {
        for (const Value& t : tr->items) {
          const Value* clips = t.Find("clips");
          for (const Value& c : (clips != nullptr && clips->IsArray()) ? clips->items : std::vector<Value>{}) {
            LookaheadEntry x = e;
            PrimeOf(doc_, doc_.Resolve("asset", StringAt(c, "asset")), true, false, false, s, &x);
            if (x.reason != "none" && (e.reason == "none" || x.primeMs > e.primeMs)) e = x;
          }
        }
      }
    }
    e.lookaheadMs = blockMs + e.primeMs;
    out.push_back(e);
  }
  return out;
}

TransitionPreview MusicEditor::Preview(const std::string& from, const std::string& to, double requestSeconds,
                                       const MusicSettings& s) const {
  TransitionPreview p;
  const std::vector<std::string> st = States();
  if (IndexOf(st, from) < 0 || IndexOf(st, to) < 0) return p;
  p.valid = true;
  p.rule = Match(from, to);
  TransitionRule r;
  if (p.rule >= 0) {
    r = Rules()[static_cast<std::size_t>(p.rule)];
  } else {  // the runtime's built-in default (music/src/rules.hpp)
    r.timing = Timing::NextBar;
    r.fadeOutBeats = 1.0;
    r.fadeInBeats = 1.0;
  }
  const std::string srcGuid = FirstSegmentOf(from);
  const std::string dstGuid = FirstSegmentOf(to);
  p.targetSegment = dstGuid;
  SegmentEditor src(doc_, srcGuid), dst(doc_, dstGuid);
  const TempoMap map = src.valid() ? src.Map() : TempoMap();
  const TempoMap dmap = dst.valid() ? dst.Map() : TempoMap();
  const double srcRate = src.valid() && src.SampleRate() > 0 ? src.SampleRate() : 48000.0;
  p.requestSeconds = requestSeconds;
  // D-184: the runtime takes the request at the start of a block (blocks counted from the music's
  // start). A request is never applied earlier than asked: the first block start at or after the
  // request, and at least one block after the start (the music starts in the From state).
  const double applyRate = s.sampleRate == 0u ? 48000.0 : static_cast<double>(s.sampleRate);
  const double applyFrames = s.blockFrames == 0u ? 256.0 : static_cast<double>(s.blockFrames);
  double blocks = std::ceil(requestSeconds * applyRate / applyFrames - 1e-9);
  if (!(blocks >= 1.0)) blocks = 1.0;
  p.applySeconds = blocks * applyFrames / applyRate;
  p.requestTick = map.TickAtSeconds(requestSeconds);
  // Lookahead of this target.
  LookaheadEntry le;
  const DocObject* dseg = doc_.Find(dstGuid);
  PrimeOf(doc_, dseg != nullptr ? doc_.Resolve("asset", StringAt(dseg->doc, "asset")) : nullptr, r.start == StartPosition::Head,
          r.prepare, dseg != nullptr && BoolAt(dseg->doc, "entryPreload", false), s, &le);
  const double blockMs = 1000.0 * s.blockFrames / (s.sampleRate == 0u ? 48000.0 : s.sampleRate);
  p.lookaheadMs = blockMs + le.primeMs;
  // Timing (from where the runtime takes the request).
  const double now = map.TickAtSeconds(p.applySeconds);
  const double exitTick = src.valid() ? src.ExitTick() : map.TickAtBar(1);
  auto nextMarker = [&](double x, double* out) {
    double best = -1;
    const Id want = r.timingCount;
    for (const MarkerDesc& mk : src.Markers()) {
      if (mk.kind != MarkerKind::User || (want != 0u && HashId(mk.name) != want)) continue;
      const double entry = src.valid() ? NumberAt(doc_.Find(srcGuid)->doc, "entryFrame", 0) : 0.0;
      const double t = map.TickAtFrame(static_cast<double>(mk.frame) - entry, srcRate);
      if (t >= x - kEps && (best < 0 || t < best)) best = t;
    }
    if (best < 0) return false;
    *out = best;
    return true;
  };
  auto timing = [&](double x) -> double {
    double out = x;
    switch (r.timing) {
      case Timing::Immediate: return x;
      case Timing::NextBeat: return map.NextBeat(x);
      case Timing::NextBar: return map.NextBar(x);
      case Timing::BarsAfter: return map.BarsAfter(x, r.timingCount);
      case Timing::Grid: return map.NextGrid(x, r.timingCount);
      case Timing::NextMarker:
        if (nextMarker(x, &out)) return out;
        return exitTick >= x ? exitTick : map.NextBar(x);
      case Timing::SegmentEnd: return exitTick >= x ? exitTick : map.NextBar(x);
    }
    return map.NextBar(x);
  };
  double fire = timing(now);
  // The runtime's lead check (music transition.cpp HandleSetState): the target material must be
  // primed by the fire time, counted from the block the request is applied in (no extra block:
  // lookaheadMs above is the display value, one block + prime, of 8.2).
  const double lookSeconds = le.primeMs / 1000.0;
  const double halfSample = 0.5 / applyRate;
  if (map.SecondsAtTick(fire) < p.applySeconds + lookSeconds - halfSample) {
    p.lateApplied = true;
    switch (r.late) {
      case LateRule::NextGrid: {
        const double reach = map.TickAtSeconds(p.applySeconds + lookSeconds);
        double x = fire;
        for (int guard = 0; guard < 10000 && x < reach - kEps; ++guard) {
          const double nx = r.timing == Timing::Immediate ? reach : timing(x + 0.5);
          if (!(nx > x)) {
            x = map.NextBar(x + 0.5);
          } else {
            x = nx;
          }
        }
        fire = x;
        break;
      }
      case LateRule::StartLate: fire = map.TickAtSeconds(p.applySeconds + lookSeconds); break;
      case LateRule::Cancel: p.cancelled = true; break;
    }
  }
  p.fireTick = fire;
  p.fireSeconds = map.SecondsAtTick(fire);
  p.fireAt = map.Musical(fire);
  // Fades: beats of the source / target tempo (a beat = the signature's beat unit).
  const std::uint32_t sp = map.PointAt(fire);
  p.fadeOutSeconds = r.fadeOutBeats * map.BeatTicks(sp) / (map.BpmAt(fire) / 60.0 * kTicksPerQuarter);
  p.fadeInSeconds = r.fadeInBeats * dmap.BeatTicks(0) / (dmap.points()[0].bpm / 60.0 * kTicksPerQuarter);
  // Bridge.
  p.bridge = r.bridge;
  SegmentEditor br(doc_, r.bridge);
  if (br.valid()) p.bridgeSeconds = br.Map().SecondsAtTick(br.ExitTick());
  p.targetStartSeconds = p.fireSeconds + p.bridgeSeconds;
  // Start position in the target segment.
  switch (r.start) {
    case StartPosition::Head:
    case StartPosition::LastPosition: p.targetStartTick = 0.0; break;
    case StartPosition::SameBar: {
      const MusicalPosition m = p.fireAt;
      const double bars = dst.valid() ? NumberAt(doc_.Find(dstGuid)->doc, "bars", 1) : 1.0;
      MusicalPosition t;
      t.bar = static_cast<std::int32_t>(std::fmod(static_cast<double>(m.bar), bars < 1 ? 1 : bars));
      const TempoPointDesc& a = map.points()[map.PointAt(fire)];
      const TempoPointDesc& b = dmap.points()[0];
      if (a.beatsPerBar == b.beatsPerBar && a.beatUnit == b.beatUnit) {
        t.beat = m.beat;
        t.tick = m.tick;
      }
      p.targetStartTick = dmap.TickOfMusical(t);
      break;
    }
    case StartPosition::Marker: {
      for (const MarkerDesc& mk : dst.Markers()) {
        if (mk.name == r.startMarker) {
          const double rate = dst.SampleRate() > 0 ? dst.SampleRate() : 48000.0;
          p.targetStartTick = dmap.TickAtFrame(static_cast<double>(mk.frame) - NumberAt(doc_.Find(dstGuid)->doc, "entryFrame", 0), rate);
        }
      }
      break;
    }
  }
  return p;
}

}  // namespace snd::tool
