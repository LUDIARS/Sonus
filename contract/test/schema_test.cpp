// Table schema and column ownership (design.md 3.1, 3.2).
#include <cstring>

#include "snd/internal/schema.hpp"
#include "snd/test/test.hpp"

namespace s = snd::schema;
using s::ColumnId;
using s::SystemId;
using s::TableId;

SND_TEST(Schema, EveryTableHasColumnsAndValidLifecycle) {
  for (const s::TableInfo& t : s::kTables) {
    int columns = 0;
    for (const s::ColumnInfo& c : s::kColumns) {
      if (c.table == t.id) ++columns;
    }
    SND_CHECK_MSG(columns > 0, "table %s has no columns", t.name);
    SND_CHECK(s::IsAudioThreadSystem(t.rowCreator));
    SND_CHECK(s::IsAudioThreadSystem(t.rowDestroyer));
  }
}

SND_TEST(Schema, ValidationPasses) {
  SND_CHECK_EQ(s::ValidateReads(), -1);
  SND_CHECK_EQ(s::ValidateColumns(), -1);
}

SND_TEST(Schema, DesignTableAssignments) {
  // 3.2: key writers named in the design.
  static_assert(s::ColumnTraits<ColumnId::ParamGlobal_target>::kWriter == SystemId::ApplyCommands);
  static_assert(s::ColumnTraits<ColumnId::ParamPlayback_current>::kWriter == SystemId::EvaluateLogicParams);
  static_assert(s::ColumnTraits<ColumnId::Transport_position>::kWriter == SystemId::UpdateTransport);
  static_assert(s::ColumnTraits<ColumnId::Playback_state>::kWriter == SystemId::RunEventLogic);
  static_assert(s::ColumnTraits<ColumnId::Voice_gain>::kWriter == SystemId::EvaluateDspParams);
  static_assert(s::ColumnTraits<ColumnId::Voice_realSlot>::kWriter == SystemId::SelectVoices);
  static_assert(s::ColumnTraits<ColumnId::Voice_playhead>::kWriter == SystemId::Render);
  static_assert(s::ColumnTraits<ColumnId::Spatial_distance>::kWriter == SystemId::Compute3D);
  static_assert(s::ColumnTraits<ColumnId::Bus_peak>::kWriter == SystemId::Render);
  // 3.2 "前ブロックの値": SelectVoices reads the previous block's bus gain.
  static_assert(s::IsDeclaredRead(SystemId::SelectVoices, ColumnId::Bus_gain));
  static_assert(s::kCanCreateRows<SystemId::RunEventLogic, TableId::Voice>);
  static_assert(!s::kCanCreateRows<SystemId::Render, TableId::Voice>);
  SND_CHECK(true);
}

SND_TEST(Schema, TypedAccessCompilesForOwnersAndDeclaredReaders) {
  s::Tables tables{};
  float gains[4] = {};
  s::ColumnTraits<ColumnId::Bus_gain>::Slot(tables) = gains;
  float* w = s::WriteColumn<SystemId::Render, ColumnId::Bus_gain>(tables);
  w[1] = 0.5f;
  const float* r = s::ReadColumn<SystemId::EvaluateDspParams, ColumnId::Bus_gain>(tables);
  SND_CHECK_EQ(r[1], 0.5f);
  const float* own = s::ReadColumn<SystemId::Render, ColumnId::Bus_gain>(tables);
  SND_CHECK(own == gains);
  s::TableRows& rows = s::CreatorRows<SystemId::ApplyCommands, TableId::Emitter>(tables);
  SND_CHECK_EQ(rows.capacity, 0u);
}

SND_TEST(Schema, ExternalWritersOnlyOnAtomicColumns) {
  int external = 0;
  for (const s::ColumnInfo& c : s::kColumns) {
    if (s::IsExternal(c.writer)) {
      ++external;
      SND_CHECK_MSG(c.atomic, "%s", c.name);
    }
  }
  SND_CHECK_EQ(external, 2);  // Stream.readPos (IO), Stream.ringWrite (decode)
}

SND_TEST(Schema, EverySystemParticipates) {
  for (int sys = 1; sys <= 10; ++sys) {
    bool writes = false;
    for (const s::ColumnInfo& c : s::kColumns) writes |= static_cast<int>(c.writer) == sys;
    bool reads = false;
    for (const s::ReadInfo& rd : s::kReads) reads |= static_cast<int>(rd.reader) == sys;
    bool frame = false;
    for (const s::FrameDataInfo& f : s::kFrameData) frame |= static_cast<int>(f.owner) == sys;
    SND_CHECK_MSG(writes || reads || frame, "system %s unused",
                  s::SystemName(static_cast<SystemId>(sys)));
  }
}

SND_TEST(Schema, ColumnNamesAreUnique) {
  const std::size_t n = sizeof(s::kColumns) / sizeof(s::kColumns[0]);
  for (std::size_t i = 0; i < n; ++i) {
    for (std::size_t j = i + 1; j < n; ++j) {
      SND_CHECK(std::strcmp(s::kColumns[i].name, s::kColumns[j].name) != 0);
    }
  }
}
