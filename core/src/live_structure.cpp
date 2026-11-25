// Block-boundary edits. Authored rows/indices remain stable for events, snapshots
// and value overrides. Additions use the defining bank's writable dev reserve.
#include <algorithm>
#include <cmath>
#include <new>

#include "access.hpp"
#include "core_impl.hpp"

namespace snd::core {
namespace {
constexpr schema::SystemId kSys = schema::SystemId::ApplyCommands;

struct LiveEffect : EditSpan {
  EffectInstance effect{};
  float params[dsp::kMaxEffectParams]{};
};
constexpr std::size_t kStateOffset = (sizeof(LiveEffect) + 63u) & ~std::size_t{63};

BankRecord* Editable(Impl& im, SlotIndex slot) noexcept {
  if (!im.tailScratch || slot == kNoSlot || !base::rows::IsLive(im.tables.Bus.rows, slot) || im.busDefs[slot].defBank >= im.cfg.maxBanks) return nullptr;
  const auto& b = im.ap.banks[im.busDefs[slot].defBank];
  return b.state == BankAudioState::Registered && b.rec->view->writable() ? b.rec : nullptr;
}

// Claim a nonmoving interval in already bound memory. Gaps are immediately
// reusable. Both the per-bank and system-wide physical limits include tails.
template<class T>
T* Claim(Impl& im, BankRecord& bank, std::uint64_t bytes, std::uint64_t token) noexcept {
  const auto& overlay = bank.prepared->overlay;
  auto left = im.config.dev_reserve.bytes;
  for (std::uint32_t i = 0; i < im.cfg.maxBanks; ++i) {
    const auto* r = RegisteredBank(im, static_cast<std::uint16_t>(i));
    if (r) left -= r->liveBytes;
  }
  if (bytes > left || bytes > overlay.spareBytes) return nullptr;
  auto** at = &bank.edits;
  std::uint64_t offset = 0;
  while (*at) {
    const auto start = static_cast<std::uint64_t>(reinterpret_cast<std::uint8_t*>(*at) - overlay.spare);
    if (start >= offset && bytes <= start - offset) break;
    offset = (start + (*at)->bytes + 63u) & ~std::uint64_t{63};
    at = &(*at)->next;
  }
  if (offset > overlay.spareBytes || bytes > overlay.spareBytes - offset) return nullptr;
  auto* item = new (overlay.spare + offset) T{};
  item->next = *at;
  item->bytes = bytes;
  item->token = token;
  *at = item;
  bank.liveBytes += bytes;
  return item;
}

template<class C>
std::uint32_t SpareRow(const BankRecord& bank) noexcept {
  const auto* table = bank.view->Table(C::kChunk);
  const auto* values = bank.view->MutableColumn<C>();
  if (values) {
    for (auto i = table->rows; i < table->capacity; ++i) if (values[i] == C::kDefault) return i;
  }
  return bf::kNone;
}

void Release(BankRecord& bank, EditSpan* span) noexcept {
  if (span->prepared) span->prepared->held.store(false, std::memory_order_release);
  if (span->table == bf::chunk::BUSS) bank.view->MutableColumn<bk::col::BUSS_id>()[span->row] = bk::col::BUSS_id::kDefault;
  if (span->table == bf::chunk::EFFX) bank.view->MutableColumn<bk::col::EFFX_effectType>()[span->row] = bk::col::EFFX_effectType::kDefault;
  auto** at = &bank.edits;
  while (*at && *at != span) at = &(*at)->next;
  if (*at) {
    *at = span->next;
    bank.liveBytes -= span->bytes;
  }
}

RangeU32 EffectRange(Impl& im, SlotIndex slot) noexcept {
  schema::Tables& t = im.tables;
  return SND_W(Bus_effectRange)[base::rows::DenseOf(t.Bus.rows, slot)];
}
std::uint32_t ChainSize(Impl& im, SlotIndex slot) noexcept {
  const auto& b = im.busDefs[slot];
  return b.customChain ? b.chainCount : EffectRange(im, slot).count;
}
void BindChain(Impl& im, BankRecord& bank, SlotIndex slot) noexcept {
  auto& b = im.busDefs[slot];
  if (b.customChain) return;
  const auto range = EffectRange(im, slot);
  b.chainCount = range.count;
  for (std::uint32_t i = 0; i < range.count; ++i) b.chain[i] = &bank.effects[range.first + i];
  b.customChain = true;
}
void Insert(BusDef& b, std::uint32_t pos, EffectInstance* e) noexcept {
  std::move_backward(b.chain + pos, b.chain + b.chainCount, b.chain + b.chainCount + 1);
  b.chain[pos] = e;
  ++b.chainCount;
}

void UnlinkTail(Impl& im, EffectInstance& e) noexcept {
  if (e.tailBus == kNoSlot) return;
  auto** at = &im.tails;
  while (*at && *at != &e) at = &(*at)->nextTail;
  if (*at) *at = e.nextTail;
  e.tailBus = kNoSlot;
  e.nextTail = nullptr;
}
void RetireEffect(Impl& im, BankRecord& bank, EffectInstance& e, SlotIndex target) noexcept {
  dsp::EffectSlot slot{e.type, e.plugin, e.state, nullptr, nullptr, 0u, 0u};
  UnlinkTail(im, e);
  if (e.ok && !dsp::EffectSlotIsTailDone(slot)) {
    e.tailBus = target;
    e.tailBank = static_cast<std::uint16_t>(bank.handle.slot);
    e.nextTail = im.tails;
    im.tails = &e;
  } else if (e.live && !e.parked) {
    --bank.liveEffects;
    Release(bank, e.live);
  }
}
void RestoreBus(Impl& im, SlotIndex s) noexcept {
  auto& b = im.busDefs[s];
  schema::Tables& t = im.tables;
  const auto d = base::rows::InsertNext(SND_ROWS(Bus), s);
  SND_W(Bus_busId)[d] = b.id;
  SND_W(Bus_volume)[d] = b.savedVolume;
  SND_W(Bus_parentSlot)[d] = b.savedParent;
  SND_W(Bus_effectRange)[d] = b.savedEffects;
  SND_W(Bus_sendRange)[d] = b.savedSends;
  if (b.removed) {
    for (std::uint32_t i = 0; i < b.chainCount; ++i) {
      auto& e = *b.chain[i];
      UnlinkTail(im, e);
      e.parked = false;
    }
  }
  b.removed = 0;
}
// Compose same-block deletions before slots can be reused. Each table writer
// applies this mapping to its own previously existing rows on its next pass.
void RedirectBusVoices(Impl& im, SlotIndex from, SlotIndex to) noexcept {
  if (!im.routesPending) {
    for (SlotIndex i = 0; i < im.cfg.busCap; ++i) im.retiredRoutes[i] = i;
    im.routesPending = true;
  }
  for (SlotIndex i = 0; i < im.cfg.busCap; ++i) {
    if (im.retiredRoutes[i] == from) im.retiredRoutes[i] = to;
  }
}

void DropSends(BankRecord& r, BusDef& bus) noexcept {
  while (bus.sends) {
    auto* send = bus.sends;
    bus.sends = send->nextSend;
    Release(r, send);
  }
}
bool Incoming(Impl& im, SlotIndex target) noexcept {
  schema::Tables& t = im.tables;
  for (SlotIndex s = 0; s < im.cfg.busCap; ++s) {
    const auto& b = im.busDefs[s];
    if (b.refCount == 0 || s == target) continue;
    const auto d = base::rows::DenseOf(t.Bus.rows, s);
    const auto parent = b.removed ? b.savedParent : SND_W(Bus_parentSlot)[d];
    if (parent == target) return true;
    for (auto* p = b.sends; p; p = p->nextSend) if (p->target == target) return true;
    const auto* r = RegisteredBank(im, b.defBank);
    if (!r) continue;
    const auto range = b.removed ? b.savedSends : SND_W(Bus_sendRange)[d];
    for (std::uint32_t i = 0; i < range.count; ++i) {
      const auto row = r->view->Column<bk::col::SEND_targetBus>()[range.first + i];
      if (row < r->busCount && r->busSlot[row] == target) return true;
    }
  }
  return false;
}
}  // namespace

void RestoreBusRow(Impl& im, SlotIndex slot) noexcept { RestoreBus(im, slot); }

std::uint32_t System::PublishedBusChannels(Id id) const noexcept {
  if (!id || !impl_->publishedBus) return 0;
  for (SlotIndex s = 0; s < impl_->cfg.busCap; ++s) {
    const auto layout = impl_->publishedBus[s].load(std::memory_order_acquire);
    if ((layout >> 32) == id) return static_cast<std::uint32_t>(layout);
  }
  return 0;
}

void System::LiveReserveLeft(std::uint32_t* buses, std::uint32_t* effects, std::uint32_t* events,
                             std::uint64_t* bytes) const noexcept {
  auto left = impl_->config.dev_reserve;
  for (std::uint32_t i = 0; i < impl_->cfg.maxBanks; ++i) {
    auto* r = RegisteredBank(*impl_, static_cast<std::uint16_t>(i));
    if (r) {
      left.buses -= r->liveBuses;
      left.effects -= r->liveEffects;
      left.bytes -= r->liveBytes;
    }
  }
  if (buses) *buses = left.buses;
  if (effects) *effects = left.effects;
  if (events) *events = left.events;
  if (bytes) *bytes = left.bytes;
}

snd_result System::LiveAddBus(Id id, Id parent, std::uint32_t channels, float db, std::uint64_t token) noexcept {
  auto& im = *impl_;
  if (!id || !token || !channels || channels > kMaxChannels || (!std::isfinite(db) || !std::isfinite(DbToGain(db))) || id == parent ||
      im.busMap.Find(id) != kNoSlot) return SND_ERR_INVALID_ARGUMENT;
  const auto p = im.busMap.Find(parent);
  if (p == kNoSlot) return SND_ERR_NOT_FOUND;
  auto* r = Editable(im, p);
  if (!r) return SND_ERR_UNSUPPORTED;
  std::uint32_t left;
  LiveReserveLeft(&left, nullptr, nullptr, nullptr);
  const auto row = SpareRow<bk::col::BUSS_id>(*r);
  if (row == bf::kNone || r->nextCell == bf::kNone) return SND_ERR_LIMIT_REACHED;
  if (!left || r->liveBuses >= r->prepared->overlay.reserveBuses) return SND_ERR_LIMIT_REACHED;
  if (!OrderBusGraph(im, kNoSlot, kNoSlot, true)) return SND_ERR_INVALID_ARGUMENT;
  const auto s = im.busFree.Pop();
  if (s == kNoSlot) return SND_ERR_LIMIT_REACHED;
  auto* span = Claim<EditSpan>(im, *r, sizeof(EditSpan), token);
  if (!span) { im.busFree.Push(s); return SND_ERR_LIMIT_REACHED; }
  auto& b = im.busDefs[s];
  b = BusDef{};
  b.id = id;
  b.defBank = static_cast<std::uint16_t>(r->handle.slot);
  b.defRow = row;
  span->table = bf::chunk::BUSS;
  span->row = row;
  span->cell = r->nextCell++;
  r->view->MutableColumn<bk::col::BUSS_id>()[row] = id;
  r->view->MutableColumn<bk::col::BUSS_channels>()[row] = channels;
  r->view->MutableColumn<bk::col::BUSS_volumeDb>()[row] = db;
  r->view->MutableColumn<bk::col::BUSS_parent>()[row] = im.busDefs[p].defRow;
  b.refCount = 1;
  b.live = span;
  b.channels = channels;
  b.baseVolumeDb = db;
  b.savedVolume = TimedRamp{1, 1, 0, 0, 0};
  b.savedParent = p;
  RestoreBus(im, s);
  im.busMap.Set(id, s);
  ++r->liveBuses;
  RebuildBusGraph(im);
  return SND_OK;
}

snd_result System::LiveRemoveBus(Id id, std::uint64_t token) noexcept {
  auto& im = *impl_;
  schema::Tables& t = im.tables;
  const auto s = im.busMap.Find(id);
  if (s == kNoSlot || im.busDefs[s].removed) return SND_ERR_NOT_FOUND;
  auto* r = Editable(im, s);
  if (!r) return SND_ERR_UNSUPPORTED;
  auto& b = im.busDefs[s];
  const auto d = base::rows::DenseOf(t.Bus.rows, s);
  const auto parent = SND_W(Bus_parentSlot)[d];
  if (!token || parent == kNoSlot || s == im.rd.masterSlot || b.refCount != 1 || Incoming(im, s))
    return SND_ERR_INVALID_ARGUMENT;
  if (!b.live && ChainSize(im, s) > kMaxBusEffects) return SND_ERR_UNSUPPORTED;
  // Every child or incoming send must first be removed; voices can keep playing.
  for (auto* e = im.tails; e; e = e->nextTail) if (e->tailBus == s) e->tailBus = parent;
  if (b.live) {
    RedirectBusVoices(im, s, parent);
    DropSends(*r, b);
    for (std::uint32_t i = 0; i < b.chainCount; ++i) RetireEffect(im, *r, *b.chain[i], parent);
    Release(*r, b.live);
    --r->liveBuses;
    im.busMap.Erase(id);
    b = BusDef{};
    im.busFree.Push(s);
  } else {
    BindChain(im, *r, s);
    for (std::uint32_t i = 0; i < b.chainCount; ++i) {
      auto& e = *b.chain[i];
      e.parked = true;
      RetireEffect(im, *r, e, parent);
    }
    b.savedParent = parent;
    b.savedVolume = SND_W(Bus_volume)[d];
    b.savedEffects = SND_W(Bus_effectRange)[d];
    b.savedSends = SND_W(Bus_sendRange)[d];
    b.removed = token; // keep the slot identity for bank references and existing playbacks
  }
  RemoveBusRow(t, s);
  RebuildBusGraph(im);
  return SND_OK;
}

snd_result System::LiveAddSend(Id from, Id to, float db, bool pre, std::uint64_t token) noexcept {
  auto& im = *impl_;
  const auto s = im.busMap.Find(from), t = im.busMap.Find(to);
  if (s == kNoSlot || t == kNoSlot) return SND_ERR_NOT_FOUND;
  if (!token || s == t || !std::isfinite(db) || !std::isfinite(DbToGain(db))) return SND_ERR_INVALID_ARGUMENT;
  auto* r = Editable(im, s);
  if (!r) return SND_ERR_UNSUPPORTED;
  if (!base::rows::IsLive(im.tables.Bus.rows, t)) return SND_ERR_NOT_FOUND;
  const auto& destination = im.ap.banks[im.busDefs[t].defBank];
  if (destination.state != BankAudioState::Registered) return SND_ERR_NOT_READY;
  if (r->nextCell == bf::kNone) return SND_ERR_LIMIT_REACHED;
  auto* send = Claim<LiveSend>(im, *r, sizeof(LiveSend), token);
  if (!send) return SND_ERR_LIMIT_REACHED;
  if (!OrderBusGraph(im, s, t, true)) {
    Release(*r, send);
    return SND_ERR_INVALID_ARGUMENT;
  }
  send->cell = r->nextCell++;
  send->target = t;
  send->targetBank = destination.rec->handle.Value();
  send->db = db;
  send->gain = DbToGain(db);
  send->pre = pre;
  auto** end = &im.busDefs[s].sends;
  while (*end) end = &(*end)->nextSend;
  *end = send;
  return SND_OK;
}

snd_result System::LiveAddEffect(Id bus, Id type, std::uint32_t pos, const float* params,
                                 std::uint32_t count, std::uint64_t token, PreparedLivePlugin* prepared) noexcept {
  auto& im = *impl_;
  const auto s = im.busMap.Find(bus);
  if (s == kNoSlot) return SND_ERR_NOT_FOUND;
  auto* r = Editable(im, s);
  if (!r) return SND_ERR_UNSUPPORTED;
  const snd_dsp_effect* plugin = nullptr;
  for (std::uint32_t i = 0; i < im.config.plugins.effect_count; ++i) {
    if (im.config.plugins.effects[i].effect_type == type) {
      plugin = &im.config.plugins.effects[i];
      break;
    }
  }
  if (plugin && (!prepared || prepared->plugin != plugin || prepared->channels != im.busDefs[s].channels))
    return SND_ERR_UNSUPPORTED;
  const auto* info = plugin ? nullptr : dsp::FindEffect(type);
  if (!plugin && !info) return SND_ERR_UNSUPPORTED;
  const auto paramCount = info ? info->paramCount : count;
  const auto n = ChainSize(im, s);
  if (pos == ~0u) pos = n;
  if (!token || pos > n || (count > paramCount || count > dsp::kMaxEffectParams) || (count && !params)) return SND_ERR_INVALID_ARGUMENT;
  for (std::uint32_t i = 0; i < count; ++i) if (!std::isfinite(params[i])) return SND_ERR_INVALID_ARGUMENT;
  std::uint32_t left;
  LiveReserveLeft(nullptr, &left, nullptr, nullptr);
  if (n >= kMaxBusEffects || !left || r->liveEffects >= r->prepared->overlay.reserveEffects)
    return SND_ERR_LIMIT_REACHED;
  snd_effect_desc d{};
  d.effect_type = type;
  d.channels = im.busDefs[s].channels;
  d.sample_rate = im.cfg.sampleRate;
  d.max_frames = im.cfg.blockFrames;
  d.param_count = count;
  d.initial_params = params;
  const auto size = plugin ? prepared->bytes : dsp::EffectStateBytes(d, d.sample_rate);
  if (!size) return SND_ERR_UNSUPPORTED;
  const auto row = SpareRow<bk::col::EFFX_effectType>(*r);
  if (row == bf::kNone || bf::kNone - r->nextCell < 1u + paramCount) return SND_ERR_LIMIT_REACHED;
  auto* node = Claim<LiveEffect>(im, *r, kStateOffset + size, token);
  if (!node) return SND_ERR_LIMIT_REACHED;
  auto& e = node->effect;
  e.state = plugin ? prepared->state : reinterpret_cast<std::uint8_t*>(node) + kStateOffset;
  e.plugin = plugin;
  if (!plugin && !dsp::EffectInit(e.state, size, d, d.sample_rate)) {
    Release(*r, node);
    return SND_ERR_INVALID_ARGUMENT;
  }
  node->prepared = plugin ? prepared : nullptr;
  node->row = row;
  node->table = bf::chunk::EFFX;
  node->cell = r->nextCell;
  r->nextCell += 1u + paramCount;
  r->view->MutableColumn<bk::col::EFFX_effectType>()[row] = type;
  e.type = type;
  e.channels = d.channels;
  e.live = node;
  e.owner = s;
  e.ok = 1;
  e.params = node->params;
  e.paramCount = paramCount;
  if (info) dsp::EffectDefaultParams(info->kind, e.params);
  if (count) std::copy_n(params, count, e.params);
  BindChain(im, *r, s);
  Insert(im.busDefs[s], pos, &e);
  ++r->liveEffects;
  return SND_OK;
}

snd_result System::LiveRemoveEffect(Id bus, std::uint32_t index, std::uint64_t token) noexcept {
  auto& im = *impl_;
  const auto s = im.busMap.Find(bus);
  if (s == kNoSlot) return SND_ERR_NOT_FOUND;
  auto* r = Editable(im, s);
  if (!r) return SND_ERR_UNSUPPORTED;
  const auto n = ChainSize(im, s);
  if (!token || index >= n) return SND_ERR_INVALID_ARGUMENT;
  if (n > kMaxBusEffects) return SND_ERR_UNSUPPORTED;
  BindChain(im, *r, s);
  auto& b = im.busDefs[s];
  auto& e = *b.chain[index];
  std::move(b.chain + index + 1, b.chain + n, b.chain + index);
  --b.chainCount;
  e.owner = s;
  if (!e.live) { e.removed = token; e.oldIndex = index; }
  RetireEffect(im, *r, e, s);
  return SND_OK;
}

std::uint64_t System::LiveBusBank(Id bus) const noexcept {
  const auto s = impl_->busMap.Find(bus);
  if (s == kNoSlot) return 0;
  const auto* r = RegisteredBank(*impl_, impl_->busDefs[s].defBank);
  return r ? r->handle.Value() : 0;
}

std::uint64_t System::LiveOrigin(Id bus, bool effect, std::uint32_t index) const noexcept {
  const auto s = impl_->busMap.Find(bus);
  if (s == kNoSlot) return 0;
  const auto& b = impl_->busDefs[s];
  if (b.removed) return 0;
  const auto* span = effect ? (index < b.chainCount ? b.chain[index]->live : nullptr) : b.live;
  return span ? span->token : 0;
}

snd_result System::LiveUndoEdit(std::uint64_t token) noexcept {
  if (!token) return SND_ERR_INVALID_ARGUMENT;
  auto& im = *impl_;
  for (SlotIndex s = 0; s < im.cfg.busCap; ++s) {
    auto& b = im.busDefs[s];
    if (!b.refCount) continue;
    auto& r = *im.ap.banks[b.defBank].rec;
    if (b.live && b.live->token == token) {
      auto* span = b.live;
      if (b.refCount > 1 && !b.chainCount && !b.sends && AdoptAuthoredBus(im, s, r.handle.slot)) {
        Release(r, span);
        --r.liveBuses;
        RebuildBusGraph(im);
        return SND_OK;
      }
      return LiveRemoveBus(b.id, token);
    }
    if (b.removed == token) { RestoreBus(im, s); RebuildBusGraph(im); return SND_OK; }
    for (std::uint32_t i = 0; i < b.chainCount; ++i) {
      auto* span = b.chain[i]->live;
      if (span && span->token == token) return LiveRemoveEffect(b.id, i, token);
    }
    for (auto** p = &b.sends; *p; p = &(*p)->nextSend) {
      if ((*p)->token != token) continue;
      auto* send = *p;
      *p = send->nextSend;
      Release(r, send);
      RebuildBusGraph(im);
      return SND_OK;
    }
  }
  for (std::uint32_t k = 0; k < im.cfg.maxBanks; ++k) {
    const auto* r = RegisteredBank(im, static_cast<std::uint16_t>(k));
    if (!r) continue;
    for (std::uint32_t i = 0; i < r->effectCount; ++i) {
      auto& e = r->effects[i];
      if (e.removed != token) continue;
      auto& b = im.busDefs[e.owner];
      if (b.removed || b.chainCount >= kMaxBusEffects) return SND_ERR_LIMIT_REACHED;
      UnlinkTail(im, e);
      Insert(b, std::min(e.oldIndex, b.chainCount), &e);
      e.removed = 0;
      return SND_OK;
    }
  }
  return SND_OK;  // already cancelled, or bank retired
}

void FinishEffectTail(Impl& im, EffectInstance& e) noexcept {
  auto& r = *im.ap.banks[e.tailBank].rec;
  UnlinkTail(im, e);
  if (e.live && !e.parked) {
    --r.liveEffects;
    Release(r, e.live);
  }
}

void PrepareBusRetirement(Impl& im, BankRecord& r) noexcept {
  for (auto* e = im.tails; e;) {
    auto* next = e->nextTail;
    if (e->tailBank == r.handle.slot) FinishEffectTail(im, *e);
    e = next;
  }
  for (auto* span = r.edits; span; span = span->next) {
    if (span->prepared) {
      span->prepared->held.store(false, std::memory_order_release);
      span->prepared = nullptr;
    }
  }
  // Remove pointers to the retiring bank before its memory can pass the epoch fence.
  for (SlotIndex s = 0; s < im.cfg.busCap; ++s) {
    auto& b = im.busDefs[s];
    if (!b.refCount) continue;
    if (b.defBank != r.handle.slot) {
      // The destination generation retired. Drop foreign sends before its slot is recycled.
      auto** link = &b.sends;
      while (*link) {
        auto* send = *link;
        if (send->targetBank == r.handle.Value()) {
          *link = send->nextSend;
          Release(*im.ap.banks[b.defBank].rec, send);
        } else {
          link = &send->nextSend;
        }
      }
      continue;
    }
    if (b.removed) RestoreBus(im, s);
    b.sends = nullptr;
    b.customChain = false;
    b.chainCount = 0;
    if (!b.live) continue;
    if (AdoptAuthoredBus(im, s, r.handle.slot)) continue;
    RedirectBusVoices(im, s, im.rd.masterSlot == s ? kNoSlot : im.rd.masterSlot);
    RemoveBusRow(im.tables, s);
    im.busMap.Erase(b.id);
    b = BusDef{};
    im.busFree.Push(s);
  }
}

}  // namespace snd::core
