// Independent self-check of contract 1.1 (CONTRACTS.md D-43 .. D-53, §2 minor rules).
//  - results: SND_ERR_BANK_ALREADY_LOADED appended (unique, after every 1.0 code);
//  - snd_config: the 1.1 fields come after every 1.0 member (1.0 is a prefix),
//    defaults 8 / 4.0 in C and C++, every top-level field converts C++ -> C;
//  - snd_effect_desc: sample_rate + reserved at the end, size a multiple of 8;
//  - commands: DeclareLanguage appended, must-deliver, name copied (NUL fits);
//  - schema: Voice.renderHold (Render, hot, read as Previous), Stream Direct with
//    atomic external-writer columns, no RealVoice.resamplerState, reads valid.
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <type_traits>

#include "snd/config.h"
#include "snd/ext.h"
#include "snd/id.hpp"
#include "snd/internal/commands.hpp"
#include "snd/internal/schema.hpp"
#include "snd/sonus.h"
#include "snd/test/test.hpp"
#include "snd/types.h"

namespace s = snd::schema;
namespace cmd = snd::cmd;

SND_TEST(IndepContract11, ResultCodesAreUniqueAndAppended) {
  const snd_result codes[] = {SND_OK,
                              SND_ERR_INVALID_ARGUMENT,
                              SND_ERR_OUT_OF_BUDGET,
                              SND_ERR_NO_DEVICE,
                              SND_ERR_WRONG_THREAD,
                              SND_ERR_THREAD_NOT_REGISTERED,
                              SND_ERR_COMMAND_BUFFER_FULL,
                              SND_ERR_INVALID_HANDLE,
                              SND_ERR_NOT_FOUND,
                              SND_ERR_BANK_IN_USE,
                              SND_ERR_BANK_DEPENDENCY,
                              SND_ERR_IO,
                              SND_ERR_CORRUPT,
                              SND_ERR_VERSION_MISMATCH,
                              SND_ERR_LIMIT_REACHED,
                              SND_ERR_UNSUPPORTED,
                              SND_ERR_NOT_IMPLEMENTED,
                              SND_ERR_NOT_READY,
                              SND_ERR_BANK_ALREADY_LOADED};
  const std::size_t n = sizeof(codes) / sizeof(codes[0]);
  for (std::size_t i = 0; i < n; ++i) {
    SND_CHECK_EQ(codes[i], -static_cast<snd_result>(i));  // dense, appended in order
    for (std::size_t j = i + 1; j < n; ++j) SND_CHECK(codes[i] != codes[j]);
  }
}

SND_TEST(IndepContract11, ConfigOneDotZeroLayoutIsAPrefix) {
  // Every 1.0 member ends before the first 1.1 field.
  const std::size_t first11 = offsetof(snd_config, max_voice_channels);
  SND_CHECK(offsetof(snd_config, plugins) + sizeof(snd_plugin_config) <= first11);
  SND_CHECK(offsetof(snd_config, output) < first11);
  SND_CHECK(offsetof(snd_config, livelink) < first11);
  SND_CHECK(offsetof(snd_config, memory) < first11);
  SND_CHECK(offsetof(snd_config, max_pitch_ratio) > first11);
  snd_config c;
  std::memset(&c, 0x5A, sizeof(c));
  snd_config_init(&c);
  SND_CHECK_EQ(c.struct_size, static_cast<std::uint32_t>(sizeof(snd_config)));
  SND_CHECK_EQ(c.contract_version, static_cast<std::uint32_t>(SND_CONTRACT_VERSION));
  SND_CHECK_EQ(c.max_voice_channels, 8u);
  SND_CHECK_EQ(c.max_pitch_ratio, 4.0f);
  SND_CHECK(c.max_voice_channels >= 1u && c.max_voice_channels <= SND_MAX_CHANNELS);
  SND_CHECK(c.max_pitch_ratio >= 1.0f && c.max_pitch_ratio <= SND_MAX_PITCH_RATIO);
  // The 1.1 defaults are the documented ones (CONTRACTS.md §3 table) and come from the X macro.
#define SND_INDEP_CHECK_DEFAULT(cname, cppname, ct, cppt, def) SND_CHECK(c.cname == static_cast<ct>(def));
  SND_CONFIG_TOP_1_1(SND_INDEP_CHECK_DEFAULT)
  SND_CONFIG_TOP(SND_INDEP_CHECK_DEFAULT)
#undef SND_INDEP_CHECK_DEFAULT
}

SND_TEST(IndepContract11, EffectDescLayout) {
  static_assert(offsetof(snd_effect_desc, sample_rate) >= offsetof(snd_effect_desc, blob_bytes) + sizeof(std::uint64_t));
  static_assert(offsetof(snd_effect_desc, reserved) == offsetof(snd_effect_desc, sample_rate) + 4);
  static_assert(sizeof(snd_effect_desc) % 8 == 0);
  static_assert(sizeof(snd_effect_desc) == offsetof(snd_effect_desc, reserved) + 4);
  static_assert(std::is_trivially_copyable_v<snd_effect_desc>);
  SND_CHECK(true);
}

SND_TEST(IndepContract11, DeclareLanguageCommandCarriesTheName) {
  const cmd::CommandInfo* info = nullptr;
  std::size_t count = 0;
  for (const cmd::CommandInfo& i : cmd::kCommandInfo) {
    ++count;
    if (i.type == cmd::CommandType::DeclareLanguage) info = &i;
  }
  SND_REQUIRE(info != nullptr);
  SND_CHECK(info->delivery == cmd::Delivery::MustDeliver);
  static_assert(sizeof(cmd::DeclareLanguage::name) == SND_MAX_LANGUAGE_NAME_BYTES);
  static_assert(SND_MAX_LANGUAGE_NAME_BYTES - 1 >= 31);  // 1..31 bytes + NUL
  static_assert(cmd::kWireSize<cmd::DeclareLanguage> >= sizeof(cmd::DeclareLanguage));
  static_assert(cmd::kWireSize<cmd::DeclareLanguage> % 8 == 0);
  // Language ids are snd_hash_id(name) (D-50).
  SND_CHECK_EQ(snd_hash_id("ja"), snd::HashId("ja"));
}

SND_TEST(IndepContract11, SchemaChangesOfOneDotOne) {
  static_assert(s::ValidateReads() == -1);
  using H = s::ColumnTraits<s::ColumnId::Voice_renderHold>;
  static_assert(H::kWriter == s::SystemId::Render);
  const s::ColumnInfo& hold = s::Column(s::ColumnId::Voice_renderHold);
  SND_CHECK(hold.temperature == s::Temperature::Hot);
  SND_CHECK_EQ(hold.elemBytes, 8u);
  SND_CHECK(!hold.atomic);
  // Stream: Direct, external writers only on atomic columns.
  SND_CHECK(s::Table(s::TableId::Stream).indexing == s::Indexing::Direct);
  for (const s::ColumnInfo& c : s::kColumns) {
    if (s::IsExternal(c.writer)) {
      SND_CHECK_MSG(c.atomic, "%s has an external writer but is not atomic", c.name);
      SND_CHECK_MSG(c.table == s::TableId::Stream, "%s: external writer outside the Direct Stream table", c.name);
    }
    SND_CHECK_MSG(std::strcmp(c.name, "RealVoice.resamplerState") != 0, "RealVoice.resamplerState still present");
  }
  SND_CHECK(s::Column(s::ColumnId::Stream_readPos).writer == s::SystemId::ExternalIo);
  SND_CHECK(s::Column(s::ColumnId::Stream_ringWrite).writer == s::SystemId::ExternalDecode);
  // Voice and RealVoice stay Sparse (swap-remove): see the D-43 note in the report.
  SND_CHECK(s::Table(s::TableId::Voice).indexing == s::Indexing::Sparse);
  SND_CHECK(s::Table(s::TableId::RealVoice).indexing == s::Indexing::Sparse);
}
