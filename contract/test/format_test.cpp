// Bank format and live link protocol (design.md 10.2, 11.2).
#include <cstdint>
#include <cstring>
#include <vector>

#include "snd/format/bank_format.hpp"
#include "snd/format/livelink_protocol.hpp"
#include "snd/id.hpp"
#include "snd/test/test.hpp"

namespace bf = snd::bank_format;
namespace ll = snd::livelink;

SND_TEST(BankFormat, Crc32cReferenceVector) {
  const char* text = "123456789";
  SND_CHECK_EQ(bf::Crc32c(reinterpret_cast<const std::uint8_t*>(text), 9), 0xE3069283u);
  // Incremental update equals one-shot.
  std::uint32_t c = bf::Crc32cUpdate(0, reinterpret_cast<const std::uint8_t*>(text), 4);
  c = bf::Crc32cUpdate(c, reinterpret_cast<const std::uint8_t*>(text) + 4, 5);
  SND_CHECK_EQ(c, 0xE3069283u);
}

SND_TEST(BankFormat, ColumnsBelongToTableChunks) {
  for (const bf::ColumnInfo& col : bf::kColumns) {
    bool found = false;
    for (const bf::ChunkInfo& ch : bf::kChunks) {
      if (ch.fourcc == col.chunk) {
        found = true;
        SND_CHECK_MSG(ch.kind == bf::ChunkKind::Table, "%s is in a blob chunk", col.name);
      }
    }
    SND_CHECK_MSG(found, "%s: unknown chunk", col.name);
  }
  for (const bf::ChunkInfo& ch : bf::kChunks) {
    if (ch.kind != bf::ChunkKind::Table) continue;
    int n = 0;
    for (const bf::ColumnInfo& col : bf::kColumns) n += col.chunk == ch.fourcc ? 1 : 0;
    SND_CHECK_MSG(n > 0, "table chunk %.4s has no columns", reinterpret_cast<const char*>(&ch.fourcc));
  }
  SND_CHECK_EQ(bf::chunk::EVNT, snd::Fourcc('E', 'V', 'N', 'T'));
  SND_CHECK_EQ(bf::col::ASST_loopEnd, 13u);
}

// Builds a minimal bank in memory exactly as bank-format.md describes and
// parses it back. Serves as the executable example for buildtool / bank.
SND_TEST(BankFormat, MinimalBankRoundTrip) {
  const std::uint32_t rows = 2;
  // Layout: header @0 (128) | chunk table @128 (1 entry, 32) | EVNT chunk @192
  // EVNT: TableChunkHeader(16) + 3 ColumnEntry(48) = 64 -> columns at 64, 128, 192.
  std::vector<std::uint8_t> file(192 + 256, 0);
  bf::FileHeader h{};
  h.magic = bf::kMagic;
  h.formatMajor = bf::kFormatMajor;
  h.formatMinor = bf::kFormatMinor;
  h.platform = bf::kPlatformAny;
  h.endianness = bf::kLittleEndian;
  h.alignLog2 = bf::kAlignLog2;
  h.headerBytes = sizeof(bf::FileHeader);
  h.bankId = snd::HashId("Common");
  h.fileBytes = file.size();
  h.chunkCount = 1;
  h.chunkTableOffset = 128;
  bf::ChunkEntry ce{bf::chunk::EVNT, 1, bf::kChunkTable, 0, 0, 192, 256};
  std::memcpy(file.data() + 128, &ce, sizeof(ce));

  std::uint8_t* chunk = file.data() + 192;
  bf::TableChunkHeader th{rows, 3, 0, 0};
  std::memcpy(chunk, &th, sizeof(th));
  const bf::ColumnEntry cols[3] = {{bf::col::EVNT_id, 4, 0, 64},
                                   {bf::col::EVNT_priority, 4, 0, 128},
                                   {bf::col::EVNT_retriggerMs, 4, 0, 192}};
  std::memcpy(chunk + 16, cols, sizeof(cols));
  const std::uint32_t ids[rows] = {snd::HashId("SE/A"), snd::HashId("SE/B")};
  const std::uint32_t prio[rows] = {10, 200};
  const float retrig[rows] = {0.0f, 50.0f};
  std::memcpy(chunk + 64, ids, sizeof(ids));
  std::memcpy(chunk + 128, prio, sizeof(prio));
  std::memcpy(chunk + 192, retrig, sizeof(retrig));

  h.checksum = bf::Crc32c(file.data() + h.headerBytes, file.size() - h.headerBytes);
  h.headerChecksum = bf::Crc32c(reinterpret_cast<const std::uint8_t*>(&h), 52);
  std::memcpy(file.data(), &h, sizeof(h));

  // --- parse --------------------------------------------------------------
  bf::FileHeader r;
  std::memcpy(&r, file.data(), sizeof(r));
  SND_REQUIRE_EQ(r.magic, bf::kMagic);
  SND_CHECK_EQ(bf::Crc32c(file.data(), 52), r.headerChecksum);
  SND_CHECK_EQ(bf::Crc32c(file.data() + r.headerBytes, r.fileBytes - r.headerBytes), r.checksum);
  bf::ChunkEntry re;
  std::memcpy(&re, file.data() + r.chunkTableOffset, sizeof(re));
  SND_REQUIRE_EQ(re.fourcc, bf::chunk::EVNT);
  SND_CHECK_EQ(re.offset % bf::kAlignment, 0u);
  const std::uint8_t* c = file.data() + re.offset;
  bf::TableChunkHeader rt;
  std::memcpy(&rt, c, sizeof(rt));
  SND_REQUIRE_EQ(rt.rowCount, rows);
  const std::uint32_t* idCol = nullptr;
  for (std::uint32_t i = 0; i < rt.columnCount; ++i) {
    bf::ColumnEntry e;
    std::memcpy(&e, c + sizeof(rt) + i * sizeof(e), sizeof(e));
    SND_CHECK_EQ(e.offset % bf::kAlignment, 0u);
    if (e.columnId == bf::col::EVNT_id) idCol = reinterpret_cast<const std::uint32_t*>(c + e.offset);
  }
  SND_REQUIRE(idCol != nullptr);
  SND_CHECK_EQ(idCol[1], snd::HashId("SE/B"));
}

SND_TEST(LiveLink, MessagesAreKnownAndSized) {
  const ll::MessageType all[] = {
      ll::MessageType::Hello,        ll::MessageType::HelloReply,   ll::MessageType::SyncDecision,
      ll::MessageType::Ping,         ll::MessageType::Pong,         ll::MessageType::Ack,
      ll::MessageType::Error,        ll::MessageType::SetValue,     ll::MessageType::ReplaceChunk,
      ll::MessageType::Audition,     ll::MessageType::OverrideParam, ll::MessageType::OverrideState,
      ll::MessageType::SetProfilerMask, ll::MessageType::RecordControl, ll::MessageType::StructEdit,
      ll::MessageType::VoiceList,    ll::MessageType::BlockTiming,  ll::MessageType::MemoryUsage,
      ll::MessageType::StreamStall,  ll::MessageType::LodChange,    ll::MessageType::EventLog,
      ll::MessageType::BusMeters,    ll::MessageType::RecordData};
  for (ll::MessageType t : all) {
    SND_CHECK(ll::IsKnown(static_cast<std::uint16_t>(t)));
    SND_CHECK(ll::FixedPayloadBytes(t) > 0u);
    SND_CHECK_EQ(ll::FixedPayloadBytes(t) % 4u, 0u);
  }
  SND_CHECK(ll::IsKnown(static_cast<std::uint16_t>(ll::MessageType::ClearOverrides)));
  SND_CHECK(!ll::IsKnown(0x7777));
  SND_CHECK_EQ(sizeof(ll::FrameHeader), 16u);
  SND_CHECK_EQ(ll::kMagic, snd::Fourcc('S', 'N', 'L', 'L'));
}
