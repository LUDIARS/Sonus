// Structural edits within Config.dev_reserve (design.md 11.2; protocol 1.1 §8.4).
//
// The live link does everything around a structural edit: the message rules
// (wire.hpp CheckStructArgs), the reserve accounting (LiveRuntime: buses,
// effects, bytes of Config.dev_reserve), the command that carries it to the
// audio thread (a LiveReplaceChunk tagged kStructEditTag whose PendingChunk
// holds StructEdit + args), the session rules (D-83, D-87), the Ack, the
// recording (RecordLiveData) and the disconnect revert (undo in reverse order).
//
// The game composition, preview and replay host install CoreStructEditor.
// A host can supply its own editor or keep the null-editor hot reload fallback.
//
// Both functions run on the audio thread at a block boundary (inside
// CoreDesc::liveCommand): principle 1 applies (no allocation, no lock, no IO).
#ifndef SND_LIVELINK_STRUCT_EDIT_HPP
#define SND_LIVELINK_STRUCT_EDIT_HPP

#include <cstdint>

#include "snd/format/livelink_protocol.hpp"
#include "snd/types.h"

namespace snd::core {
class System;
struct PreparedLivePlugin;
}  // namespace snd::core

namespace snd::livelink {

// One validated structural edit (CheckStructArgs passed). `args` stays valid during the call.
struct StructEditRequest {
  StructOp op = StructOp::AddBus;
  std::uint32_t targetId = 0;
  std::uint32_t argCount = 0;
  std::uint32_t session = 0;  // live link session that sent it (restored from recorded producer markers)
  const std::uint64_t* args = nullptr;
  std::uint64_t token = 0;  // unique within the bound runtime, retained for Undo
  core::PreparedLivePlugin* prepared = nullptr;  // worker-owned storage, transferred on success
};

class IStructEditor {
 public:
  virtual ~IStructEditor() = default;
  // Applies the edit. SND_OK, or why not (SND_ERR_UNSUPPORTED: rebuild and hot reload;
  // SND_ERR_NOT_FOUND: unknown target; ...). A failed edit must leave core unchanged.
  virtual snd_result Apply(core::System& sys, const StructEditRequest& edit) noexcept = 0;
  virtual std::uint64_t OwnerBank(core::System&, const StructEditRequest&) noexcept { return 0; }
  // Token of the addition a Remove would cancel; zero for an authored row.
  // Legacy editors without identity support retain their original matching rules.
  virtual std::uint64_t RemovalOrigin(core::System&, const StructEditRequest&) noexcept { return ~std::uint64_t{0}; }
  // Undoes an edit this editor applied earlier (disconnect revert, newest first). For a
  // Remove the editor restores what it removed if it can; the result is only counted.
  // Effects and sends that went away with a removed live bus are never undone.
  virtual snd_result Undo(core::System& sys, const StructEditRequest& edit) noexcept = 0;
};

IStructEditor& CoreStructEditor() noexcept;

// What an edit takes from Config.dev_reserve (LiveRuntime::StructCostOf).
//   AddBus: 1 bus, AddEffect: 1 effect, AddSend: no unit; every Add takes
//   kStructEditBaseBytes + 8 * argCount bytes. Removes take nothing; removing an object
//   a live edit added gives the whole cost of that Add back (units and bytes) once the
//   remove is applied, and removing a live bus also gives back the live effects on it and
//   the live sends from it: they leave the revert log with it (D-172, no Undo is asked for
//   them). Every edit also takes one place of the revert log at acceptance
//   (RuntimeDesc::maxStructLog); an edit that does not fit is refused (LIMIT_REACHED).
//   A remove is accepted with a full log: if it cancels a live add it needs no place;
//   otherwise the audio thread takes one when it applies it, or answers LIMIT_REACHED
//   without asking the editor.
inline constexpr std::uint32_t kStructEditBaseBytes = 64;
struct StructCost {
  std::uint32_t buses = 0;
  std::uint32_t effects = 0;
  std::uint64_t bytes = 0;
};
StructCost StructCostOf(StructOp op, std::uint32_t argCount) noexcept;

}  // namespace snd::livelink

#endif  // SND_LIVELINK_STRUCT_EDIT_HPP
