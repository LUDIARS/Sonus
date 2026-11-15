// Corruption cases shared by the runtime validation (bank/test: ParseBank) and
// buildtool's self-check of written banks (buildtool/test: SelfCheckBank), so
// that both implementations of bank-format.md §8 reject the same inputs.
// Each case edits fixture::RichBank() and/or the built bytes; `resign` re-signs
// the file afterwards so that the structural check itself is reached.
#ifndef SND_BANK_TEST_CORRUPTION_CASES_HPP
#define SND_BANK_TEST_CORRUPTION_CASES_HPP

#include <cstring>
#include <functional>
#include <vector>

#include "fixture_writer.hpp"
#include "snd/types.h"

namespace corruption {

namespace bf = snd::bank_format;
namespace c = bf::col;
namespace ch = bf::chunk;

struct Case {
  const char* name;
  std::function<void(fixture::Bank&)> edit;              // may be empty
  std::function<void(std::vector<std::uint8_t>&)> bytes;  // may be empty
  bool resign;
  snd_result expected;
};

template <class F>
std::function<void(std::vector<std::uint8_t>&)> Header(F f) {
  return [f](std::vector<std::uint8_t>& b) {
    bf::FileHeader h;
    std::memcpy(&h, b.data(), sizeof(h));
    f(h);
    std::memcpy(b.data(), &h, sizeof(h));
  };
}

inline std::vector<Case> Cases() {
  using B = fixture::Bank;
  std::vector<Case> v;
  auto add = [&](const char* n, std::function<void(B&)> e, std::function<void(std::vector<std::uint8_t>&)> b,
                 bool resign, snd_result r) { v.push_back(Case{n, std::move(e), std::move(b), resign, r}); };
  // §8.1 / 8.2 header and checksums
  add("header byte 60 (chunkCount) not re-signed", nullptr, [](std::vector<std::uint8_t>& b) { b[60] ^= 1; }, false,
      SND_ERR_CORRUPT);
  add("languageId not re-signed", nullptr, [](std::vector<std::uint8_t>& b) { b[80] ^= 1; }, false, SND_ERR_CORRUPT);
  add("body bit flip", nullptr, [](std::vector<std::uint8_t>& b) { b[b.size() - 70] ^= 0x40; }, false, SND_ERR_CORRUPT);
  add("magic", nullptr, Header([](bf::FileHeader& h) { h.magic = 0x12345678; }), true, SND_ERR_CORRUPT);
  add("formatMajor 3", nullptr, Header([](bf::FileHeader& h) { h.formatMajor = 3; }), true, SND_ERR_VERSION_MISMATCH);
  add("endianness", nullptr, Header([](bf::FileHeader& h) { h.endianness = 2; }), true, SND_ERR_VERSION_MISMATCH);
  add("alignLog2", nullptr, Header([](bf::FileHeader& h) { h.alignLog2 = 4; }), true, SND_ERR_CORRUPT);
  add("headerBytes", nullptr, Header([](bf::FileHeader& h) { h.headerBytes = 132; }), true, SND_ERR_CORRUPT);
  add("reserved byte", nullptr, Header([](bf::FileHeader& h) { h.reserved[7] = 1; }), true, SND_ERR_CORRUPT);
  add("reserved0", nullptr, Header([](bf::FileHeader& h) { h.reserved0 = 1; }), true, SND_ERR_CORRUPT);
  add("languageId without flag", nullptr, Header([](bf::FileHeader& h) { h.languageId = 5; }), true, SND_ERR_CORRUPT);
  add("fileBytes", nullptr, Header([](bf::FileHeader& h) { h.fileBytes += 64; }), true, SND_ERR_CORRUPT);
  add("trailing padding", nullptr, [](std::vector<std::uint8_t>& b) { b.push_back(0); }, true, SND_ERR_CORRUPT);
  // §8.3 tables of contents
  add("self dependency", [](B& b) { b.deps.push_back(bf::Dependency{b.bankId, 0, 1}); }, nullptr, true, SND_ERR_CORRUPT);
  add("chunk table out of range", nullptr, Header([](bf::FileHeader& h) { h.chunkTableOffset = h.fileBytes; }), true,
      SND_ERR_CORRUPT);
  add("chunk misaligned", [](B& b) { b.Find(ch::WAVE)->offsetOverride = 130; }, nullptr, true, SND_ERR_CORRUPT);
  add("duplicate chunk", [](B& b) { b.Blob(ch::WAVE, std::vector<std::uint8_t>(4, 0)); }, nullptr, true, SND_ERR_CORRUPT);
  add("table flag on a blob", [](B& b) { b.Find(ch::WAVE)->flags = bf::kChunkTable; }, nullptr, true, SND_ERR_CORRUPT);
  add("unknown layout version of a required chunk", [](B& b) { b.Find(ch::EVNT)->version = 2; }, nullptr, true,
      SND_ERR_VERSION_MISMATCH);
  // §8.4 tables
  add("duplicate column", [](B& b) { b.Find(ch::EVNT)->columns.push_back(fixture::U32(c::EVNT_bus, {0, 0, 0})); },
      nullptr, true, SND_ERR_CORRUPT);
  add("elemBytes", [](B& b) { b.Find(ch::EVNT)->columns.push_back(fixture::U64(c::EVNT_retriggerMs, {0, 0, 0})); },
      nullptr, true, SND_ERR_CORRUPT);
  add("required column missing", [](B& b) { b.RemoveColumn(ch::EVNT, c::EVNT_bus); }, nullptr, true, SND_ERR_CORRUPT);
  add("rowCount beyond columns", [](B& b) { b.Find(ch::EVNT)->rows = 1000; }, nullptr, true, SND_ERR_CORRUPT);
  // §8.5 references and 2.0 order rules
  add("row reference", [](B& b) { b.SetU32(ch::EVNT, c::EVNT_bus, 0, 3); }, nullptr, true, SND_ERR_CORRUPT);
  add("range", [](B& b) { b.SetU32(ch::EVNT, c::EVNT_trackCount, 0, 4); }, nullptr, true, SND_ERR_CORRUPT);
  add("clip kind", [](B& b) { b.SetU32(ch::CLIP, c::CLIP_kind, 3, 7); }, nullptr, true, SND_ERR_CORRUPT);
  add("bus graph order", [](B& b) { b.SetU32(ch::BUSS, c::BUSS_parent, 1, 0); }, nullptr, true, SND_ERR_CORRUPT);
  add("nested event before its owner (self)", [](B& b) { b.SetU32(ch::CLIP, c::CLIP_target, 2, 0); }, nullptr, true,
      SND_ERR_CORRUPT);
  add("category parent cycle", [](B& b) { b.Find(ch::CATG)->columns.push_back(fixture::U32(c::CATG_parent, {1, 0})); },
      nullptr, true, SND_ERR_CORRUPT);
  add("seek table not starting at 0", [](B& b) { b.SetU64(ch::SEEK, c::SEEK_frame, 0, 5); }, nullptr, true,
      SND_ERR_CORRUPT);
  add("seek table not increasing", [](B& b) { b.SetU64(ch::SEEK, c::SEEK_frame, 1, 0); }, nullptr, true,
      SND_ERR_CORRUPT);
  add("blob range", [](B& b) { b.SetU64(ch::ASST, c::ASST_dataBytes, 0, 321); }, nullptr, true, SND_ERR_CORRUPT);
  add("unterminated stream path", [](B& b) { b.Find(ch::SPTH)->blob = fixture::Bytes("x", false); }, nullptr, true,
      SND_ERR_CORRUPT);
  add("stem channels", [](B& b) { b.SetU32(ch::MSTM, c::MSTM_channelCount, 0, 3); }, nullptr, true, SND_ERR_CORRUPT);
  add("ALIS not sorted",
      [](B& b) { b.Table(ch::ALIS, 2, {fixture::U32(c::ALIS_oldId, {9, 3}), fixture::U32(c::ALIS_id, {1, 1})}); },
      nullptr, true, SND_ERR_CORRUPT);
  add("ALIS duplicate oldId",
      [](B& b) { b.Table(ch::ALIS, 2, {fixture::U32(c::ALIS_oldId, {3, 3}), fixture::U32(c::ALIS_id, {1, 2})}); },
      nullptr, true, SND_ERR_CORRUPT);
  add("ALIS id 0", [](B& b) { b.Table(ch::ALIS, 1, {fixture::U32(c::ALIS_oldId, {3}), fixture::U32(c::ALIS_id, {0})}); },
      nullptr, true, SND_ERR_CORRUPT);
  add("ALIS oldId 0", [](B& b) { b.Table(ch::ALIS, 1, {fixture::U32(c::ALIS_oldId, {0}), fixture::U32(c::ALIS_id, {4})}); },
      nullptr, true, SND_ERR_CORRUPT);
  add("ALIS self", [](B& b) { b.Table(ch::ALIS, 1, {fixture::U32(c::ALIS_oldId, {7}), fixture::U32(c::ALIS_id, {7})}); },
      nullptr, true, SND_ERR_CORRUPT);
  add("DSPN size", [](B& b) { b.Find(ch::DSPN)->blob.resize(8); }, nullptr, true, SND_ERR_CORRUPT);
  add("PHYS size", [](B& b) { b.Find(ch::PHYS)->blob.resize(40); }, nullptr, true, SND_ERR_CORRUPT);
  return v;
}

// The bytes of one case.
inline std::vector<std::uint8_t> Build(const Case& k) {
  fixture::Bank b = fixture::RichBank();
  if (k.edit) k.edit(b);
  std::vector<std::uint8_t> bytes = b.Build();
  if (k.bytes) k.bytes(bytes);
  if (k.resign) fixture::Bank::Rehash(bytes);
  return bytes;
}

}  // namespace corruption

#endif  // SND_BANK_TEST_CORRUPTION_CASES_HPP
