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
}

std::int32_t CellSync::Find(const CellAddress& a) const {
  for (std::size_t i = 0; i < addresses_.size(); ++i) {
    if (addresses_[i] == a) return static_cast<std::int32_t>(i);
  }
  return -1;
}

std::uint32_t CellSync::Count(CellState st) const {
  std::uint32_t n = 0;
  for (const CellStatus& s : cells_) n += s.state == st ? 1u : 0u;
  for (const CellStatus& s : params_) n += s.state == st ? 1u : 0u;
  return n;
}

void CellSync::Update(livelink::Client* client, const std::vector<Cell>& cells, const std::vector<ParamCell>& params,
                      bool sessionStarted, bool sendAll) {
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

  for (CellStatus& s : cells_) TakeAck(client, s, stats_);
  for (CellStatus& s : params_) TakeAck(client, s, stats_);

  if (sessionStarted) {
    for (std::size_t i = 0; i < cells.size(); ++i) {
      CellStatus& s = cells_[i];
      s.seq = 0;  // acks of the previous session will not come
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
    if (client != nullptr) seq = client->SetValue(Encode(cells[i].address, v), true);
    s.sent = v;
    if (seq == 0u) {
      s.state = CellState::Unsent;
      continue;
    }
    s.seq = seq;
    s.state = CellState::Pending;
    s.everSent = true;
    ++stats_.sent;
  }
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
    s.state = CellState::Pending;
    s.everSent = true;
    ++stats_.sent;
  }
}

}  // namespace snd::tool
