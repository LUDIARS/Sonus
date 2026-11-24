// Live reflection of the document (design.md 11.2), group P.
#include "snd/tool/live_sync.hpp"

#include <algorithm>
#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <set>

#include "snd/buildtool/patch.hpp"
#include "snd/format/bank_format.hpp"
#include "snd/livelink/client.hpp"
#include "snd/livelink/struct_edit.hpp"
#include "snd/tool/names.hpp"
#include "snd/types.h"

namespace snd::tool {
namespace ll = livelink;
namespace bf = bank_format;
namespace ch = bank_format::chunk;
namespace co = bank_format::col;

// --- item 8: what the runtime cannot take live (the one place to change at integration) ------
LiveSupport LiveSupportOf(std::uint32_t fourcc, std::uint32_t column, std::uint64_t bits, bool haveBits) {
  LiveSupport s;
  if (fourcc == ch::PHYS) {
    s.supported = false;
    s.blocking = true;
    s.reason = "material tables (PHYS): the runtime has no live path for them yet (physics, request C-11)";
  } else if (fourcc == buildtool::patch::kChunkFourcc) {
    s.supported = false;
    s.blocking = true;
    s.reason = "waveform patches (XPCH): the runtime has no Patch source and does not read XPCH (request C-10)";
  } else if (fourcc == ch::PTCH) {
    s.supported = false;
    s.blocking = true;
    s.reason = "patch programs (PTCH): not read by the runtime yet (request C-10)";
  } else if (fourcc == ch::CURV && column == co::CURV_compose) {
    s.supported = false;
    s.reason = "curve compose: the runtime applies a fixed rule per target and does not read compose (D-179b)";
  } else if (fourcc == ch::CURV && column == co::CURV_target && haveBits) {
    const auto t = static_cast<bf::CurveTarget>(static_cast<std::uint32_t>(bits));
    if (t == bf::CurveTarget::Send || t == bf::CurveTarget::EffectParam || t == bf::CurveTarget::Spread ||
        t == bf::CurveTarget::ReverbSend || t == bf::CurveTarget::Blend3D) {
      s.supported = false;
      s.reason = "curve target: the runtime does not apply this target yet (stage 3, D-179)";
    }
  } else if (fourcc == ch::SNPV && column == co::SNPV_targetKind && haveBits && bits != 0u) {
    s.supported = false;
    s.reason = "snapshot value: the runtime applies bus volumes only (effect parameters and send levels later)";
  }
  return s;
}

const char* LivePhaseName(LivePhase p) {
  switch (p) {
    case LivePhase::Offline: return "offline";
    case LivePhase::Connecting: return "connecting";
    case LivePhase::AwaitingDecision: return "awaiting decision";
    case LivePhase::Checking: return "checking";
    case LivePhase::Syncing: return "syncing";
    case LivePhase::Live: return "live";
    case LivePhase::Refused: return "refused";
    case LivePhase::Failed: return "failed";
  }
  return "?";
}
const char* LiveRouteName(LiveRoute r) {
  switch (r) {
    case LiveRoute::None: return "-";
    case LiveRoute::SetValue: return "SetValue";
    case LiveRoute::ReplaceChunk: return "ReplaceChunk";
    case LiveRoute::StructEdit: return "StructEdit";
    case LiveRoute::Reload: return "Reload";
  }
  return "?";
}
const char* LiveStateName(LiveState s) {
  switch (s) {
    case LiveState::Live: return "live";
    case LiveState::Queued: return "queued";
    case LiveState::InFlight: return "in flight";
    case LiveState::Reloading: return "reloading";
    case LiveState::Failed: return "failed";
    case LiveState::Unsupported: return "unsupported";
    case LiveState::HeldByMixer: return "held by the mixer";
    case LiveState::Offline: return "offline";
  }
  return "?";
}

ReloadWriter DirectoryReloadWriter(const std::string& dir, const std::string& gamePrefix) {
  auto counter = std::make_shared<std::uint32_t>(0);
  return [dir, gamePrefix, counter](const BankImage& b, std::string* gamePath, std::string* error) {
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    const std::string base = b.file.size() > 5 ? b.file.substr(0, b.file.size() - 5) : b.file;
    const std::string name = base + "." + std::to_string(++*counter) + ".sndb";
    std::ofstream f(std::filesystem::path(dir) / name, std::ios::binary | std::ios::trunc);
    f.write(reinterpret_cast<const char*>(b.bytes.data()), static_cast<std::streamsize>(b.bytes.size()));
    if (!f) {
      if (error != nullptr) *error = "cannot write " + (std::filesystem::path(dir) / name).string();
      return false;
    }
    *gamePath = gamePrefix + name;
    return true;
  };
}

LiveSync::LiveSync(const ProjectDocument& doc, LiveSyncOptions options) : doc_(doc), options_(std::move(options)) {}

std::string LiveSync::GuidText(const Guid& g) const {
  if (g.lo == 0u && g.hi == 0u) return std::string();
  char b[48];
  std::snprintf(b, sizeof b, "%08" PRIx64 "-%04" PRIx64 "-%04" PRIx64 "-%04" PRIx64 "-%012" PRIx64, g.hi >> 32,
                (g.hi >> 16) & 0xFFFFu, g.hi & 0xFFFFu, g.lo >> 48, static_cast<std::uint64_t>(g.lo & 0xFFFFFFFFFFFFull));
  return b;
}

namespace {

std::string MemberOf(std::uint32_t chunk, std::uint32_t column) {
  for (const FieldColumn& f : FieldColumns()) {
    if (f.chunk == chunk && f.column == column) return f.pattern;
  }
  char b[32];
  std::snprintf(b, sizeof b, "%c%c%c%c.%u", static_cast<char>(chunk & 0xFF), static_cast<char>((chunk >> 8) & 0xFF),
                static_cast<char>((chunk >> 16) & 0xFF), static_cast<char>((chunk >> 24) & 0xFF), column);
  return b;
}

DocBuild AsBuild(const std::vector<BankImage>& banks) {
  DocBuild b;
  b.ok = true;
  b.banks = banks;
  return b;
}

bool IsTable(std::uint32_t fourcc) {
  for (const bf::ChunkInfo& ci : bf::kChunks) {
    if (ci.fourcc == fourcc) return ci.kind == bf::ChunkKind::Table;
  }
  return false;
}

std::string CellKey(const std::string& bank, const ll::SetValue& v) {
  return bank + "|" + std::to_string(v.chunk) + "|" + std::to_string(v.runtimeId) + "|" + std::to_string(v.column) +
         "|" + std::to_string(v.index);
}

bool Transient(std::int32_t r) { return r == SND_ERR_COMMAND_BUFFER_FULL || r == SND_ERR_NOT_READY; }

}  // namespace

void LiveSync::SetBaseline(const DocBuild& build) {
  baseline_ = build.banks;
  baselineHash_ = build.buildHash;
  haveBaseline_ = true;
  for (const BankImage& b : build.banks) known_[b.contentHash] = b;
}

ConnectionDesc LiveSync::Prepare(ConnectionDesc desc) {
  if (builtRevision_ != doc_.revision()) {
    flush_ = true;
    Build(0);
  }
  desc.projectBuildHash = current_.ok ? current_.buildHash : desc.projectBuildHash;
  desc.deferSyncDecision = options_.decision == LiveDecision::Ask;
  desc.policy = options_.decision == LiveDecision::Refuse ? MismatchPolicy::Refuse : MismatchPolicy::SendDiffs;
  return desc;
}

bool LiveSync::Build(std::uint64_t nowNs) {
  if (doc_.revision() != seenDocRevision_) {
    seenDocRevision_ = doc_.revision();
    for (const std::string& g : doc_.lastChanged()) {
      if (std::find(changed_.begin(), changed_.end(), g) == changed_.end()) changed_.push_back(g);
    }
  }
  const bool need = doc_.revision() != builtRevision_;
  if (!need && !flush_) return false;
  if (!need && flush_) {
    flush_ = false;
    return false;
  }
  if (!flush_ && builtRevision_ != ~0ull && nowNs >= lastBuildNs_ &&
      nowNs - lastBuildNs_ < std::uint64_t{options_.throttleMs} * 1000000ull) {
    return false;  // thinned: the last edit is built at the next Tick after the interval
  }
  flush_ = false;
  DocBuild b = BuildDocument(doc_, options_.build);
  ++stats_.builds;
  builtRevision_ = doc_.revision();
  lastBuildNs_ = nowNs;
  if (!b.ok) {
    ++stats_.buildErrors;
    LiveProblem p;
    p.revision = builtRevision_;
    p.message = "the document does not build (" + std::to_string(b.errorCount()) + " errors): nothing sent";
    problems_.push_back(p);
    return false;
  }
  current_ = std::move(b);
  if (!haveBaseline_) SetBaseline(current_);
  return true;
}

const BankImage* LiveSync::CurrentBank(const std::string& file) const {
  for (const BankImage& b : current_.banks) {
    if (b.file == file) return &b;
  }
  return nullptr;
}

std::vector<std::string> LiveSync::ObjectsOf(const std::string& bankFile) const {
  std::vector<std::string> out;
  const BankImage* b = CurrentBank(bankFile);
  const std::string name = b != nullptr ? b->name : std::string();
  for (const std::string& g : changed_) {
    const DocObject* o = doc_.Find(g);
    if (o == nullptr) continue;
    if (name.empty() || StringAt(o->doc, "bank") == name || o->kind() == "bank") out.push_back(g);
  }
  return out;
}

void LiveSync::Mark(const Op& op, LiveState s, std::int32_t result, const std::string& reason) {
  std::vector<std::string> guids = op.guids;
  if (guids.empty()) guids.push_back(std::string());
  for (const std::string& g : guids) {
    const std::string member = op.route == LiveRoute::SetValue ? op.member : std::string();
    LiveItem& it = items_[g + "\n" + member];
    it.guid = g;
    it.member = member;
    it.bank = op.bank;
    it.route = op.route;
    it.state = s;
    it.result = result;
    it.reason = reason;
    it.revision = op.revision;
    if (const DocObject* o = g.empty() ? nullptr : doc_.Find(g)) {
      it.kind = o->kind();
      it.name = o->name();
    } else if (g.empty()) {
      it.kind = "bank";
      it.name = op.bank;
    }
  }
}

void LiveSync::Problem(const Op& op, std::int32_t result, const std::string& message) {
  std::vector<std::string> guids = op.guids;
  if (guids.empty()) guids.push_back(std::string());
  for (const std::string& g : guids) {
    LiveProblem p;
    p.revision = op.revision;
    p.guid = g;
    if (const DocObject* o = g.empty() ? nullptr : doc_.Find(g)) p.name = o->name();
    p.member = op.member;
    p.bank = op.bank;
    p.chunk = op.fourcc != 0u ? op.fourcc : op.value.chunk;
    p.route = op.route;
    p.result = result;
    p.message = message;
    problems_.push_back(p);
  }
  if (problems_.size() > 512u) problems_.erase(problems_.begin(), problems_.begin() + static_cast<std::ptrdiff_t>(problems_.size() - 512u));
}

void LiveSync::Unsupported(const std::string& bank, std::uint32_t chunk, std::uint32_t column, const std::string& guid,
                           bool blocking, const std::string& reason) {
  for (const UnsupportedItem& u : unsupported_) {
    if (u.bank == bank && u.chunk == chunk && u.column == column && u.guid == guid && u.reason == reason) return;
  }
  UnsupportedItem u;
  u.bank = bank;
  u.chunk = chunk;
  u.column = column;
  u.guid = guid;
  u.blocking = blocking;
  u.reason = reason;
  unsupported_.push_back(u);
}

void LiveSync::Enqueue(Op op) {
  op.epoch = epoch_[op.bank];
  op.serial = ++serial_;
  if (op.route == LiveRoute::SetValue) {
    latestSerial_[op.cellKey] = op.serial;
    held_.erase(op.cellKey);  // a newer value of the cell decides
    for (auto it = queue_.begin(); it != queue_.end(); ++it) {
      if (it->route == LiveRoute::SetValue && it->cellKey == op.cellKey) {  // the newer value wins
        queue_.erase(it);
        ++stats_.coalesced;
        break;
      }
    }
  }
  Mark(op, LiveState::Queued, 0, std::string());
  queue_.push_back(std::move(op));
}

void LiveSync::Escalate(const std::string& bank, const std::string& reason, std::uint64_t revision,
                        const std::vector<std::string>* guids) {
  Op op;
  op.route = LiveRoute::Reload;
  op.bank = bank;
  op.revision = revision;
  op.guids = guids != nullptr ? *guids : ObjectsOf(bank);
  const BankImage* b = CurrentBank(bank);
  if (b != nullptr) {
    op.bankId = b->bankId;
    const ParsedBank* v = b->view();
    op.languageId = v != nullptr ? v->languageId() : 0u;
  }
  // The document is the truth: a bank the game does not hold (never loaded, or detached by a failed
  // reload) is loaded.
  op.loadIfAbsent = true;
  if (!options_.writer || peerMinor_ < 1u || b == nullptr) {
    const std::string why = b == nullptr ? "the bank is not in the document's build"
                            : peerMinor_ < 1u ? "the game speaks protocol 1.0 (no hot reload)"
                                              : "no output for rebuilt banks (LiveSyncOptions::writer)";
    Unsupported(bank, 0, ~0u, std::string(), true, "needs a rebuild and hot reload (" + reason + "): " + why);
    Mark(op, LiveState::Unsupported, 0, reason + ": " + why);
    return;
  }
  // The reload carries the whole bank: what was queued for it goes with it; answers of requests
  // in flight for it are no longer looked at (epoch).
  ++epoch_[bank];
  for (auto it = queue_.begin(); it != queue_.end();) {
    if (it->bank == bank && it->route != LiveRoute::Reload) {
      it = queue_.erase(it);
    } else {
      ++it;
    }
  }
  for (Op& q : queue_) {
    if (q.bank == bank && q.route == LiveRoute::Reload) {  // one is waiting: it writes the latest build
      for (const std::string& g : op.guids) {
        if (std::find(q.guids.begin(), q.guids.end(), g) == q.guids.end()) q.guids.push_back(g);
      }
      Mark(op, LiveState::Reloading, 0, reason);
      return;
    }
  }
  op.epoch = epoch_[bank];
  ReloadProgress p;
  p.bank = bank;
  p.bankId = op.bankId;
  p.revision = revision;
  p.reason = reason;
  reloads_.push_back(p);
  if (reloads_.size() > 64u) reloads_.erase(reloads_.begin());
  op.reloadIndex = static_cast<std::int32_t>(reloads_.size() - 1u);
  Mark(op, LiveState::Reloading, 0, reason);
  queue_.push_back(std::move(op));
}

// --- planning --------------------------------------------------------------------------------
namespace {

// Bus channels: 0 = the parent's.
std::uint32_t BusChannels(const ParsedBank& b, std::uint32_t row) {
  for (std::uint32_t guard = 0; guard < 64u && row != bf::kNone; ++guard) {
    const std::uint32_t c = b.U32(ch::BUSS, co::BUSS_channels, row);
    if (c != 0u && c != bf::kNone) return c;
    row = b.U32(ch::BUSS, co::BUSS_parent, row);
  }
  return 2u;
}
std::uint64_t F32Bits(float f) {
  std::uint32_t u = 0;
  std::memcpy(&u, &f, 4);
  return u;
}

struct StructPlan {
  bool ok = false;
  struct Edit {
    ll::StructOp op;
    std::uint32_t target;
    std::vector<std::uint64_t> args;
  };
  std::vector<Edit> edits;
  // D-183: value changes of the same tables (buses, effect parameters and sends that exist before
  // and after) go as SetValue after the structural edits, so that no value of the build is lost.
  struct Value {
    std::uint32_t chunk, column, runtimeId, index, row;
    std::uint64_t bits;
  };
  std::vector<Value> values;
};

// Bus / effect / send changes that StructEdit can carry: buses added or removed (other rows keep
// their order), effects appended to or removed from the end of a bus, sends added.
StructPlan DeriveStructEdits(const ParsedBank& a, const ParsedBank& b) {
  StructPlan p;
  auto busIds = [](const ParsedBank& x) {
    std::vector<Id> ids;
    for (std::uint32_t r = 0; r < x.Rows(ch::BUSS); ++r) ids.push_back(x.U32(ch::BUSS, co::BUSS_id, r));
    return ids;
  };
  const std::vector<Id> ia = busIds(a), ib = busIds(b);
  for (std::uint32_t r = 0; r < ib.size(); ++r) {  // added
    if (std::find(ia.begin(), ia.end(), ib[r]) != ia.end()) continue;
    const std::uint32_t parentRow = b.U32(ch::BUSS, co::BUSS_parent, r);
    if (parentRow == bf::kNone || parentRow >= ib.size()) return p;  // a new master: no
    StructPlan::Edit e{ll::StructOp::AddBus, ib[r], {ib[parentRow], BusChannels(b, r), F32Bits(b.F32(ch::BUSS, co::BUSS_volumeDb, r))}};
    p.edits.push_back(e);
    if (b.U32(ch::BUSS, co::BUSS_effectCount, r) != 0u && b.U32(ch::BUSS, co::BUSS_effectCount, r) != bf::kNone) return p;
    if (b.U32(ch::BUSS, co::BUSS_sendCount, r) != 0u && b.U32(ch::BUSS, co::BUSS_sendCount, r) != bf::kNone) return p;
  }
  for (std::uint32_t r = 0; r < ia.size(); ++r) {  // removed
    if (std::find(ib.begin(), ib.end(), ia[r]) == ib.end()) p.edits.push_back(StructPlan::Edit{ll::StructOp::RemoveBus, ia[r], {}});
  }
  for (std::uint32_t r = 0; r < ib.size(); ++r) {  // effects and sends of buses in both
    const std::uint32_t ra = a.FindId(ch::BUSS, ib[r]);
    if (ra == bf::kNone) continue;
    auto count = [](const ParsedBank& x, std::uint32_t row, std::uint32_t col) {
      const std::uint32_t n = x.U32(ch::BUSS, col, row);
      return n == bf::kNone ? 0u : n;
    };
    // The bus's own cells: the volume is a live value; the others need a rebuild.
    for (const std::uint32_t col : {co::BUSS_channels, co::BUSS_flags, co::BUSS_lodPriority}) {
      std::uint64_t x = 0, y = 0;
      a.Cell(ch::BUSS, col, ra, &x);
      b.Cell(ch::BUSS, col, r, &y);
      if (x != y) return p;
    }
    {
      std::uint64_t x = 0, y = 0;
      a.Cell(ch::BUSS, co::BUSS_volumeDb, ra, &x);
      b.Cell(ch::BUSS, co::BUSS_volumeDb, r, &y);
      if (x != y) p.values.push_back(StructPlan::Value{ch::BUSS, co::BUSS_volumeDb, ib[r], 0u, r, y});
    }
    const std::uint32_t ea = count(a, ra, co::BUSS_effectCount), eb = count(b, r, co::BUSS_effectCount);
    const std::uint32_t fa = a.U32(ch::BUSS, co::BUSS_effectFirst, ra), fb = b.U32(ch::BUSS, co::BUSS_effectFirst, r);
    const std::uint32_t common = std::min(ea, eb);
    for (std::uint32_t k = 0; k < common; ++k) {  // the kept effects must be the same kinds
      if (a.U32(ch::EFFX, co::EFFX_effectType, fa + k) != b.U32(ch::EFFX, co::EFFX_effectType, fb + k)) return p;
      if (a.U32(ch::EFFX, co::EFFX_flags, fa + k) != b.U32(ch::EFFX, co::EFFX_flags, fb + k)) return p;
      const std::uint32_t pa = a.U32(ch::EFFX, co::EFFX_paramFirst, fa + k), pca = a.U32(ch::EFFX, co::EFFX_paramCount, fa + k);
      const std::uint32_t pb = b.U32(ch::EFFX, co::EFFX_paramFirst, fb + k), pcb = b.U32(ch::EFFX, co::EFFX_paramCount, fb + k);
      if (pca != pcb) return p;
      for (std::uint32_t q = 0; pa != bf::kNone && pb != bf::kNone && pca != bf::kNone && q < pca; ++q) {
        std::uint64_t x = 0, y = 0;
        a.Cell(ch::EFPV, co::EFPV_value, pa + q, &x);
        b.Cell(ch::EFPV, co::EFPV_value, pb + q, &y);
        if (x != y) p.values.push_back(StructPlan::Value{ch::EFPV, co::EFPV_value, ib[r], (k << 16) | q, pb + q, y});
      }
    }
    for (std::uint32_t k = ea; k < eb; ++k) {
      StructPlan::Edit e{ll::StructOp::AddEffect, ib[r], {b.U32(ch::EFFX, co::EFFX_effectType, fb + k), 0xFFFFFFFFu}};
      const std::uint32_t pf = b.U32(ch::EFFX, co::EFFX_paramFirst, fb + k), pc = b.U32(ch::EFFX, co::EFFX_paramCount, fb + k);
      for (std::uint32_t q = 0; pf != bf::kNone && pc != bf::kNone && q < pc && q < 32u; ++q) {
        e.args.push_back(F32Bits(b.F32(ch::EFPV, co::EFPV_value, pf + q)));
      }
      p.edits.push_back(e);
    }
    for (std::uint32_t k = eb; k < ea; ++k) {
      p.edits.push_back(StructPlan::Edit{ll::StructOp::RemoveEffect, ib[r], {ea - 1u - (k - eb)}});
    }
    const std::uint32_t sa = count(a, ra, co::BUSS_sendCount), sb = count(b, r, co::BUSS_sendCount);
    if (sb < sa) return p;  // removing a send: no op
    const std::uint32_t ga = a.U32(ch::BUSS, co::BUSS_sendFirst, ra), gb = b.U32(ch::BUSS, co::BUSS_sendFirst, r);
    for (std::uint32_t k = 0; k < sa; ++k) {
      if (a.U32(ch::SEND, co::SEND_targetBus, ga + k) != b.U32(ch::SEND, co::SEND_targetBus, gb + k)) return p;
      if (a.U32(ch::SEND, co::SEND_preFader, ga + k) != b.U32(ch::SEND, co::SEND_preFader, gb + k)) return p;
      std::uint64_t x = 0, y = 0;
      a.Cell(ch::SEND, co::SEND_levelDb, ga + k, &x);
      b.Cell(ch::SEND, co::SEND_levelDb, gb + k, &y);
      if (x != y) p.values.push_back(StructPlan::Value{ch::SEND, co::SEND_levelDb, ib[r], k, gb + k, y});
    }
    for (std::uint32_t k = sa; k < sb; ++k) {
      const std::uint32_t target = b.U32(ch::SEND, co::SEND_targetBus, gb + k);
      if (target == bf::kNone || target >= ib.size()) return p;
      p.edits.push_back(StructPlan::Edit{ll::StructOp::AddSend, ib[r],
                                         {ib[target], F32Bits(b.F32(ch::SEND, co::SEND_levelDb, gb + k)),
                                          b.U32(ch::SEND, co::SEND_preFader, gb + k) != 0u ? 1u : 0u}});
    }
  }
  p.ok = !p.edits.empty();
  return p;
}

bool StructOnly(std::uint32_t fourcc) {
  return fourcc == ch::BUSS || fourcc == ch::EFFX || fourcc == ch::EFPV || fourcc == ch::SEND || fourcc == ch::NAME ||
         fourcc == ch::GUID || fourcc == ch::DSPN || fourcc == ch::STRS;
}

}  // namespace

void LiveSync::PlanBank(const BankImage* before, const BankImage& after, bool unknownBase) {
  if (before == nullptr || unknownBase) {
    if (options_.writer && peerMinor_ >= 1u) {
      Escalate(after.file, "the game's bank is a build the tool does not know", builtRevision_);
      return;
    }
    // No reload: replace every table; blobs cannot be verified.
    const ParsedBank* v = after.view();
    if (v == nullptr) return;
    for (const bf::ChunkInfo& ci : bf::kChunks) {
      std::uint64_t n = 0;
      const std::uint8_t* data = v->Chunk(ci.fourcc, &n);
      if (data == nullptr) continue;
      const LiveSupport s = LiveSupportOf(ci.fourcc);
      if (!s.supported && s.blocking) {
        Unsupported(after.file, ci.fourcc, ~0u, std::string(), true, s.reason);
        continue;
      }
      if (ci.kind != bf::ChunkKind::Table || ci.fourcc == ch::NAME || ci.fourcc == ch::GUID || ci.fourcc == ch::ALIS) {
        Unsupported(after.file, ci.fourcc, ~0u, std::string(), false,
                    "not verified: the game's bank is a build the tool does not know and only tables can be sent "
                    "without a hot reload");
        continue;
      }
      Op op;
      op.route = LiveRoute::ReplaceChunk;
      op.revision = builtRevision_;
      op.bank = after.file;
      op.bankId = after.bankId;
      op.fourcc = ci.fourcc;
      op.bytes.assign(data, data + n);
      op.guids = ObjectsOf(after.file);
      Enqueue(std::move(op));
    }
    return;
  }
  const LiveDelta d = DiffBuilds(AsBuild({*before}), AsBuild({after}));
  PlanDelta(d, {*before});
}

void LiveSync::PlanDelta(const LiveDelta& d, const std::vector<BankImage>& before) {
  const std::uint64_t rev = builtRevision_;
  for (const std::string& f : d.banksAdded) {
    if (options_.writer && peerMinor_ >= 1u) {
      // A new bank: loaded with ReloadBank's load-if-absent.
      Escalate(f, "a bank the game does not have", rev);
      for (Op& q : queue_) {
        if (q.bank == f && q.route == LiveRoute::Reload) q.loadIfAbsent = true;
      }
    } else {
      Escalate(f, "a bank the game does not have", rev);
    }
  }
  for (const std::string& f : d.banksRemoved) {
    Unsupported(f, 0, ~0u, std::string(), false, "removed from the project: the game keeps the bank (no unload message)");
  }
  std::set<std::string> reload;
  for (const std::string& f : d.banksRelinked) reload.insert(f);
  // Chunks per bank.
  std::map<std::string, std::vector<const ChunkEdit*>> chunks;
  for (const ChunkEdit& c : d.chunks) chunks[c.language.empty() ? c.bank + ".sndb" : c.bank + "." + c.language + ".sndb"].push_back(&c);
  for (const auto& [file, list] : chunks) {
    if (reload.count(file) != 0u) continue;
    const BankImage* after = CurrentBank(file);
    // Structural edits within the reserve.
    bool structOnly = options_.structEdits && peerMinor_ >= 1u && after != nullptr;
    for (const ChunkEdit* c : list) structOnly = structOnly && StructOnly(c->fourcc) && !c->removed;
    bool anyShape = false;
    for (const ChunkEdit* c : list) anyShape = anyShape || c->fourcc == ch::BUSS || c->fourcc == ch::EFFX || c->fourcc == ch::SEND;
    if (structOnly && anyShape) {
      const BankImage* old = nullptr;
      for (const BankImage& b : before) {
        if (b.file == file) old = &b;
      }
      const ParsedBank* va = old != nullptr ? old->view() : nullptr;
      const ParsedBank* vb = after->view();
      if (va != nullptr && vb != nullptr) {
        const StructPlan sp = DeriveStructEdits(*va, *vb);
        if (sp.ok) {
          for (const StructPlan::Edit& e : sp.edits) {
            StructEditInfo info;
            info.bank = file;
            info.op = e.op;
            info.target = e.target;
            info.args = e.args;
            info.revision = rev;
            structs_.push_back(info);
            Op op;
            op.route = LiveRoute::StructEdit;
            op.revision = rev;
            op.bank = file;
            op.bankId = after->bankId;
            op.structOp = e.op;
            op.target = e.target;
            op.args = e.args;
            op.structIndex = static_cast<std::int32_t>(structs_.size() - 1u);
            op.guids = ObjectsOf(file);
            Enqueue(std::move(op));
          }
          // The values of the same tables, after the structural edits (D-183). A refused edit
          // sends the bank by reload, which carries them too.
          for (const StructPlan::Value& x : sp.values) {
            Op op;
            op.route = LiveRoute::SetValue;
            op.revision = rev;
            op.bank = file;
            op.bankId = after->bankId;
            op.member = MemberOf(x.chunk, x.column);
            ll::SetValue& v = op.value;
            std::uint64_t lo = 0, hi = 0;
            if (vb->Guid(x.runtimeId, &lo, &hi)) {
              v.guidLo = lo;
              v.guidHi = hi;
              const std::string g = GuidText(Guid{lo, hi});
              if (!g.empty()) op.guids.push_back(g);
            }
            v.runtimeId = x.runtimeId;
            v.chunk = x.chunk;
            v.column = x.column;
            v.valueType = static_cast<std::uint32_t>(ll::ValueType::F32);
            v.index = x.index;
            v.value = x.bits;
            op.cellKey = CellKey(file, v);
            op.row = x.row;
            if (options_.heldByMixer && options_.heldByMixer(v.runtimeId, v.chunk, v.column, v.index)) {
              Mark(op, LiveState::HeldByMixer, 0, "the mixer's monitoring value holds this cell");
              latestSerial_[op.cellKey] = ++serial_;
              held_[op.cellKey] = op;
              continue;
            }
            Enqueue(std::move(op));
          }
          continue;
        }
      }
    }
    for (const ChunkEdit* c : list) {
      const LiveSupport s = LiveSupportOf(c->fourcc);
      if (!s.supported && s.blocking) {
        Unsupported(file, c->fourcc, ~0u, std::string(), true, s.reason);
        Op tmp;
        tmp.route = LiveRoute::ReplaceChunk;
        tmp.bank = file;
        tmp.revision = rev;
        tmp.guids = ObjectsOf(file);
        Mark(tmp, LiveState::Unsupported, SND_ERR_UNSUPPORTED, s.reason);
        continue;
      }
      if (c->removed || !IsTable(c->fourcc) || c->fourcc == ch::NAME || c->fourcc == ch::GUID || c->fourcc == ch::ALIS) {
        reload.insert(file);
        break;
      }
      Op op;
      op.route = LiveRoute::ReplaceChunk;
      op.revision = rev;
      op.bank = file;
      op.bankId = c->bankId;
      op.fourcc = c->fourcc;
      op.bytes = c->bytes;
      op.guids = ObjectsOf(file);
      Enqueue(std::move(op));
    }
  }
  for (const std::string& f : reload) Escalate(f, "a structural change (rows, names, references or data)", rev);
  // Values.
  for (const ValueEdit& e : d.values) {
    const std::string file = e.cell.language.empty() ? e.cell.bank + ".sndb" : e.cell.bank + "." + e.cell.language + ".sndb";
    if (reload.count(file) != 0u) continue;
    Op op;
    op.route = LiveRoute::SetValue;
    op.revision = rev;
    op.bank = file;
    op.bankId = e.cell.bankId;
    op.member = MemberOf(e.cell.chunk, e.cell.column);
    const std::string g = GuidText(e.cell.guid);
    op.guids = g.empty() ? ObjectsOf(file) : std::vector<std::string>{g};
    const LiveSupport s = LiveSupportOf(e.cell.chunk, e.cell.column, e.cell.bits, true);
    if (!s.supported) {
      Unsupported(file, e.cell.chunk, e.cell.column, g, s.blocking, s.reason);
      if (s.blocking) {
        Mark(op, LiveState::Unsupported, SND_ERR_UNSUPPORTED, s.reason);
        continue;
      }
    }
    ll::SetValue& v = op.value;
    v.guidLo = e.cell.guid.lo;
    v.guidHi = e.cell.guid.hi;
    v.runtimeId = e.cell.runtimeId;
    v.chunk = e.cell.chunk;
    v.column = e.cell.column;
    v.valueType = static_cast<std::uint32_t>(e.cell.type == CellType::U64 ? CellType::U32 : e.cell.type);
    v.index = e.cell.index;
    v.value = e.cell.bits;
    op.cellKey = CellKey(file, v);
    op.row = e.cell.row;
    if (options_.heldByMixer && options_.heldByMixer(v.runtimeId, v.chunk, v.column, v.index)) {
      Mark(op, LiveState::HeldByMixer, 0, "the mixer's monitoring value holds this cell");
      latestSerial_[op.cellKey] = ++serial_;  // older values of the cell must not go out either
      held_[op.cellKey] = op;                 // sent when the mixer lets go (D-183, G8)
      continue;
    }
    Enqueue(std::move(op));
  }
}

// --- sessions ------------------------------------------------------------------------------
void LiveSync::OnSessionStarted(Connection& c) {
  sessionLive_ = false;
  banksAsked_ = false;
  queue_.clear();
  inflight_.clear();
  sentValid_ = false;
  unsupported_.clear();
  peerMinor_ = c.peerMinor();
  ll::Client* cl = c.client();
  check_.peerMajor = c.hello().protocolMajor;
  check_.peerMinor = c.hello().protocolMinor;
  check_.helloStatus = c.hello().status;
  check_.gameBuildHash = c.hello().runtimeBuildHash;
  check_.toolBuildHash = current_.buildHash;
  if (cl != nullptr && cl->Supports(ll::MessageType::QueryBanks)) {
    banksSeq_ = cl->QueryBanks(false);
    banksAsked_ = banksSeq_ != 0u;
  }
}

void LiveSync::Reconcile(Connection& c) {
  sent_.clear();
  std::vector<ll::BankEntry> game;
  if (banksAsked_) {
    game.resize(1024);
    std::uint32_t reply = 0;
    const std::uint32_t n = c.client()->CopyBanks(game.data(), 1024u, &reply);
    game.resize(std::min<std::size_t>(n, game.size()));
  }
  check_.valid = true;
  check_.banksKnown = banksAsked_;
  check_.banks.clear();
  check_.note = banksAsked_ ? std::string() : "the game speaks protocol 1.0: no bank list (matched by build hash)";
  if (!current_.ok) {
    sessionLive_ = true;
    return;
  }
  if (banksAsked_) {
    std::vector<bool> used(game.size(), false);
    for (const BankImage& b : current_.banks) {
      const ParsedBank* v = b.view();
      const Id lang = v != nullptr ? v->languageId() : 0u;
      BankMatch m;
      m.file = b.file;
      m.bankId = b.bankId;
      m.languageId = lang;
      m.toolContentHash = b.contentHash;
      m.toolBuildHash = current_.buildHash;
      const ll::BankEntry* e = nullptr;
      for (std::size_t i = 0; i < game.size(); ++i) {
        if (!used[i] && game[i].bankId == b.bankId && game[i].languageId == lang) {
          e = &game[i];
          used[i] = true;
          break;
        }
      }
      if (e == nullptr) {
        m.verdict = BankVerdict::ToolOnly;
        check_.banks.push_back(m);
        sent_.push_back(b);  // the reload loads it (or it is listed once)
        Escalate(b.file, "a bank the game does not have", builtRevision_);
        for (Op& q : queue_) {
          if (q.bank == b.file && q.route == LiveRoute::Reload) q.loadIfAbsent = true;
        }
        continue;
      }
      m.gameContentHash = e->contentHash;
      m.gameBuildHash = e->projectBuildHash;
      m.writable = e->writable != 0u;
      bool liveStruct = false;
      for (const StructEditInfo& x : structs_) liveStruct = liveStruct || (x.bank == b.file && x.applied && !x.undone);
      if (liveStruct) {
        // D-182 / D-183: whether the game kept the live structural edits is not known. They are not
        // sent again (an editor would get a second Add); the bank is reloaded from the document,
        // and a successful reload ends them (reserve() and structEdits() start from zero).
        m.verdict = e->contentHash == b.contentHash ? BankVerdict::Same : BankVerdict::Differs;
        m.knownBase = known_.count(e->contentHash) != 0u;
        sent_.push_back(b);
        check_.banks.push_back(m);
        Escalate(b.file, "live structural edits before the reconnect: the game's structure is not known", builtRevision_);
        continue;
      }
      if (e->contentHash == b.contentHash) {
        m.verdict = BankVerdict::Same;
        m.knownBase = true;
        sent_.push_back(b);
      } else {
        m.verdict = BankVerdict::Differs;
        auto k = known_.find(e->contentHash);
        m.knownBase = k != known_.end();
        if (!m.writable) {
          Escalate(b.file, "the game's bank has no override layer (release build)", builtRevision_);
        } else {
          PlanBank(m.knownBase ? &k->second : nullptr, b, !m.knownBase);
        }
        sent_.push_back(b);
      }
      check_.banks.push_back(m);
    }
    for (std::size_t i = 0; i < game.size(); ++i) {
      if (used[i]) continue;
      BankMatch m;
      m.bankId = game[i].bankId;
      m.languageId = game[i].languageId;
      m.verdict = BankVerdict::GameOnly;
      m.gameContentHash = game[i].contentHash;
      m.gameBuildHash = game[i].projectBuildHash;
      m.writable = game[i].writable != 0u;
      check_.banks.push_back(m);
    }
  } else {
    // 1.0: matched by the build hash only.
    const std::uint64_t h = c.hello().runtimeBuildHash;
    const std::vector<BankImage>* base = nullptr;
    if (h == current_.buildHash) {
      base = &current_.banks;
    } else if (haveBaseline_ && h == baselineHash_) {
      base = &baseline_;
    }
    for (const BankImage& b : current_.banks) {
      BankMatch m;
      m.file = b.file;
      m.bankId = b.bankId;
      m.toolContentHash = b.contentHash;
      m.toolBuildHash = current_.buildHash;
      m.gameBuildHash = h;
      const BankImage* before = nullptr;
      if (base != nullptr) {
        for (const BankImage& x : *base) {
          if (x.file == b.file) before = &x;
        }
      }
      m.knownBase = before != nullptr;
      m.verdict = before != nullptr && before->contentHash == b.contentHash ? BankVerdict::Same : BankVerdict::Differs;
      if (m.verdict == BankVerdict::Differs) PlanBank(before, b, before == nullptr);
      sent_.push_back(b);
      check_.banks.push_back(m);
    }
  }
  // A bank the game lists again (a restarted game, another session) is no longer a failure unless
  // this session's reconcile reloads it again.
  if (banksAsked_) {
    for (const BankMatch& m : check_.banks) {
      if (m.verdict == BankVerdict::Same || m.verdict == BankVerdict::Differs) failedBanks_.erase(m.file);
    }
  }
  // The answer given during the handshake is tied to the game as this session sees it (its bank
  // list may have come after the answer).
  if (rememberedValid_ && rememberPending_) {
    rememberedGame_ = GameSignature();
    rememberPending_ = false;
  }
  ResendSentState();
  sentRevision_ = builtRevision_;
  sentValid_ = true;
  sessionLive_ = true;
  changed_.clear();
}

std::string LiveSync::GameSignature() const {
  std::vector<std::string> parts;
  for (const BankMatch& m : check_.banks) {
    if (m.gameContentHash == 0u) continue;  // not in the game (a ToolOnly bank until a reload loads it)
    parts.push_back(std::to_string(m.bankId) + ":" + std::to_string(m.languageId) + ":" + std::to_string(m.gameContentHash));
  }
  std::sort(parts.begin(), parts.end());
  // With a bank list the banks say it (the Hello hash follows the registration order, which a hot
  // reload changes); a 1.0 game only has its build hash.
  std::string s = check_.banksKnown ? std::string("banks") : std::to_string(check_.gameBuildHash);
  for (const std::string& p : parts) s += "|" + p;
  return s;
}

// D-182: the game may still hold what earlier sessions wrote (11.2: overrides stay when the tool
// leaves), or may have put it back. Every cell and table the tool ever sent goes again with the
// document's current value, so that after a reconnect the game holds the document's build.
void LiveSync::ResendSentState() {
  for (const auto& [key, cell] : sentCells_) {
    bool reloading = false;
    for (const Op& q : queue_) reloading = reloading || (q.bank == cell.bank && q.route == LiveRoute::Reload);
    if (reloading) continue;  // the reload carries the file's values
    bool queued = false;
    for (const Op& q : queue_) queued = queued || (q.route == LiveRoute::SetValue && q.cellKey == key);
    if (queued) continue;
    const BankImage* b = CurrentBank(cell.bank);
    const ParsedBank* v = b != nullptr ? b->view() : nullptr;
    std::uint64_t bits = 0;
    if (v == nullptr || !v->Cell(cell.value.chunk, cell.value.column, cell.row, &bits)) continue;  // the shape changed
    Op op;
    op.route = LiveRoute::SetValue;
    op.revision = builtRevision_;
    op.bank = cell.bank;
    op.bankId = b->bankId;
    op.value = cell.value;
    op.value.value = bits;
    op.cellKey = key;
    op.row = cell.row;
    op.member = MemberOf(cell.value.chunk, cell.value.column);
    const std::string g = GuidText(Guid{cell.value.guidLo, cell.value.guidHi});
    if (!g.empty()) op.guids.push_back(g);
    if (options_.heldByMixer &&
        options_.heldByMixer(op.value.runtimeId, op.value.chunk, op.value.column, op.value.index)) {
      Mark(op, LiveState::HeldByMixer, 0, "the mixer's monitoring value holds this cell");
      held_[key] = op;
      continue;
    }
    Enqueue(std::move(op));
  }
  for (const auto& [bank, list] : sentChunks_) {
    bool reloading = false;
    for (const Op& q : queue_) reloading = reloading || (q.bank == bank && q.route == LiveRoute::Reload);
    const BankImage* b = CurrentBank(bank);
    const ParsedBank* v = b != nullptr ? b->view() : nullptr;
    if (reloading || v == nullptr) continue;
    for (const std::uint32_t fourcc : list) {
      std::uint64_t n = 0;
      const std::uint8_t* data = v->Chunk(fourcc, &n);
      if (data == nullptr) continue;
      Op op;
      op.route = LiveRoute::ReplaceChunk;
      op.revision = builtRevision_;
      op.bank = bank;
      op.bankId = b->bankId;
      op.fourcc = fourcc;
      op.bytes.assign(data, data + n);
      Enqueue(std::move(op));
    }
  }
}

// D-183 (G8): a value the mixer held back goes out once the mixer lets go of the cell.
void LiveSync::ReleaseHeld() {
  std::vector<Op> release;
  for (auto it = held_.begin(); it != held_.end();) {
    const ll::SetValue& v = it->second.value;
    if (options_.heldByMixer && options_.heldByMixer(v.runtimeId, v.chunk, v.column, v.index)) {
      ++it;
    } else {
      release.push_back(it->second);
      it = held_.erase(it);
    }
  }
  for (Op& op : release) Enqueue(std::move(op));
}

// --- sending -----------------------------------------------------------------------------------
void LiveSync::Send(ll::Client* c, std::uint64_t nowNs) {
  (void)nowNs;
  for (;;) {
    if (queue_.empty() || inflight_.size() >= options_.maxInFlight) return;
    bool reloadInFlight = false, chunkInFlight = false;
    for (const Op& o : inflight_) {
      reloadInFlight = reloadInFlight || o.route == LiveRoute::Reload;
      chunkInFlight = chunkInFlight || o.route == LiveRoute::ReplaceChunk || o.route == LiveRoute::StructEdit;
    }
    if (reloadInFlight) return;  // the game answers NOT_READY meanwhile: wait (never shown)
    if (queue_.front().route == LiveRoute::Reload) {
      // The latest build of the bank goes: what was queued for it is in it.
      const std::string bank = queue_.front().bank;
      for (auto it = queue_.begin() + 1; it != queue_.end();) {
        if (it->bank == bank && it->route != LiveRoute::Reload) {
          it = queue_.erase(it);
        } else {
          ++it;
        }
      }
    }
    Op& op = queue_.front();
    if ((op.route == LiveRoute::ReplaceChunk || op.route == LiveRoute::StructEdit) && chunkInFlight) return;
    if (op.route == LiveRoute::Reload && !inflight_.empty()) return;
    std::uint32_t seq = 0;
    switch (op.route) {
      case LiveRoute::SetValue:
        if (options_.heldByMixer && options_.heldByMixer(op.value.runtimeId, op.value.chunk, op.value.column, op.value.index)) {
          Mark(op, LiveState::HeldByMixer, 0, "the mixer's monitoring value holds this cell");
          held_[op.cellKey] = op;
          queue_.pop_front();
          continue;
        }
        seq = c->SetValue(op.value, true);
        if (seq != 0u) sentCells_[op.cellKey] = SentCell{op.bank, op.row, op.value};
        break;
      case LiveRoute::ReplaceChunk:
        seq = c->ReplaceChunk(op.bankId, op.fourcc, op.bytes.data(), op.bytes.size(), true);
        if (seq != 0u) {
          std::vector<std::uint32_t>& list = sentChunks_[op.bank];
          if (std::find(list.begin(), list.end(), op.fourcc) == list.end()) list.push_back(op.fourcc);
        }
        break;
      case LiveRoute::StructEdit:
        seq = c->StructEdit(op.structOp, op.target, op.args.data(), static_cast<std::uint32_t>(op.args.size()), true);
        break;
      case LiveRoute::Reload: {
        Op& o = op;
        const BankImage* b = CurrentBank(o.bank);
        std::string path, err;
        if (b == nullptr || !options_.writer(*b, &path, &err)) {
          const std::string why = "the rebuilt bank could not be written: " + (b == nullptr ? std::string("not in the build") : err);
          Problem(o, SND_ERR_IO, why);
          Mark(o, LiveState::Failed, SND_ERR_IO, why);
          if (o.reloadIndex >= 0) {  // D-184: a failed phase with the writer's reason (the game still has the bank)
            ReloadProgress& p = reloads_[static_cast<std::size_t>(o.reloadIndex)];
            p.finished = true;
            p.result = SND_ERR_IO;
            p.phase = ll::ReloadPhase::Failed;
            p.detached = false;
            p.reason = why;
          }
          failedBanks_[o.bank] = why;
          queue_.pop_front();
          continue;
        }
        o.path = path;
        for (BankImage& s : sent_) {
          if (s.file == o.bank) s = *b;
        }
        if (std::none_of(sent_.begin(), sent_.end(), [&](const BankImage& s) { return s.file == o.bank; })) sent_.push_back(*b);
        known_[b->contentHash] = *b;
        o.contentHash = b->contentHash;
        seq = c->ReloadBank(o.bankId, o.languageId, path.c_str(), o.loadIfAbsent, true);
        if (o.reloadIndex >= 0) {
          reloads_[static_cast<std::size_t>(o.reloadIndex)].path = path;
          reloads_[static_cast<std::size_t>(o.reloadIndex)].seq = seq;
        }
        break;
      }
      case LiveRoute::None: break;
    }
    if (seq == 0u) return;  // not connected or the send queue is full: next Tick
    op.seq = seq;
    if (op.firstSentNs == 0u) op.firstSentNs = nowNs_ == 0u ? 1u : nowNs_;
    ++stats_.sent;
    switch (op.route) {
      case LiveRoute::SetValue: ++stats_.values; break;
      case LiveRoute::ReplaceChunk: ++stats_.chunks; break;
      case LiveRoute::StructEdit: ++stats_.structEdits; break;
      case LiveRoute::Reload: ++stats_.reloads; break;
      case LiveRoute::None: break;
    }
    Mark(op, op.route == LiveRoute::Reload ? LiveState::Reloading : LiveState::InFlight, 0, std::string());
    inflight_.push_back(std::move(op));
    queue_.pop_front();
  }
}

void LiveSync::TakeAnswers(ll::Client* c) {
  ll::ReloadStatus rs[64];
  const std::uint32_t n = c->CopyReloadStatus(rs, 64u);
  for (std::uint32_t i = 0; i < n; ++i) {
    for (ReloadProgress& p : reloads_) {
      if (p.seq == rs[i].requestSeq && !p.finished) p.phase = static_cast<ll::ReloadPhase>(rs[i].phase);
    }
  }
  for (std::size_t i = 0; i < inflight_.size();) {
    std::int32_t r = 0;
    if (!c->TakeAck(inflight_[i].seq, &r)) {
      ++i;
      continue;
    }
    Op op = std::move(inflight_[i]);
    inflight_.erase(inflight_.begin() + static_cast<std::ptrdiff_t>(i));
    Settle(op, r);
  }
}

void LiveSync::Settle(const Op& op, std::int32_t r) {
  if (op.route != LiveRoute::Reload && op.epoch != epoch_[op.bank]) {
    ++stats_.superseded;  // a reload of the bank replaced it
    return;
  }
  if (r == SND_OK) {
    ++stats_.acked;
    Mark(op, LiveState::Live, r, std::string());
    if (op.route == LiveRoute::StructEdit && op.structIndex >= 0) {
      StructEditInfo& s = structs_[static_cast<std::size_t>(op.structIndex)];
      if (s.applied) {
        s.undone = true;  // the answer to an undo
      } else {
        s.applied = true;
        // D-183: a Remove (e.g. the document's Undo of a live Add) gives the Add's cost back.
        const bool removeEffect = s.op == ll::StructOp::RemoveEffect, removeBus = s.op == ll::StructOp::RemoveBus;
        if (removeEffect || removeBus) {
          s.undone = true;
          for (auto it = structs_.rbegin(); it != structs_.rend(); ++it) {
            const bool match = it->bank == s.bank && it->target == s.target && it->applied && !it->undone &&
                               ((removeEffect && it->op == ll::StructOp::AddEffect) || (removeBus && it->op == ll::StructOp::AddBus));
            if (match) {
              it->undone = true;
              break;
            }
          }
        }
      }
    }
    if (op.route == LiveRoute::Reload && op.reloadIndex >= 0) {
      ReloadProgress& p = reloads_[static_cast<std::size_t>(op.reloadIndex)];
      p.finished = true;
      p.result = r;
      p.phase = ll::ReloadPhase::Done;
    }
    if (op.route == LiveRoute::Reload) {
      // The game holds the bank's file again: no live cell, table or structural edit of it is left.
      failedBanks_.erase(op.bank);
      for (auto it = sentCells_.begin(); it != sentCells_.end();) {
        if (it->second.bank == op.bank) {
          it = sentCells_.erase(it);
        } else {
          ++it;
        }
      }
      sentChunks_.erase(op.bank);
      // The game changed by the tool's own reload: still the same game for the remembered answer.
      for (BankMatch& m : check_.banks) {
        if (m.file == op.bank) {
          m.gameContentHash = op.contentHash;  // (the verdict stays what the connection saw)
        }
      }
      if (rememberedValid_ && !rememberPending_) rememberedGame_ = GameSignature();
      // The bank's live structural edits are gone with the reload: the list and the reserve start
      // from zero (D-182). Requests that point at removed entries no longer do.
      std::vector<std::int32_t> remap(structs_.size(), -1);
      std::vector<StructEditInfo> kept;
      for (std::size_t i = 0; i < structs_.size(); ++i) {
        if (structs_[i].bank == op.bank) continue;
        remap[i] = static_cast<std::int32_t>(kept.size());
        kept.push_back(structs_[i]);
      }
      structs_ = std::move(kept);
      auto fix = [&](Op& o) {
        if (o.structIndex >= 0) o.structIndex = remap[static_cast<std::size_t>(o.structIndex)];
      };
      for (Op& o : queue_) fix(o);
      for (Op& o : inflight_) fix(o);
    }
    return;
  }
  const bool chunkSlot = r == SND_ERR_LIMIT_REACHED && op.route == LiveRoute::ReplaceChunk;
  if (Transient(r) || chunkSlot) {
    // D-183: a newer value of the same cell was edited since: the old one is not sent again.
    if (op.route == LiveRoute::SetValue && op.serial < latestSerial_[op.cellKey]) {
      ++stats_.superseded;
      return;
    }
    Op again = op;
    again.seq = 0;
    ++again.resends;
    ++stats_.resent;
    if (nowNs_ > again.firstSentNs && nowNs_ - again.firstSentNs > std::uint64_t{options_.giveUpMs} * 1000000ull) {
      ++stats_.failed;
      Problem(op, r, std::string("the game kept refusing the request as busy (") + ResultName(r) + ") for " +
                         std::to_string(options_.giveUpMs) + " ms");
      Mark(op, LiveState::Failed, r, "busy");
      return;
    }
    Mark(again, LiveState::Queued, 0, std::string());
    queue_.push_front(std::move(again));
    return;
  }
  if (op.route == LiveRoute::Reload) {
    if (op.reloadIndex >= 0) {
      ReloadProgress& p = reloads_[static_cast<std::size_t>(op.reloadIndex)];
      p.finished = true;
      p.result = r;
      p.phase = ll::ReloadPhase::Failed;
      p.detached = r != SND_ERR_NOT_FOUND && r != SND_ERR_LIMIT_REACHED && r != SND_ERR_BANK_IN_USE;
      p.reason = std::string(ResultName(r)) +
                 (p.detached ? ": the bank is detached in the game (not loaded until a reload succeeds)" : "");
      failedBanks_[op.bank] = p.reason;
    } else {
      failedBanks_[op.bank] = ResultName(r);
    }
    ++stats_.failed;
    Problem(op, r,
            std::string("hot reload failed: ") + ResultName(r) +
                (r == SND_ERR_BANK_IN_USE ? " (another loaded bank depends on it)" : "") +
                (r != SND_ERR_NOT_FOUND && r != SND_ERR_BANK_IN_USE ? "; the bank is detached in the game" : ""));
    Mark(op, LiveState::Failed, r, ResultName(r));
    return;
  }
  if ((r == SND_ERR_UNSUPPORTED || r == SND_ERR_OUT_OF_BUDGET ||
       (r == SND_ERR_LIMIT_REACHED && op.route == LiveRoute::StructEdit)) &&
      (op.route == LiveRoute::ReplaceChunk || op.route == LiveRoute::StructEdit || op.route == LiveRoute::SetValue)) {
    if (op.route == LiveRoute::StructEdit && op.structIndex >= 0 && structs_[static_cast<std::size_t>(op.structIndex)].applied) {
      Problem(op, r, std::string("the structural edit could not be undone live: ") + ResultName(r));
      return;
    }
    Escalate(op.bank,
             std::string(LiveRouteName(op.route)) + " refused (" + ResultName(r) + ")" +
                 (r == SND_ERR_OUT_OF_BUDGET ? ": dev reserve exhausted" : ""),
             op.revision, &op.guids);
    return;
  }
  // D-183 (G7): a value of an object a live structural edit added (core keeps no cell for it until
  // its editor does, C-01): it needs a rebuild and hot reload, it is not a problem of the edit.
  if (r == SND_ERR_NOT_FOUND && op.route == LiveRoute::SetValue) {
    for (const StructEditInfo& s : structs_) {
      if (s.applied && !s.undone && s.target == op.value.runtimeId &&
          (s.op == ll::StructOp::AddBus || s.op == ll::StructOp::AddEffect)) {
        const std::string why = "a live-added object: its values need a rebuild and hot reload until core's editor keeps "
                                "cells for it (C-01)";
        Escalate(op.bank, why, op.revision, &op.guids);  // listed as unsupported when no reload is possible
        return;
      }
    }
  }
  ++stats_.failed;
  Problem(op, r, std::string(LiveRouteName(op.route)) + " refused by the game: " + ResultName(r));
  Mark(op, LiveState::Failed, r, ResultName(r));
}

bool LiveSync::reloading() const {
  for (const Op& o : inflight_) {
    if (o.route == LiveRoute::Reload) return true;
  }
  for (const Op& o : queue_) {
    if (o.route == LiveRoute::Reload) return true;
  }
  return false;
}

ReserveUsage LiveSync::reserve() const {
  ReserveUsage u;
  u.limits = options_.reserveLimits;
  for (const StructEditInfo& s : structs_) {
    if (!s.applied || s.undone) continue;
    const ll::StructCost c = ll::StructCostOf(s.op, static_cast<std::uint32_t>(s.args.size()));
    u.buses += c.buses;
    u.effects += c.effects;
    u.bytes += c.bytes;
  }
  return u;
}

bool LiveSync::UndoStructEdit(std::size_t i) {
  if (i >= structs_.size() || !structs_[i].applied || structs_[i].undone) return false;
  const StructEditInfo& s = structs_[i];
  Op op;
  op.route = LiveRoute::StructEdit;
  op.revision = builtRevision_;
  op.bank = s.bank;
  op.target = s.target;
  op.structIndex = static_cast<std::int32_t>(i);
  if (s.op == ll::StructOp::AddBus) {
    op.structOp = ll::StructOp::RemoveBus;
  } else if (s.op == ll::StructOp::AddEffect) {
    // Appended: the last effect of the bus when nothing was added after it.
    std::uint32_t index = 0;
    const BankImage* b = CurrentBank(s.bank);
    const ParsedBank* v = b != nullptr ? b->view() : nullptr;
    const std::uint32_t row = v != nullptr ? v->FindId(ch::BUSS, s.target) : bf::kNone;
    if (row != bf::kNone) {
      const std::uint32_t n = v->U32(ch::BUSS, co::BUSS_effectCount, row);
      index = n == bf::kNone || n == 0u ? 0u : n - 1u;
    }
    op.structOp = ll::StructOp::RemoveEffect;
    op.args = {index};
  } else {
    return false;  // a send has no remove: a hot reload of the bank takes it away
  }
  Enqueue(std::move(op));
  return true;
}

std::vector<LiveFeature> LiveSync::features() const {
  const bool v11 = peerMinor_ >= 1u;
  const std::string why = v11 ? std::string() : "the game speaks protocol 1.0";
  return {
      LiveFeature{"value edits (SetValue)", true, std::string()},
      LiveFeature{"table replacement (ReplaceChunk)", true, std::string()},
      LiveFeature{"bank list at connection (QueryBanks)", v11, why},
      LiveFeature{"hot reload (ReloadBank)", v11 && static_cast<bool>(options_.writer),
                  !v11 ? why : (options_.writer ? std::string() : "no output for rebuilt banks")},
      LiveFeature{"structural edits (StructEdit)", v11, v11 ? "the game answers UNSUPPORTED until core can (C-01)" : why},
      LiveFeature{"item revert (RevertValue)", v11, why},
      LiveFeature{"read-back (ReadCell)", v11, why},
      LiveFeature{"stop all auditions", v11, why},
  };
}

std::vector<LiveItem> LiveSync::items() const {
  std::vector<LiveItem> out;
  for (const auto& [k, v] : items_) out.push_back(v);
  return out;
}

const LiveItem* LiveSync::Find(const std::string& guid, const std::string& member) const {
  auto it = items_.find(guid + "\n" + member);
  return it == items_.end() ? nullptr : &it->second;
}

bool LiveSync::ChooseSendDiffs(Connection& c) {
  if (!c.awaitingSync()) return false;
  decided_ = true;
  refused_ = false;
  // D-182: remembered for a reconnect to the same game only (its build hash and banks).
  rememberedValid_ = true;
  remembered_ = LiveDecision::SendDiffs;
  rememberedGame_ = GameSignature();
  rememberPending_ = true;
  return c.DecideSync(MismatchPolicy::SendDiffs);
}

bool LiveSync::ChooseRefuse(Connection& c) {
  if (!c.awaitingSync()) return false;
  decided_ = true;
  refused_ = true;
  rememberedValid_ = true;
  remembered_ = LiveDecision::Refuse;
  rememberedGame_ = GameSignature();
  rememberPending_ = !check_.banksKnown;  // (a refused session has no reconcile: keep what was seen)
  return c.DecideSync(MismatchPolicy::Refuse);
}

void LiveSync::UpdatePhase(Connection& c) {
  if (c.client() == nullptr) {
    phase_ = LivePhase::Offline;
  } else if (c.awaitingSync()) {
    phase_ = waitingForList_ ? LivePhase::Checking : LivePhase::AwaitingDecision;
  } else if (c.state() == LinkState::Rejected) {
    phase_ = refused_ ? LivePhase::Refused : LivePhase::Offline;
  } else if (c.state() != LinkState::Online) {
    phase_ = LivePhase::Connecting;
  } else if (!sessionLive_) {
    phase_ = LivePhase::Checking;
  } else if (!queue_.empty() || !inflight_.empty() || doc_.revision() != builtRevision_ || !held_.empty()) {
    // (a document change not built yet: the throttle holds it until its interval ends; a value the
    // mixer holds back: the game does not hold the document's value)
    phase_ = held_.empty() || !queue_.empty() || !inflight_.empty() || doc_.revision() != builtRevision_
                 ? LivePhase::Syncing
                 : LivePhase::Live;
    if (phase_ == LivePhase::Live && !failedBanks_.empty()) phase_ = LivePhase::Failed;
  } else if (!failedBanks_.empty()) {
    phase_ = LivePhase::Failed;  // D-184: never Live while a bank did not reach the game
  } else {
    phase_ = LivePhase::Live;
  }
}

void LiveSync::Tick(Connection& c, const ConnectionEvents& ev, std::uint64_t nowNs) {
  nowNs_ = nowNs;
  Build(nowNs);
  // D-182: the Hello of every connection carries the document's current build (the client sends
  // the hash it was opened with: a new client while connecting).
  if (c.client() != nullptr && c.state() == LinkState::Connecting && current_.ok &&
      c.desc().projectBuildHash != current_.buildHash) {
    c.SetProjectBuildHash(current_.buildHash);
  }
  ll::Client* cl = c.client();
  // A new client (Open) or a new connection of the client: what the old one showed is gone.
  const std::uint64_t link = (std::uint64_t{c.opens()} << 32) | (cl != nullptr ? c.generation() : 0u);
  const bool newLink = link != seenLink_;
  seenLink_ = link;
  if (ev.sessionLost || cl == nullptr || newLink) {
    sessionLive_ = false;
    banksAsked_ = false;
    for (const Op& o : inflight_) Mark(o, LiveState::Offline, 0, "the connection dropped");
    for (const Op& o : queue_) Mark(o, LiveState::Offline, 0, "not connected");
    inflight_.clear();
    queue_.clear();
    check_.valid = false;
  }
  if (cl == nullptr) {
    UpdatePhase(c);
    return;
  }
  if (c.awaitingSync()) {
    if (!check_.valid || check_.helloStatus != c.hello().status || !banksAsked_) {
      peerMinor_ = c.peerMinor();
      check_ = SyncCheck{};
      check_.peerMajor = c.hello().protocolMajor;
      check_.peerMinor = c.hello().protocolMinor;
      check_.helloStatus = c.hello().status;
      check_.gameBuildHash = c.hello().runtimeBuildHash;
      check_.toolBuildHash = current_.buildHash;
      check_.valid = true;
      if (cl->Supports(ll::MessageType::QueryBanks)) {
        banksSeq_ = cl->QueryBanks(false);
        banksAsked_ = banksSeq_ != 0u;
      } else {
        check_.note = "the game speaks protocol 1.0: no bank list";
        banksAsked_ = true;  // nothing to wait for
      }
    }
    // Fill the bank verdicts once the list is there (shown while the user decides).
    std::uint32_t reply = 0;
    std::vector<ll::BankEntry> game(1024);
    const std::uint32_t n = cl->CopyBanks(game.data(), 1024u, &reply);
    if (banksSeq_ != 0u && reply == banksSeq_ && !check_.banksKnown) {
      check_.banksKnown = true;
      game.resize(std::min<std::size_t>(n, game.size()));
      for (const BankImage& b : current_.banks) {
        BankMatch m;
        m.file = b.file;
        m.bankId = b.bankId;
        m.toolContentHash = b.contentHash;
        m.toolBuildHash = current_.buildHash;
        m.verdict = BankVerdict::ToolOnly;
        for (const ll::BankEntry& e : game) {
          if (e.bankId != b.bankId) continue;
          m.languageId = e.languageId;
          m.gameContentHash = e.contentHash;
          m.gameBuildHash = e.projectBuildHash;
          m.writable = e.writable != 0u;
          m.verdict = e.contentHash == b.contentHash ? BankVerdict::Same : BankVerdict::Differs;
          m.knownBase = known_.count(e.contentHash) != 0u;
        }
        check_.banks.push_back(m);
      }
      for (const ll::BankEntry& e : game) {
        bool inTool = false;
        for (const BankImage& b : current_.banks) inTool = inTool || b.bankId == e.bankId;
        if (inTool) continue;
        BankMatch m;
        m.bankId = e.bankId;
        m.languageId = e.languageId;
        m.verdict = BankVerdict::GameOnly;
        m.gameContentHash = e.contentHash;
        m.gameBuildHash = e.projectBuildHash;
        m.writable = e.writable != 0u;
        check_.banks.push_back(m);
      }
    }
    LiveDecision d = options_.decision;
    // A 1.1 game: no question and no remembered answer before its bank list is in (the question
    // shows the verdict per bank; the answer given before stands only for the same game, D-182).
    // Until then the phase is Checking. A 1.0 game has no list: its build hash decides at once.
    const bool known = check_.banksKnown || !cl->Supports(ll::MessageType::QueryBanks);
    waitingForList_ = d == LiveDecision::Ask && !known;
    if (d == LiveDecision::Ask && rememberedValid_ && known) {
      d = GameSignature() == rememberedGame_ ? remembered_ : LiveDecision::Ask;
    }
    if (d == LiveDecision::SendDiffs) {
      refused_ = false;
      c.DecideSync(MismatchPolicy::SendDiffs);
    } else if (d == LiveDecision::Refuse) {
      refused_ = true;
      c.DecideSync(MismatchPolicy::Refuse);
    }
    UpdatePhase(c);
    return;
  }
  if (ev.sessionStarted) OnSessionStarted(c);
  if (!sessionLive_ && c.state() == LinkState::Online) {
    if (!banksAsked_ && peerMinor_ >= 1u && cl->Supports(ll::MessageType::QueryBanks)) {
      banksSeq_ = cl->QueryBanks(false);
      banksAsked_ = banksSeq_ != 0u;
    }
    bool ready = !banksAsked_;
    if (banksAsked_) {
      ll::BankEntry tmp[1];
      std::uint32_t reply = 0;
      cl->CopyBanks(tmp, 0u, &reply);
      ready = reply == banksSeq_;
    }
    if (ready) Reconcile(c);
  }
  if (sessionLive_ && sentValid_) {
    if (resync_) {
      resync_ = false;
      sessionLive_ = false;
      OnSessionStarted(c);
    } else if (current_.ok && builtRevision_ != sentRevision_) {
      const LiveDelta d = DiffBuilds(AsBuild(sent_), current_);
      PlanDelta(d, sent_);
      sent_ = current_.banks;
      sentRevision_ = builtRevision_;
      changed_.clear();
    }
    ReleaseHeld();
    TakeAnswers(cl);
    Send(cl, nowNs);
  }
  UpdatePhase(c);
}

}  // namespace snd::tool
