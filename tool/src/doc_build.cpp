// Checking, building and diffing the document (doc_build.hpp).
#include "snd/tool/doc_build.hpp"

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <map>
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

void Classify(CellRef* c) {
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
  c->liveWritable = c->liveWritable && c->addressable;
}

// --- field table --------------------------------------------------------------------------
std::vector<FieldColumn> MakeFields() {
  return {
      {"event", "sequence", ch::EVNT, co::EVNT_sequenceKind},
      {"event", "priority", ch::EVNT, co::EVNT_priority},
      {"event", "maxInstances", ch::EVNT, co::EVNT_maxInstances},
      {"event", "retriggerMs", ch::EVNT, co::EVNT_retriggerMs},
      {"event", "fadeOutMs", ch::EVNT, co::EVNT_fadeOutMs},
      {"event", "comboWindowMs", ch::EVNT, co::EVNT_comboWindowMs},
      {"event", "maxDistance", ch::EVNT, co::EVNT_maxDistance},
      {"event", "cone.innerDeg", ch::EVNT, co::EVNT_coneInnerDeg},
      {"event", "cone.outerDeg", ch::EVNT, co::EVNT_coneOuterDeg},
      {"event", "cone.outerDb", ch::EVNT, co::EVNT_coneOuterDb},
      {"event", "virtualReturn", ch::EVNT, co::EVNT_virtualReturn},
      {"event", "is3D", ch::EVNT, co::EVNT_flags},
      {"event", "ignorePause", ch::EVNT, co::EVNT_flags},
      {"event", "stinger", ch::EVNT, co::EVNT_flags},
      {"event", "tracks[#].flags", ch::TRAK, co::TRAK_flags},
      {"event", "tracks[#].layerParameter", ch::TRAK, co::TRAK_layerParam},
      {"event", "tracks[#].clips[#].startMs", ch::CLIP, co::CLIP_startMs},
      {"event", "tracks[#].clips[#].weight", ch::CLIP, co::CLIP_weight},
      {"event", "tracks[#].clips[#].volumeDb", ch::CLIP, co::CLIP_volumeDb},
      {"event", "tracks[#].clips[#].pitchCents", ch::CLIP, co::CLIP_pitchCents},
      {"event", "tracks[#].clips[#].random.volumeDb", ch::CLIP, co::CLIP_randVolumeDb},
      {"event", "tracks[#].clips[#].random.pitchCents", ch::CLIP, co::CLIP_randPitchCents},
      {"event", "tracks[#].clips[#].random.pan", ch::CLIP, co::CLIP_randPan},
      {"event", "tracks[#].clips[#].random.startMs", ch::CLIP, co::CLIP_randStartMs},
      {"event", "tracks[#].clips[#].random.filterHz", ch::CLIP, co::CLIP_randFilterHz},
      {"event", "tracks[#].clips[#].switchValue", ch::CLIP, co::CLIP_switchValue},
      {"event", "tracks[#].clips[#].layer[0]", ch::CLIP, co::CLIP_layerMin},
      {"event", "tracks[#].clips[#].layer[1]", ch::CLIP, co::CLIP_layerMax},
      {"event", "tracks[#].clips[#].fadeInMs", ch::CLIP, co::CLIP_fadeInMs},
      {"event", "tracks[#].clips[#].fadeOutMs", ch::CLIP, co::CLIP_fadeOutMs},
      {"event", "tracks[#].clips[#].loopCount", ch::CLIP, co::CLIP_loopCount},
      {"event", "tracks[#].actions[#].timeMs", ch::ACTN, co::ACTN_timeMs},
      {"event", "tracks[#].actions[#].value", ch::ACTN, co::ACTN_value},
      {"event", "tracks[#].actions[#].target", ch::ACTN, co::ACTN_target},
      {"event", "curves[#].points[#][0]", ch::CPNT, co::CPNT_x},
      {"event", "curves[#].points[#][1]", ch::CPNT, co::CPNT_y},
      {"event", "curves[#].points[#][2]", ch::CPNT, co::CPNT_shape},
      {"bus", "volumeDb", ch::BUSS, co::BUSS_volumeDb},
      {"bus", "channels", ch::BUSS, co::BUSS_channels},
      {"bus", "lodPriority", ch::BUSS, co::BUSS_lodPriority},
      {"bus", "sends[#].levelDb", ch::SEND, co::SEND_levelDb},
      {"bus", "sends[#].preFader", ch::SEND, co::SEND_preFader},
      {"bus", "effects[#].params[#]", ch::EFPV, co::EFPV_value},
      {"bus", "effects[#].flags", ch::EFFX, co::EFFX_flags},
      {"category", "volumeDb", ch::CATG, co::CATG_volumeDb},
      {"category", "ignorePause", ch::CATG, co::CATG_flags},
      {"category", "muteOnFocusLoss", ch::CATG, co::CATG_flags},
      {"parameter", "min", ch::PARM, co::PARM_minValue},
      {"parameter", "max", ch::PARM, co::PARM_maxValue},
      {"parameter", "default", ch::PARM, co::PARM_defaultValue},
      {"parameter", "rampRate", ch::PARM, co::PARM_rampRate},
      {"parameter", "scope", ch::PARM, co::PARM_scopeMask},
      {"parameter", "kind", ch::PARM, co::PARM_kind},
      {"snapshot", "values[#].value", ch::SNPV, co::SNPV_value},
      {"voiceLimitGroup", "maxVoices", ch::VLGR, co::VLGR_maxVoices},
      {"voiceLimitGroup", "steal", ch::VLGR, co::VLGR_stealMode},
      {"ducking", "amountDb", ch::DUCK, co::DUCK_amountDb},
      {"ducking", "attackMs", ch::DUCK, co::DUCK_attackMs},
      {"ducking", "releaseMs", ch::DUCK, co::DUCK_releaseMs},
      {"ducking", "thresholdDb", ch::DUCK, co::DUCK_thresholdDb},
      {"playlist", "mode", ch::MPLS, co::MPLS_mode},
      {"playlist", "loopCount", ch::MPLS, co::MPLS_loopCount},
      {"playlist", "entries[#].weight", ch::MPLE, co::MPLE_weight},
      {"playlist", "entries[#].repeat", ch::MPLE, co::MPLE_repeat},
  };
}

// Matches "tracks[2].clips[0].startMs" against "tracks[#].clips[#].startMs"; indices out.
bool MatchPattern(const std::string& path, const char* pattern, std::vector<std::uint32_t>* idx) {
  idx->clear();
  std::size_t i = 0;
  const char* p = pattern;
  while (*p != '\0') {
    if (p[0] == '[' && p[1] == '#' && p[2] == ']') {
      if (i >= path.size() || path[i] != '[') return false;
      std::size_t e = path.find(']', i);
      if (e == std::string::npos || e == i + 1) return false;
      std::uint32_t v = 0;
      for (std::size_t k = i + 1; k < e; ++k) {
        if (path[k] < '0' || path[k] > '9') return false;
        v = v * 10u + static_cast<std::uint32_t>(path[k] - '0');
      }
      idx->push_back(v);
      i = e + 1;
      p += 3;
      continue;
    }
    if (i >= path.size() || path[i] != *p) return false;
    ++i;
    ++p;
  }
  return i == path.size();
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
BankImage::BankImage(const BankImage& o)
    : name(o.name), language(o.language), file(o.file), bankId(o.bankId), contentHash(o.contentHash), bytes(o.bytes) {}
BankImage& BankImage::operator=(const BankImage& o) {
  if (this != &o) {
    name = o.name;
    language = o.language;
    file = o.file;
    bankId = o.bankId;
    contentHash = o.contentHash;
    bytes = o.bytes;
    parsed_.reset();
  }
  return *this;
}
BankImage::BankImage(BankImage&&) noexcept = default;
BankImage& BankImage::operator=(BankImage&&) noexcept = default;

const ParsedBank* BankImage::view() const {
  if (!parsed_) {
    parsed_ = std::make_shared<ParsedBank>();
    parsed_->Parse(bytes);
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

const std::vector<FieldColumn>& FieldColumns() {
  static const std::vector<FieldColumn> f = MakeFields();
  return f;
}

std::vector<CellRef> LocateField(const DocBuild& build, const ProjectDocument& doc, const std::string& guid,
                                 const std::string& path) {
  std::vector<CellRef> out;
  const DocObject* o = doc.Find(guid);
  if (o == nullptr) return out;
  const std::string kind = o->kind();
  const FieldColumn* field = nullptr;
  std::vector<std::uint32_t> idx;
  for (const FieldColumn& f : FieldColumns()) {
    if (kind == f.kind && MatchPattern(path, f.pattern, &idx)) {
      field = &f;
      break;
    }
  }
  if (field == nullptr) return out;
  const Id id = HashId(o->name());
  for (const BankImage& b : build.banks) {
    const ParsedBank* v = b.view();
    if (v == nullptr) continue;
    std::uint32_t row = bf::kNone;
    if (kind == "ducking") {
      // DUCK rows: the duckings of the bank in name order (build.cpp); the live index is the
      // rank among the bank's rules with the same target bus.
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
      if (row != bf::kNone && row >= v->Rows(ch::DUCK)) row = bf::kNone;
    } else if (field->chunk == ch::EVNT || field->chunk == ch::BUSS || field->chunk == ch::CATG ||
               field->chunk == ch::PARM || field->chunk == ch::VLGR || field->chunk == ch::MPLS) {
      row = FindIdRow(*v, field->chunk, id);
    } else if (const Owner* o1 = FindOwner(kOwners, std::size(kOwners), field->chunk)) {
      if (idx.size() == 1) row = InRange(*v, o1->parent, FindIdRow(*v, o1->parent, id), o1->firstCol, o1->countCol, idx[0]);
    } else if (const Owner* o2 = FindOwner(kOwners2, std::size(kOwners2), field->chunk)) {
      const Owner* p1 = FindOwner(kOwners, std::size(kOwners), o2->parent);
      if (p1 != nullptr && idx.size() >= 2) {
        const std::uint32_t mid = InRange(*v, p1->parent, FindIdRow(*v, p1->parent, id), p1->firstCol, p1->countCol, idx[0]);
        row = InRange(*v, o2->parent, mid, o2->firstCol, o2->countCol, idx[1]);
      }
    }
    if (row == bf::kNone) continue;
    CellRef c;
    c.bank = b.name;
    c.language = b.language;
    c.bankId = b.bankId;
    c.chunk = field->chunk;
    c.column = field->column;
    c.row = row;
    if (!v->Cell(c.chunk, c.column, row, &c.bits)) continue;
    Address(*v, &c);
    Classify(&c);
    out.push_back(c);
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
          Classify(&e.cell);
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
