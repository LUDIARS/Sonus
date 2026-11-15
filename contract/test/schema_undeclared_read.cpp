// MUST NOT COMPILE (ctest: contract_undeclared_read_rejected, WILL_FAIL).
// Compute3D does not declare a read of Voice.gain in SND_SCHEMA_READS.
#include "snd/internal/schema.hpp"

int main() {
  const snd::schema::Tables tables{};
  const float* gain = &snd::schema::ReadColumn<snd::schema::SystemId::Compute3D,
                                               snd::schema::ColumnId::Voice_gain>(tables)->start;
  return gain == nullptr ? 0 : 1;
}
