// Contract 1.1 (CONTRACTS.md D-43 .. D-52), internal layer: commands and schema.
#include <cstddef>
#include <cstdint>
#include <cstring>

#include "snd/internal/commands.hpp"
#include "snd/internal/schema.hpp"
#include "snd/test/test.hpp"

namespace s = snd::schema;
namespace cmd = snd::cmd;

SND_TEST(Contract11, DeclareLanguageCommand) {
  const cmd::CommandInfo* info = nullptr;
  for (const cmd::CommandInfo& i : cmd::kCommandInfo) {
    if (i.type == cmd::CommandType::DeclareLanguage) info = &i;
  }
  SND_REQUIRE(info != nullptr);
  SND_CHECK(info->delivery == cmd::Delivery::MustDeliver);
  SND_CHECK(info->origin == cmd::Origin::Game);
  // Appended: the last command type (wire values of 1.0 unchanged).
  SND_CHECK_EQ(static_cast<int>(cmd::CommandType::DeclareLanguage) + 1, static_cast<int>(cmd::CommandType::Count));
  cmd::DeclareLanguage dl{};
  SND_CHECK_EQ(sizeof(dl.name), static_cast<std::size_t>(SND_MAX_LANGUAGE_NAME_BYTES));
}

// D-43 / D-44 / D-45: schema.
SND_TEST(Contract11, SchemaChanges) {
  static_assert(s::ColumnTraits<s::ColumnId::Voice_renderHold>::kWriter == s::SystemId::Render);
  static_assert(s::IsDeclaredRead(s::SystemId::SelectVoices, s::ColumnId::Voice_renderHold));
  static_assert(s::IsDeclaredRead(s::SystemId::BuildRenderList, s::ColumnId::Voice_renderHold));
  static_assert(s::IsDeclaredRead(s::SystemId::RunEventLogic, s::ColumnId::Voice_renderHold));
  static_assert(sizeof(snd::VoiceRenderHold) == 8);
  SND_CHECK(s::Table(s::TableId::Stream).indexing == s::Indexing::Direct);
  bool resampler = false;
  for (const s::ColumnInfo& c : s::kColumns) {
    if (std::strcmp(c.name, "RealVoice.resamplerState") == 0) resampler = true;
  }
  SND_CHECK(!resampler);
}

