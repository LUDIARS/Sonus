// Live link support in core (design.md 11.2, 11.3; dev builds): the profiler
// reads and the override-layer writes the live link module calls from its
// audio-thread hooks (CoreDesc::liveTap, CoreDesc::liveCommand).
//
// Profiler reads happen at the end of a block, after every system ran, so they
// read each column through its writer (DiagRead): nothing writes any column at
// that point. They are diagnostics, not a system of 3.2.
//
// Override layer (11.2): a cell is (registered bank, table, row, column) of the
// bank's writable copy (bank::PreparedBank::overlay, dev banks only). A write
// happens at a block boundary (ApplyCommands, through the live link's
// liveCommand) and refreshes what core copied out of the bank at registration.
// Only value columns are live-editable: every f32 column (finite values) and the
// u32 columns listed in EditRule (counts, ids looked up through maps, small
// enums with their range). Row references, ranges, blob offsets and layout
// flags stay as validated (bank-format.md §8): changing them needs a rebuild
// and a hot reload. EVNT.priority is excluded because the game side reads it
// on registered threads (D-10).
#include <cmath>
#include <cstring>

#include "access.hpp"
#include "core_impl.hpp"

namespace snd::core {

namespace {

namespace ch = bf::chunk;
namespace co = bf::col;

template <schema::ColumnId C>
const typename schema::ColumnTraits<C>::Type* DiagRead(const schema::Tables& t) noexcept {
  return schema::ReadColumn<schema::ColumnTraits<C>::kWriter, C>(t);
}

const BankRecord* LiveBank(const Impl& im, std::uint32_t slot) noexcept {
  if (slot >= im.cfg.maxBanks) return nullptr;
  return RegisteredBank(im, static_cast<std::uint16_t>(slot));
}

// U32 column read from a view (absent column: its default).
std::uint32_t ViewU32(const bk::BankView& v, std::uint32_t fourcc, std::uint32_t column, std::uint32_t row) noexcept {
  const bk::TableView* t = v.Table(fourcc);
  const bf::ColumnInfo* info = bf::check::FindColumnInfo(fourcc, column);
  if (t == nullptr || info == nullptr || row >= t->rows) return bf::kNone;
  if (t->columns[column] == nullptr) return static_cast<std::uint32_t>(info->defaultValue);
  std::uint32_t x = 0;
  std::memcpy(&x, static_cast<const std::uint8_t*>(t->columns[column]) + std::size_t{row} * 4u, 4u);
  return x;
}

bool HasIdColumn(std::uint32_t fourcc) noexcept {
  const bf::ColumnInfo* info = bf::check::FindColumnInfo(fourcc, 0);
  if (info == nullptr || info->type != bf::ColumnType::U32) return false;
  const char* n = info->name;
  const std::size_t len = std::strlen(n);
  return len >= 3 && std::strcmp(n + len - 3, ".id") == 0;
}

std::uint32_t FindById(const bk::BankView& v, std::uint32_t fourcc, Id id) noexcept {
  const std::uint32_t rows = v.Rows(fourcc);
  for (std::uint32_t r = 0; r < rows; ++r) {
    if (ViewU32(v, fourcc, 0, r) == id) return r;
  }
  return bf::kNone;
}

// Row `index` within the owner row's [first, first + count) range, kNone if outside.
std::uint32_t InRange(const bk::BankView& v, std::uint32_t ownerChunk, std::uint32_t ownerRow, std::uint32_t firstCol,
                      std::uint32_t countCol, std::uint32_t index) noexcept {
  if (ownerRow == bf::kNone) return bf::kNone;
  const std::uint32_t first = ViewU32(v, ownerChunk, firstCol, ownerRow);
  const std::uint32_t count = ViewU32(v, ownerChunk, countCol, ownerRow);
  if (first == bf::kNone || index >= count) return bf::kNone;
  return first + index;
}

struct Owner {
  std::uint32_t table;
  std::uint32_t parent;     // chunk of the parent row (owned by an id table, or second level)
  std::uint32_t firstCol;
  std::uint32_t countCol;
};
// One level below an id table.
constexpr Owner kOwners[] = {
    {ch::TRAK, ch::EVNT, co::EVNT_trackFirst, co::EVNT_trackCount},
    {ch::SEND, ch::BUSS, co::BUSS_sendFirst, co::BUSS_sendCount},
    {ch::EFFX, ch::BUSS, co::BUSS_effectFirst, co::BUSS_effectCount},
    {ch::SNPV, ch::SNAP, co::SNAP_valueFirst, co::SNAP_valueCount},
    {ch::MPLE, ch::MPLS, co::MPLS_entryFirst, co::MPLS_entryCount},
    {ch::CURV, ch::EVNT, co::EVNT_curveFirst, co::EVNT_curveCount},
};
// Two levels: (id table) -> parent list table -> this table.
constexpr Owner kOwners2[] = {
    {ch::CLIP, ch::TRAK, co::TRAK_clipFirst, co::TRAK_clipCount},
    {ch::ACTN, ch::TRAK, co::TRAK_actionFirst, co::TRAK_actionCount},
    {ch::EFPV, ch::EFFX, co::EFFX_paramFirst, co::EFFX_paramCount},
    {ch::CPNT, ch::CURV, co::CURV_pointFirst, co::CURV_pointCount},
};

const Owner* FindOwner(const Owner* list, std::size_t n, std::uint32_t table) noexcept {
  for (std::size_t i = 0; i < n; ++i) {
    if (list[i].table == table) return &list[i];
  }
  return nullptr;
}

std::uint32_t ResolveRow(const bk::BankView& v, Id objectId, std::uint32_t fourcc, std::uint32_t index) noexcept {
  if (HasIdColumn(fourcc)) return index == 0u ? FindById(v, fourcc, objectId) : bf::kNone;
  if (const Owner* o = FindOwner(kOwners, sizeof(kOwners) / sizeof(kOwners[0]), fourcc)) {
    return InRange(v, o->parent, FindById(v, o->parent, objectId), o->firstCol, o->countCol, index);
  }
  if (const Owner* o2 = FindOwner(kOwners2, sizeof(kOwners2) / sizeof(kOwners2[0]), fourcc)) {
    const Owner* o1 = FindOwner(kOwners, sizeof(kOwners) / sizeof(kOwners[0]), o2->parent);
    if (o1 == nullptr) return bf::kNone;
    const std::uint32_t mid =
        InRange(v, o1->parent, FindById(v, o1->parent, objectId), o1->firstCol, o1->countCol, index >> 16);
    return InRange(v, o2->parent, mid, o2->firstCol, o2->countCol, index & 0xFFFFu);
  }
  if (fourcc == ch::DUCK) {  // by target bus id: the index-th rule of the bank targeting it
    const std::uint32_t bus = FindById(v, ch::BUSS, objectId);
    if (bus == bf::kNone) return bf::kNone;
    std::uint32_t n = 0;
    for (std::uint32_t r = 0, rows = v.Rows(ch::DUCK); r < rows; ++r) {
      if (ViewU32(v, ch::DUCK, co::DUCK_targetBus, r) == bus && n++ == index) return r;
    }
  }
  return bf::kNone;
}

// Live-editable u32 columns (value range inclusive; kNone = any value).
struct EditRule {
  std::uint32_t chunk;
  std::uint32_t column;
  std::uint32_t maxValue;
};
constexpr EditRule kU32Rules[] = {
    {ch::EVNT, co::EVNT_maxInstances, bf::kNone},
    {ch::EVNT, co::EVNT_virtualReturn, 2u},
    {ch::TRAK, co::TRAK_layerParam, bf::kNone},
    {ch::CLIP, co::CLIP_switchValue, bf::kNone},
    {ch::CLIP, co::CLIP_loopCount, bf::kNone},
    {ch::ACTN, co::ACTN_target, bf::kNone},
    {ch::SEND, co::SEND_preFader, 1u},
    {ch::CURV, co::CURV_input, bf::kNone},
    {ch::PARM, co::PARM_scopeMask, 7u},
    {ch::VLGR, co::VLGR_maxVoices, bf::kNone},
    {ch::VLGR, co::VLGR_stealMode, 3u},
    {ch::MPLS, co::MPLS_mode, 2u},
    {ch::MPLS, co::MPLS_loopCount, bf::kNone},
    {ch::MPLE, co::MPLE_repeat, bf::kNone},
    {ch::MSTG, co::MSTG_overlap, 1u},
};

// SND_OK if `bits` may be written into the column (the column exists).
snd_result CheckEdit(std::uint32_t fourcc, std::uint32_t column, const bf::ColumnInfo& info, std::uint64_t bits) noexcept {
  if (info.type == bf::ColumnType::F32) {
    float f = 0;
    const std::uint32_t lo = static_cast<std::uint32_t>(bits);
    std::memcpy(&f, &lo, 4);
    return std::isfinite(f) ? SND_OK : SND_ERR_INVALID_ARGUMENT;
  }
  if (info.type == bf::ColumnType::U32) {
    for (const EditRule& r : kU32Rules) {
      if (r.chunk == fourcc && r.column == column) {
        return (r.maxValue == bf::kNone || bits <= r.maxValue) && bits <= 0xFFFFFFFFull ? SND_OK
                                                                                         : SND_ERR_INVALID_ARGUMENT;
      }
    }
  }
  return SND_ERR_UNSUPPORTED;
}

float BitsF32(std::uint64_t bits) noexcept {
  float f = 0;
  const std::uint32_t lo = static_cast<std::uint32_t>(bits);
  std::memcpy(&f, &lo, 4);
  return f;
}

// Re-derives core's copies of the edited cell (bank_registry.cpp registration).
void Refresh(Impl& im, const BankRecord& r, std::uint32_t bankSlot, std::uint32_t fourcc, std::uint32_t column,
             std::uint32_t row, std::uint64_t bits) noexcept {
  const bk::BankView& v = *r.view;
  const float f = BitsF32(bits);
  const std::uint32_t u = static_cast<std::uint32_t>(bits);
  if (fourcc == ch::BUSS && column == co::BUSS_volumeDb) {
    const std::uint32_t slot = row < r.busCount ? r.busSlot[row] : kNoSlot;
    if (slot != kNoSlot && im.busDefs[slot].defBank == bankSlot && im.busDefs[slot].defRow == row) {
      im.busDefs[slot].baseVolumeDb = f;
    }
  } else if (fourcc == ch::CATG && column == co::CATG_volumeDb) {
    const std::uint32_t slot = row < r.catCount ? r.catSlot[row] : kNoSlot;
    if (slot < im.cfg.catCap && slot != im.globalCategory) im.catDefs[slot].baseVolumeDb = f;
  } else if (fourcc == ch::VLGR) {
    const std::uint32_t slot = row < r.groupCount ? r.groupSlot[row] : kNoSlot;
    if (slot != kNoSlot) {
      if (column == co::VLGR_maxVoices) im.groupDefs[slot].maxVoices = u;
      if (column == co::VLGR_stealMode) im.groupDefs[slot].stealMode = u;
    }
  } else if (fourcc == ch::PARM) {
    const std::uint32_t slot = im.paramDefMap.Find(v.Column<bk::col::PARM_id>()[row]);
    if (slot != IdMap::kNotFound) {
      ParamDef& d = im.paramDefs[slot];
      if (column == co::PARM_scopeMask) d.scopeMask = u;
      if (column == co::PARM_minValue) d.minValue = f;
      if (column == co::PARM_maxValue) d.maxValue = f;
      if (column == co::PARM_defaultValue) d.defaultValue = f;
      if (column == co::PARM_rampRate) d.rampRate = f;
    }
  } else if (fourcc == ch::DUCK) {
    const std::uint32_t slot = row < r.duckCount ? r.duckSlot[row] : kNoSlot;
    if (slot != kNoSlot) {
      DuckRule& d = im.duckRules[slot];
      if (column == co::DUCK_amountDb) d.amountDb = f;
      if (column == co::DUCK_attackMs) d.attackMs = f;
      if (column == co::DUCK_releaseMs) d.releaseMs = f;
      if (column == co::DUCK_thresholdDb) d.thresholdDb = f;
    }
  } else if (fourcc == ch::EFPV && column == co::EFPV_value) {
    for (std::uint32_t x = 0; x < r.effectCount; ++x) {
      const std::uint32_t pf = v.Column<bk::col::EFFX_paramFirst>()[x];
      const std::uint32_t pc = pf == bf::kNone ? 0u : v.Column<bk::col::EFFX_paramCount>()[x];
      if (pf != bf::kNone && row >= pf && row - pf < pc && row - pf < r.effects[x].paramCount) {
        r.effects[x].params[row - pf] = f;
      }
    }
  }
}

}  // namespace

std::uint32_t System::LiveVoices(livelink::VoiceInfo* out, std::uint32_t capacity) const noexcept {
  const Impl& im = *impl_;
  const schema::Tables& t = im.tables;
  const schema::TableRows& vr = t.Voice.rows;
  const SlotIndex* vpb = DiagRead<schema::ColumnId::Voice_playbackSlot>(t);
  const SourceKind* vkind = DiagRead<schema::ColumnId::Voice_sourceKind>(t);
  const SlotIndex* vbus = DiagRead<schema::ColumnId::Voice_busSlot>(t);
  const Ramp* vgain = DiagRead<schema::ColumnId::Voice_gain>(t);
  const float* vaud = DiagRead<schema::ColumnId::Voice_audibility>(t);
  const SlotIndex* vreal = DiagRead<schema::ColumnId::Voice_realSlot>(t);
  const Id* pev = DiagRead<schema::ColumnId::Playback_eventId>(t);
  const PlaybackStateId* pst = DiagRead<schema::ColumnId::Playback_state>(t);
  const schema::TableRows& pr = t.Playback.rows;
  if (out != nullptr) {
    for (std::uint32_t d = 0; d < vr.count && d < capacity; ++d) {
      livelink::VoiceInfo& o = out[d];
      o = livelink::VoiceInfo{};
      const SlotIndex ps = vpb[d];
      const std::uint32_t pd = ps == kNoSlot ? kNoSlot : base::rows::DenseOf(pr, ps);
      o.playbackSlot = ps;
      o.eventId = pd != kNoSlot ? pev[pd] : 0u;
      o.playbackState = pd != kNoSlot ? static_cast<std::uint32_t>(pst[pd]) : 0u;
      o.audibility = vaud[d];
      o.isVirtual = vreal[d] == kNoSlot ? 1u : 0u;
      o.busId = vbus[d] != kNoSlot && vbus[d] < im.cfg.busCap ? im.busDefs[vbus[d]].id : 0u;
      o.gain = vgain[d].end;
      o.sourceKind = static_cast<std::uint32_t>(vkind[d]);
    }
  }
  return vr.count;
}

std::uint32_t System::LiveMeters(livelink::MeterEntry* out, std::uint32_t capacity) const noexcept {
  const Impl& im = *impl_;
  const schema::TableRows& br = im.tables.Bus.rows;
  const Id* bid = DiagRead<schema::ColumnId::Bus_busId>(im.tables);
  std::uint32_t n = 0;
  auto put = [&](SlotIndex s, Id id, std::uint32_t channels) {
    if (out != nullptr && n < capacity) {
      livelink::MeterEntry& e = out[n];
      e = livelink::MeterEntry{};
      e.busId = id;
      e.channels = channels;
      for (std::uint32_t c = 0; c < kMaxChannels; ++c) {
        if (s != kNoSlot) {
          e.peak[c] = im.rd.peak[s].peak[c];
          e.rms[c] = dsp::RmsMeterValue(im.rd.rms[s], c);
        } else if (c < channels) {
          const float* x = im.rd.outBuf + std::size_t{c} * im.cfg.blockFrames;
          e.peak[c] = dsp::Peak(x, im.cfg.blockFrames);
          e.rms[c] = static_cast<float>(std::sqrt(dsp::SumOfSquares(x, im.cfg.blockFrames) / im.cfg.blockFrames));
        }
      }
    }
    ++n;
  };
  const SlotIndex master =
      im.rd.masterSlot != kNoSlot && base::rows::IsLive(br, im.rd.masterSlot) ? im.rd.masterSlot : kNoSlot;
  put(master, 0u, master != kNoSlot ? im.busDefs[master].channels : im.cfg.outChannels);
  for (std::uint32_t d = 0; d < br.count; ++d) {
    const SlotIndex s = base::rows::SlotAt(br, d);
    if (s == master) continue;
    put(s, bid[d], im.busDefs[s].channels);
  }
  return n;
}

std::uint32_t System::LiveBanks(LiveBankInfo* out, std::uint32_t capacity) const noexcept {
  const Impl& im = *impl_;
  std::uint32_t n = 0;
  for (std::uint32_t s = 0; s < im.cfg.maxBanks; ++s) {
    const BankRecord* r = LiveBank(im, s);
    if (r == nullptr) continue;
    if (out != nullptr && n < capacity) {
      LiveBankInfo& b = out[n];
      b = LiveBankInfo{};
      b.handle = r->handle;
      b.bankId = r->bankId;
      b.languageId = r->languageId;
      b.projectBuildHash = r->view->header().projectBuildHash;
      b.contentHash = r->view->contentHash();
      b.writable = r->view->writable() ? 1u : 0u;
    }
    ++n;
  }
  return n;
}

void System::LiveStats(snd_stats* out) const noexcept {
  if (out == nullptr) return;
  const Impl& im = *impl_;
  *out = im.stats;
  out->rejected_commands += im.rejectedCommands.load(std::memory_order_relaxed);
  out->reserve_overflows += im.reserveOverflows.load(std::memory_order_relaxed);
}

std::uint32_t System::LiveFindCells(Id objectId, std::uint32_t fourcc, std::uint32_t index, LiveCell* out,
                                    std::uint32_t capacity) const noexcept {
  const Impl& im = *impl_;
  std::uint32_t n = 0;
  for (std::uint32_t s = 0; s < im.cfg.maxBanks; ++s) {
    const BankRecord* r = LiveBank(im, s);
    if (r == nullptr) continue;
    const std::uint32_t row = ResolveRow(*r->view, objectId, fourcc, index);
    if (row == bf::kNone) continue;
    if (out != nullptr && n < capacity) out[n] = LiveCell{s, row};
    ++n;
  }
  return n;
}

std::uint32_t System::LiveFindBanks(Id bankId, std::uint32_t* slots, std::uint32_t capacity) const noexcept {
  const Impl& im = *impl_;
  std::uint32_t n = 0;
  for (std::uint32_t s = 0; s < im.cfg.maxBanks; ++s) {
    const BankRecord* r = LiveBank(im, s);
    if (r == nullptr || r->bankId != bankId) continue;
    if (slots != nullptr && n < capacity) slots[n] = s;
    ++n;
  }
  return n;
}

std::uint32_t System::LiveTableRows(std::uint32_t bankSlot, std::uint32_t fourcc) const noexcept {
  const BankRecord* r = LiveBank(*impl_, bankSlot);
  if (r == nullptr || !r->view->writable()) return 0;
  return r->view->Rows(fourcc);
}

snd_result System::LiveWriteCell(const LiveCell& cell, std::uint32_t fourcc, std::uint32_t column, std::uint64_t bits,
                                 std::uint64_t* previous, bool apply) noexcept {
  Impl& im = *impl_;
  const BankRecord* r = LiveBank(im, cell.bankSlot);
  if (r == nullptr) return SND_ERR_NOT_FOUND;
  const bf::ColumnInfo* info = bf::check::FindColumnInfo(fourcc, column);
  if (info == nullptr || column > 0xFFFFu) return SND_ERR_NOT_FOUND;
  bk::WritableColumn wc;
  const snd_result found = bk::FindWritableColumn(*r->prepared, fourcc, static_cast<std::uint16_t>(column), &wc);
  if (found != SND_OK) return found;
  if (cell.row >= wc.rows) return SND_ERR_NOT_FOUND;
  std::uint8_t* at = static_cast<std::uint8_t*>(wc.data) + std::size_t{cell.row} * wc.elemBytes;
  std::uint64_t old = 0;
  std::memcpy(&old, at, wc.elemBytes);
  if (previous != nullptr) *previous = old;
  const std::uint64_t mask = wc.elemBytes == 4u ? 0xFFFFFFFFull : ~0ull;
  if ((bits & ~mask) != 0u && wc.elemBytes == 4u) return SND_ERR_INVALID_ARGUMENT;
  if ((bits & mask) == old) return SND_OK;  // unchanged: any column
  if (fourcc == ch::EVNT && column == co::EVNT_priority) return SND_ERR_UNSUPPORTED;  // read by game threads (D-10)
  const snd_result ok = CheckEdit(fourcc, column, *info, bits);
  if (ok != SND_OK || !apply) return ok;
  std::memcpy(at, &bits, wc.elemBytes);
  Refresh(im, *r, cell.bankSlot, fourcc, column, cell.row, bits);
  return SND_OK;
}

}  // namespace snd::core
