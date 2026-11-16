// Row removal for the Sparse tables: the destroying system removes the row
// and moves EVERY column of the row that took its place (3.1 swap-remove).
// Moving another system's column element is part of the row lifecycle, not a
// write of the value (the value stays the same, only its dense position moves).
#include "core_impl.hpp"

namespace snd::core {

#define SND_MOVE_COLUMN(T, name, type, temp, writer) base::rows::MoveElement(tb.col.name, res.dense, res.movedFrom);
#define SND_DEFINE_REMOVE(T)                                                                  \
  base::rows::RemoveResult Remove##T##Row(schema::Tables& t, SlotIndex slot) noexcept {       \
    schema::T##Table& tb = t.T;                                                               \
    const base::rows::RemoveResult res = base::rows::Remove(tb.rows, slot);                   \
    if (res.Moved()) {                                                                        \
      SND_COLS_##T(SND_MOVE_COLUMN)                                                           \
    }                                                                                         \
    return res;                                                                               \
  }
SND_DEFINE_REMOVE(Playback)
SND_DEFINE_REMOVE(Voice)
SND_DEFINE_REMOVE(Emitter)
SND_DEFINE_REMOVE(ParamGlobal)
SND_DEFINE_REMOVE(ParamEmitter)
SND_DEFINE_REMOVE(ParamPlayback)
SND_DEFINE_REMOVE(Bus)
SND_DEFINE_REMOVE(Transport)
SND_DEFINE_REMOVE(Reservation)
SND_DEFINE_REMOVE(Category)
SND_DEFINE_REMOVE(Snapshot)
#undef SND_DEFINE_REMOVE
#undef SND_MOVE_COLUMN

}  // namespace snd::core
