// Mixer values -> game (SetValue / OverrideParam), confirmed by Ack.
#include "snd/tool/cell_sync.hpp"

#include <cstring>

#include "snd/livelink/client.hpp"
#include "snd/types.h"

namespace snd::tool {
namespace {

livelink::SetValue Encode(const CellAddress& a, float value) {
  livelink::SetValue v{};
  v.guidLo = a.guid.lo;
  v.guidHi = a.guid.hi;
  v.runtimeId = a.runtimeId;
  v.chunk = a.chunk;
  v.column = a.column;
  v.valueType = static_cast<std::uint32_t>(livelink::ValueType::F32);
  v.index = a.index;
  std::uint32_t bits = 0;
  std::memcpy(&bits, &value, 4);
  v.value = bits;
  return v;
}

// Step 1 for one cell.
void TakeAck(livelink::Client* client, CellStatus& s, SyncStats& stats) {
  if (client == nullptr || s.seq == 0u) return;
  std::int32_t result = 0;
  if (!client->TakeAck(s.seq, &result)) return;
  s.seq = 0;
  if (result == SND_OK) {
    s.confirmed = s.sent;
    if (s.state == CellState::Pending) s.state = CellState::Synced;
    ++stats.acked;
  } else if (result == SND_ERR_COMMAND_BUFFER_FULL) {
    s.state = CellState::Unsent;
    ++stats.resent;
  } else {
    s.state = CellState::Failed;
    s.error = result;
    stats.lastError = result;
    ++stats.refused;
  }
}

}  // namespace

void CellSync::Reset() {
  addresses_.clear();
  cells_.clear();
  params_.clear();
  stats_ = SyncStats{};
  readAll_ = false;
  readNext_ = 0;
  reverts_ = 0;
}

bool CellSync::RequestRevert(const CellAddress& address) {
  const std::int32_t i = Find(address);
  if (i < 0) return false;
  CellStatus& s = cells_[static_cast<std::size_t>(i)];
  s.revertRequested = true;
  if (s.state != CellState::Pending) s.state = CellState::Unsent;  // sent at the next Update
  return true;
}

bool CellSync::RequestReadBack() {
  if (cells_.empty()) return false;
  readAll_ = true;
  readNext_ = 0;
  for (CellStatus& s : cells_) {
    if (s.readState != CellStatus::Read::Pending) s.readState = CellStatus::Read::None;
  }
  return true;
}

std::uint32_t CellSync::ReadsOutstanding() const {
  std::uint32_t n = 0;
  for (std::size_t i = 0; i < cells_.size(); ++i) {
    const CellStatus& s = cells_[i];
    if (s.readState == CellStatus::Read::Pending) ++n;
    else if (readAll_ && i >= readNext_) ++n;
  }
  return n;
}

void CellSync::UpdateReads(livelink::Client* client) {
  // Answers first (they free places of the client's CellValue buffer).
  for (CellStatus& s : cells_) {
    if (s.readState != CellStatus::Read::Pending || s.readSeq == 0u) continue;
    if (client == nullptr) {
      s.readState = CellStatus::Read::None;
      s.readSeq = 0;
      continue;
    }
    livelink::CellValue v{};
    if (!client->TakeCell(s.readSeq, &v)) continue;
    s.readSeq = 0;
    s.readResult = v.result;
    s.readCells = v.cells;
    if (v.result != SND_OK) {
      s.readState = CellStatus::Read::Failed;
      continue;
    }
    const std::uint32_t cur = static_cast<std::uint32_t>(v.current), orig = static_cast<std::uint32_t>(v.original);
    std::memcpy(&s.readCurrent, &cur, 4);
    std::memcpy(&s.readOriginal, &orig, 4);
    s.readOverridden = (v.flags & livelink::kCellOverridden) != 0u;
    s.readState = CellStatus::Read::Done;
  }
  if (!readAll_ || client == nullptr || !client->Supports(livelink::MessageType::ReadCell)) return;
  std::uint32_t pending = 0;
  for (const CellStatus& s : cells_) pending += s.readState == CellStatus::Read::Pending ? 1u : 0u;
  while (readNext_ < cells_.size() && pending < kReadBatch) {
    CellStatus& s = cells_[readNext_];
    const std::uint32_t seq = client->ReadCell(Encode(addresses_[readNext_], 0.0f), false);
    if (seq == 0u) return;  // not connected / queue full: next Update
    s.readSeq = seq;
    s.readState = CellStatus::Read::Pending;
    ++pending;
    ++readNext_;
  }
  if (readNext_ >= cells_.size()) readAll_ = false;
}

std::int32_t CellSync::Find(const CellAddress& a) const {
  for (std::size_t i = 0; i < addresses_.size(); ++i) {
    if (addresses_[i] == a) return static_cast<std::int32_t>(i);
  }
  return -1;
}

std::uint32_t CellSync::NoAnswer(std::uint64_t nowNs) const {
  std::uint32_t n = 0;
  for (const CellStatus& s : cells_) n += IsNoAnswer(s, nowNs) ? 1u : 0u;
  for (const CellStatus& s : params_) n += IsNoAnswer(s, nowNs) ? 1u : 0u;
  return n;
}

std::uint32_t CellSync::Count(CellState st) const {
  std::uint32_t n = 0;
  for (const CellStatus& s : cells_) n += s.state == st ? 1u : 0u;
  for (const CellStatus& s : params_) n += s.state == st ? 1u : 0u;
  return n;
}

void CellSync::Update(livelink::Client* client, const std::vector<Cell>& cells, const std::vector<ParamCell>& params,
                      bool sessionStarted, bool sendAll, std::uint64_t nowNs) {
  // A different cell layout (new project) starts over from the bank values.
  bool same = addresses_.size() == cells.size();
  for (std::size_t i = 0; same && i < cells.size(); ++i) same = addresses_[i] == cells[i].address;
  if (!same) {
    addresses_.clear();
    cells_.clear();
    for (const Cell& c : cells) {
      addresses_.push_back(c.address);
      CellStatus s;
      s.sent = s.confirmed = c.bankValue;
      cells_.push_back(s);
    }
  }
  if (params_.size() != params.size()) {
    params_.assign(params.size(), CellStatus{});
    for (std::size_t i = 0; i < params.size(); ++i) params_[i].sent = params_[i].confirmed = params[i].value;
  }

  for (CellStatus& s : cells_) {
    TakeAck(client, s, stats_);
    if (s.revertRequested && s.state == CellState::Synced) s.state = CellState::Unsent;  // asked while in flight
  }
  for (CellStatus& s : params_) TakeAck(client, s, stats_);

  if (sessionStarted) {
    for (std::size_t i = 0; i < cells.size(); ++i) {
      CellStatus& s = cells_[i];
      s.seq = 0;  // acks of the previous session will not come
      if (s.readState == CellStatus::Read::Pending) {  // nor its CellValues
        s.readState = CellStatus::Read::None;
        s.readSeq = 0;
      }
      if (sendAll || s.everSent || cells[i].value != cells[i].bankValue) {
        if (s.everSent) ++stats_.resent;
        s.state = CellState::Unsent;
      }
    }
    for (std::size_t i = 0; i < params.size(); ++i) {
      CellStatus& s = params_[i];
      s.seq = 0;
      if (s.everSent || params[i].enabled) s.state = CellState::Unsent;
    }
  }

  for (std::size_t i = 0; i < cells.size(); ++i) {
    CellStatus& s = cells_[i];
    const float v = cells[i].value;
    const bool changed = v != s.sent;
    if (!changed && s.state != CellState::Unsent) continue;
    if (changed && s.state == CellState::Failed) s.state = CellState::Unsent;
    std::uint32_t seq = 0;
    // 1.1 item revert: the model put the value back to the bank value; the game forgets the cell.
    const bool revert = s.revertRequested && v == cells[i].bankValue && client != nullptr &&
                        client->Supports(livelink::MessageType::RevertValue);
    if (client != nullptr) {
      seq = revert ? client->RevertValue(Encode(cells[i].address, v), true)
                   : client->SetValue(Encode(cells[i].address, v), true);
    }
    s.sent = v;
    if (seq != 0u) {
      s.revertedLive = revert;
      s.revertRequested = false;
      if (revert) ++reverts_;
    }
    if (seq == 0u) {
      s.state = CellState::Unsent;
      continue;
    }
    s.seq = seq;
    s.sentNs = nowNs;
    s.state = CellState::Pending;
    s.everSent = true;
    ++stats_.sent;
  }
  UpdateReads(client);
  for (std::size_t i = 0; i < params.size(); ++i) {
    CellStatus& s = params_[i];
    const float v = params[i].value;
    const bool changed = v != s.sent || params[i].enabled != s.enabled;
    if (!changed && s.state != CellState::Unsent) continue;
    if (!s.everSent && !params[i].enabled && !s.enabled) {
      s.sent = v;  // nothing to override yet: the game uses its own value
      s.state = CellState::Synced;
      continue;
    }
    std::uint32_t seq = 0;
    if (client != nullptr) seq = client->OverrideParam(params[i].paramId, v, params[i].enabled, true);
    s.sent = v;
    s.enabled = params[i].enabled;
    if (seq == 0u) {
      s.state = CellState::Unsent;
      continue;
    }
    s.seq = seq;
    s.sentNs = nowNs;
    s.state = CellState::Pending;
    s.everSent = true;
    ++stats_.sent;
  }
}

}  // namespace snd::tool
