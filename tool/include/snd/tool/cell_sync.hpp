// Sends the mixer's values to the game and confirms them by Ack (design.md 11.2).
//
// Update() is called once per UI frame with the model's cells (MixerModel::Cells,
// same order every time):
//   1. Acks are taken for the frames in flight: SND_OK confirms the value;
//      SND_ERR_COMMAND_BUFFER_FULL (the game's command arena was full) re-sends it;
//      any other result marks the cell Failed with that result (not re-sent until
//      the value changes again).
//   2. At the start of a session (Connection::Poll sessionStarted) every cell that
//      differs from the bank or was ever sent is re-sent (the game may have restarted;
//      SetValue is idempotent), and with `sendAll` (BuildMismatch with SendDiffs)
//      every cell is sent.
//   3. Every cell whose value differs from the last value sent is sent (SetValue,
//      wantAck). Moves of a fader within one frame therefore become one message.
//      A send the client could not queue (seq 0) stays Unsent and is retried.
// Parameter overrides (OverrideParam) follow the same rules.
// A refused value is not sent again in the same session until it changes; a new
// session sends it again (step 2).
// No answer: a cell Pending for more than kAckTimeoutNs is shown as "no answer"
// (NoAnswer()); nothing is re-sent for it (a new session re-sends, step 2).
//
// Protocol 1.1 (group P; livelink-protocol.md §8.2):
//   - Item revert: RequestRevert(address) marks a cell whose value the model has put back to
//     the bank value (MixerModel::Revert / RevertOnly). The next send of that cell is a
//     RevertValue (the game's override layer forgets the cell) instead of a SetValue of the bank
//     value, when the game is 1.1 and the cell's value is its bank value; otherwise (a 1.0 game,
//     or the value moved on, e.g. an Undo before the send) the ordinary SetValue goes out. The
//     Ack rules are the same. Undo after a revert is an ordinary edit (SetValue).
//   - Read-back: RequestReadBack() asks the game for every cell (ReadCell, at most
//     kReadBatch per Update, always fewer than the client keeps: kClientCellsKept). Each answer
//     fills the cell's `read*` fields: what the game holds now, the bank value it remembers and
//     whether it is overridden. A 1.0 game: nothing is asked (readState stays None).
// Nothing changes for a 1.0 game: the rules above are the stage-2 rules.
#ifndef SND_TOOL_CELL_SYNC_HPP
#define SND_TOOL_CELL_SYNC_HPP

#include <cstdint>
#include <vector>

#include "snd/tool/mixer.hpp"

namespace snd::livelink {
class Client;
}

namespace snd::tool {

enum class CellState : std::uint8_t {
  Synced = 0,   // the game confirmed the current value (or never needed it)
  Pending = 1,  // sent, Ack not yet received
  Unsent = 2,   // must be sent (not connected, or re-send)
  Failed = 3,   // the game refused the value (error)
};

struct CellStatus {
  CellState state = CellState::Synced;
  float sent = 0.0f;       // last value sent (or the bank value before any send)
  float confirmed = 0.0f;  // last value the game acknowledged
  std::uint32_t seq = 0;   // frame in flight
  std::int32_t error = 0;  // snd_result of the last refusal
  bool everSent = false;
  bool enabled = false;    // parameter overrides only
  std::uint64_t sentNs = 0;  // when the frame in flight was sent (Update's nowNs)
  // 1.1 (group P)
  bool revertRequested = false;  // RequestRevert: the next send is a RevertValue (if still the bank value)
  bool revertedLive = false;     // the last send was a RevertValue
  enum class Read : std::uint8_t { None = 0, Pending = 1, Done = 2, Failed = 3 } readState = Read::None;
  std::uint32_t readSeq = 0;
  std::int32_t readResult = 0;   // CellValue.result (NOT_FOUND: the game has no such cell)
  float readCurrent = 0.0f;      // what the game holds now
  float readOriginal = 0.0f;     // the bank value the game remembers
  bool readOverridden = false;   // the game's override layer holds an edit of the cell
  std::uint32_t readCells = 0;   // banks holding the cell
};
inline constexpr std::uint32_t kReadBatch = 64;
inline constexpr std::uint64_t kAckTimeoutNs = 2000000000ull;

struct SyncStats {
  std::uint64_t sent = 0;
  std::uint64_t acked = 0;
  std::uint64_t resent = 0;   // COMMAND_BUFFER_FULL or a new session
  std::uint64_t refused = 0;  // Failed
  std::int32_t lastError = 0;
};

class CellSync {
 public:
  // client may be null (offline): nothing is sent, cells become Unsent when they change.
  void Update(livelink::Client* client, const std::vector<Cell>& cells, const std::vector<ParamCell>& params,
              bool sessionStarted, bool sendAll, std::uint64_t nowNs = 0);
  void Reset();  // forget everything (new project)

  const std::vector<CellStatus>& cells() const { return cells_; }
  const std::vector<CellStatus>& params() const { return params_; }
  // Index of `address` in the last cells passed to Update, -1 if none.
  std::int32_t Find(const CellAddress& address) const;
  std::uint32_t Count(CellState s) const;
  // Pending cells sent more than kAckTimeoutNs before nowNs.
  std::uint32_t NoAnswer(std::uint64_t nowNs) const;
  static bool IsNoAnswer(const CellStatus& s, std::uint64_t nowNs) {
    return s.state == CellState::Pending && nowNs > s.sentNs && nowNs - s.sentNs > kAckTimeoutNs;
  }
  const SyncStats& stats() const { return stats_; }

  // --- 1.1 (group P) ------------------------------------------------------------------------
  // Marks `address` for an item revert (see the header). false if no such cell.
  bool RequestRevert(const CellAddress& address);
  // Reads every cell back from the game over the next Updates (ReadCell). false if no cells.
  bool RequestReadBack();
  // Cells still to ask or waiting for their CellValue.
  std::uint32_t ReadsOutstanding() const;
  std::uint64_t reverts() const { return reverts_; }  // RevertValue sent

 private:
  void UpdateReads(livelink::Client* client);
  std::vector<CellAddress> addresses_;
  std::vector<CellStatus> cells_;
  std::vector<CellStatus> params_;
  SyncStats stats_;
  bool readAll_ = false;
  std::size_t readNext_ = 0;
  std::uint64_t reverts_ = 0;
};

}  // namespace snd::tool

#endif  // SND_TOOL_CELL_SYNC_HPP
