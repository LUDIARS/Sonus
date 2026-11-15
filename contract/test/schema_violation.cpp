// MUST NOT COMPILE (ctest: contract_schema_violation_rejected, WILL_FAIL).
// Render is not the writer of Playback.state (RunEventLogic is, design.md 3.2).
#include "snd/internal/schema.hpp"

int main() {
  snd::schema::Tables tables{};
  auto* state = snd::schema::WriteColumn<snd::schema::SystemId::Render,
                                         snd::schema::ColumnId::Playback_state>(tables);
  return state == nullptr ? 0 : 1;
}
