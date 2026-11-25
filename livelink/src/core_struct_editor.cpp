#include <bit>

#include "snd/core/core.hpp"
#include "snd/livelink/struct_edit.hpp"
#include "snd/livelink/wire.hpp"

namespace snd::livelink {
namespace {
class CoreEditor final : public IStructEditor {
 public:
  snd_result Apply(core::System& sys, const StructEditRequest& e) noexcept override {
    const StructEdit header{static_cast<std::uint32_t>(e.op), e.targetId, e.argCount, 0u};
    if (!e.token || CheckStructArgs(header, e.args) != SND_OK) return SND_ERR_INVALID_ARGUMENT;
    auto u = [&](std::size_t i) { return static_cast<std::uint32_t>(e.args[i]); };
    auto f = [&](std::size_t i) { return std::bit_cast<float>(u(i)); };
    switch (e.op) {
      case StructOp::AddBus: return sys.LiveAddBus(e.targetId, u(0), u(1), e.argCount == 3 ? f(2) : 0, e.token);
      case StructOp::RemoveBus: return sys.LiveRemoveBus(e.targetId, e.token);
      case StructOp::AddEffect: {
        float params[kMaxStructEffectParams];
        for (std::uint32_t i = 2; i < e.argCount; ++i) params[i - 2] = f(i);
        return sys.LiveAddEffect(e.targetId, u(0), u(1), params, e.argCount - 2, e.token, e.prepared);
      }
      case StructOp::RemoveEffect: return sys.LiveRemoveEffect(e.targetId, u(0), e.token);
      case StructOp::AddSend: return sys.LiveAddSend(e.targetId, u(0), f(1), e.argCount == 3 && u(2), e.token);
    }
    return SND_ERR_INVALID_ARGUMENT;
  }
  std::uint64_t OwnerBank(core::System& sys, const StructEditRequest& e) noexcept override {
    return sys.LiveBusBank(e.op == StructOp::AddBus ? static_cast<Id>(e.args[0]) : e.targetId);
  }
  std::uint64_t RemovalOrigin(core::System& sys, const StructEditRequest& e) noexcept override {
    const bool fx = e.op == StructOp::RemoveEffect;
    return sys.LiveOrigin(e.targetId, fx, fx ? static_cast<std::uint32_t>(e.args[0]) : 0);
  }
  snd_result Undo(core::System& sys, const StructEditRequest& e) noexcept override {
    return sys.LiveUndoEdit(e.token);
  }
};
}  // namespace

IStructEditor& CoreStructEditor() noexcept {
  static CoreEditor editor;  // instantiated by the composition root, before rendering
  return editor;
}
}  // namespace snd::livelink
