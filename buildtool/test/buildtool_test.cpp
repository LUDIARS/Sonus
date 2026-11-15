// buildtool unit tests: JSON, WAV, conversion, bank writer, the full build of
// the sample project, errors, determinism and the differential build.
// Banks are inspected with a reader built only from the contract's format
// definition (bank_format.hpp); the full parse by the bank module is in
// bank/test/roundtrip_test.cpp.
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "sample_project.hpp"
#include "snd/buildtool/bank_writer.hpp"
#include "snd/buildtool/build.hpp"
#include "snd/buildtool/json.hpp"
#include "snd/buildtool/wav.hpp"
#include "snd/ext.h"
#include "snd/id.hpp"
#include "snd/test/test.hpp"

namespace bf = snd::bank_format;
namespace bt = snd::buildtool;
namespace fs = std::filesystem;
using snd::HashId;

namespace {

// --- contract-only bank reader --------------------------------------------------------------
struct Chunk {
  bf::ChunkEntry e{};
  const std::uint8_t* p = nullptr;
};
struct Bank {
  std::vector<std::uint8_t> bytes;
  bf::FileHeader h{};
  std::map<std::uint32_t, Chunk> chunks;
  bool ok = false;

  explicit Bank(std::vector<std::uint8_t> b) : bytes(std::move(b)) {
    if (bytes.size() < 128) return;
    std::memcpy(&h, bytes.data(), sizeof(h));
    if (h.magic != bf::kMagic || h.fileBytes != bytes.size()) return;
    if (bf::Crc32c(bytes.data(), 52) != h.headerChecksum) return;
    if (bf::Crc32c(bytes.data() + 128, bytes.size() - 128) != h.checksum) return;
    for (std::uint32_t i = 0; i < h.chunkCount; ++i) {
      Chunk c;
      std::memcpy(&c.e, bytes.data() + h.chunkTableOffset + i * sizeof(bf::ChunkEntry), sizeof(c.e));
      if (c.e.offset % 64 != 0 || c.e.offset + c.e.bytes > bytes.size()) return;
      c.p = bytes.data() + c.e.offset;
      chunks[c.e.fourcc] = c;
    }
    ok = true;
  }
  bool Has(std::uint32_t f) const { return chunks.count(f) != 0; }
  std::uint32_t Rows(std::uint32_t f) const {
    auto it = chunks.find(f);
    if (it == chunks.end()) return 0;
    bf::TableChunkHeader th;
    std::memcpy(&th, it->second.p, sizeof(th));
    return th.rowCount;
  }
  // Column cell as raw u64 (U32/F32 zero-extended), or `def` when the column is absent.
  std::uint64_t Cell(std::uint32_t f, std::uint16_t col, std::uint32_t row, std::uint64_t def = 0) const {
    auto it = chunks.find(f);
    if (it == chunks.end()) return def;
    bf::TableChunkHeader th;
    std::memcpy(&th, it->second.p, sizeof(th));
    for (std::uint32_t i = 0; i < th.columnCount; ++i) {
      bf::ColumnEntry ce;
      std::memcpy(&ce, it->second.p + 16 + i * 16, sizeof(ce));
      if (ce.columnId != col) continue;
      std::uint64_t v = 0;
      std::memcpy(&v, it->second.p + ce.offset + std::uint64_t{row} * ce.elemBytes, ce.elemBytes);
      return v;
    }
    return def;
  }
  float CellF(std::uint32_t f, std::uint16_t col, std::uint32_t row, float def = 0) const {
    std::uint32_t defBits;
    std::memcpy(&defBits, &def, 4);
    const auto bits = static_cast<std::uint32_t>(Cell(f, col, row, defBits));
    float v;
    std::memcpy(&v, &bits, 4);
    return v;
  }
  std::uint32_t FindRow(std::uint32_t f, std::uint16_t col, std::uint32_t value) const {
    for (std::uint32_t r = 0; r < Rows(f); ++r) {
      if (Cell(f, col, r) == value) return r;
    }
    return bf::kNone;
  }
};

bt::BuildOptions Options(const fs::path& proj, const std::string& out) {
  bt::BuildOptions o;
  o.projectDir = proj.string();
  o.outDir = (proj.parent_path() / out).string();
  return o;
}

std::string DumpDiag(const bt::Diagnostics& d) {
  std::string s;
  for (const bt::Diagnostic& x : d.items()) s += bt::Diagnostics::Format(x) + "\n";
  return s;
}

bool BuildOk(const bt::BuildOptions& o, bt::BuildResult* r, bt::Diagnostics* d) {
  const bool ok = bt::Build(o, *d, r);
  if (!ok) snd::test::Note("%s", DumpDiag(*d).c_str());
  return ok;
}

// Writes one extra object file into a project.
void AddObject(const fs::path& proj, const std::string& rel, const std::string& json) {
  sample::WriteText(proj / rel, json);
}

}  // namespace

// ---------------------------------------------------------------------------
SND_TEST(BuildtoolJson, ParsesAndReportsPositions) {
  bt::json::Value v;
  bt::json::ParseError e;
  SND_REQUIRE(bt::json::Parse(R"({"a": [1, -2.5e3, true, null], "b": {"c": "xé😀"},
    "big": 18446744073709551615})", &v, &e));
  SND_CHECK(v.Find("a")->items.size() == 4);
  double d = 0;
  SND_CHECK(v.Find("a")->items[1].AsDouble(&d) && d == -2500.0);
  std::uint64_t u = 0;
  SND_CHECK(v.Find("big")->AsU64(&u) && u == UINT64_MAX);
  SND_CHECK(!v.Find("a")->items[1].AsU64(&u));
  SND_CHECK(v.Find("b")->Find("c")->text == "x\xC3\xA9\xF0\x9F\x98\x80");
  SND_CHECK_EQ(v.Find("b")->line, 1u);
  SND_CHECK_EQ(v.Find("big")->line, 2u);
  SND_CHECK(!bt::json::Parse("{\n  \"a\": 1,\n  \"a\": 2\n}", &v, &e));
  SND_CHECK_EQ(e.line, 3u);
  SND_CHECK(e.message.find("duplicate") != std::string::npos);
  SND_CHECK(!bt::json::Parse("{\"a\": [1, 2,]}", &v, &e));
  SND_CHECK(!bt::json::Parse("{\"a\": \"unterminated}", &v, &e));
  SND_CHECK(!bt::json::Parse("[1] 2", &v, &e));
  std::string deep(1000, '[');
  SND_CHECK(!bt::json::Parse(deep, &v, &e));
  // Round trip through the writer.
  SND_REQUIRE(bt::json::Parse(R"({"k": ["s\n", 3, {"z": false}]})", &v, &e));
  bt::json::Value w;
  SND_REQUIRE(bt::json::Parse(bt::json::Write(v), &w, &e));
  SND_CHECK(bt::json::Write(w) == bt::json::Write(v));
}

SND_TEST(BuildtoolWav, RoundTripsAllFormatsAndLoops) {
  bt::PcmData src = sample::Tone(2, 22050, 300, 1000);
  src.hasLoop = true;
  src.loopStart = 10;
  src.loopEnd = 250;
  for (bt::WavSampleFormat f : {bt::WavSampleFormat::Int16, bt::WavSampleFormat::Int24, bt::WavSampleFormat::Int32,
                                bt::WavSampleFormat::Float32}) {
    bt::PcmData back;
    std::string err;
    SND_REQUIRE(bt::ReadWav(bt::WriteWav(src, f), &back, &err));
    SND_CHECK_EQ(back.channels, 2u);
    SND_CHECK_EQ(back.sampleRate, 22050u);
    SND_CHECK_EQ(back.frames, 300u);
    SND_CHECK(back.hasLoop && back.loopStart == 10 && back.loopEnd == 250);
    double maxErr = 0;
    for (std::size_t i = 0; i < src.samples.size(); ++i) {
      maxErr = std::max(maxErr, static_cast<double>(std::fabs(src.samples[i] - back.samples[i])));
    }
    SND_CHECK(maxErr < 1e-4);
  }
  bt::PcmData out;
  std::string err;
  SND_CHECK(!bt::ReadWav(std::vector<std::uint8_t>(10, 0), &out, &err));
  std::vector<std::uint8_t> bad = bt::WriteWav(src, bt::WavSampleFormat::Int16);
  bad[20] = 2;  // format tag 2 (MS ADPCM)
  SND_CHECK(!bt::ReadWav(bad, &out, &err));
  SND_CHECK(err.find("format tag") != std::string::npos);
}

SND_TEST(BuildtoolWriter, OmitsDefaultColumnsAndSignsTheFile) {
  bt::TableBuilder ev(bf::chunk::EVNT);
  const std::uint32_t r = ev.AddRow();
  ev.U32(bf::col::EVNT_id, r, HashId("A"));
  bt::TableBuilder bus(bf::chunk::BUSS);
  bus.U32(bf::col::BUSS_id, bus.AddRow(), 1);
  bt::BankWriter w;
  w.AddTable(ev);
  w.AddTable(bus);
  w.AddTable(bt::TableBuilder(bf::chunk::CLIP));  // empty: omitted
  w.AddBlob(bf::chunk::WAVE, {1, 2, 3});
  bt::BankHeaderInfo hi;
  hi.bankId = HashId("B");
  Bank b(w.Finish(hi));
  SND_REQUIRE(b.ok);
  SND_CHECK(!b.Has(bf::chunk::CLIP));
  SND_CHECK_EQ(b.Rows(bf::chunk::EVNT), 1u);
  SND_CHECK_EQ(b.Cell(bf::chunk::EVNT, bf::col::EVNT_priority, 0), 128u);  // required, default written
  SND_CHECK_EQ(b.Cell(bf::chunk::EVNT, bf::col::EVNT_category, 0, 77), 77u);  // optional default: absent
  SND_CHECK_EQ(b.h.contentHash, w.ContentHash(hi));
  // Chunks follow the kChunks order: EVNT before BUSS before WAVE.
  SND_CHECK(b.chunks[bf::chunk::EVNT].e.offset < b.chunks[bf::chunk::BUSS].e.offset);
  SND_CHECK(b.chunks[bf::chunk::BUSS].e.offset < b.chunks[bf::chunk::WAVE].e.offset);
}

// ---------------------------------------------------------------------------
SND_TEST(BuildtoolBuild, SampleProject) {
  const fs::path proj = sample::MakeProject("bt-sample");
  bt::BuildOptions o = Options(proj, "out");
  o.platform = bf::kPlatformLinux;
  bt::BuildResult r;
  bt::Diagnostics d;
  SND_REQUIRE(BuildOk(o, &r, &d));
  SND_CHECK_EQ(r.banks.size(), 4u);  // Common, Level1, Voice.ja, Voice.en (dependencies first)
  SND_CHECK(r.banks[0].path == "Common.sndb");
  SND_CHECK(d.Contains("can never match"));  // Battle -> Explore after "* -> *"
  SND_CHECK(d.Contains("DSP state size provider"));
  SND_CHECK(!d.Contains("entry preload"));  // seg asset: stream with entryFrame 0 -> representable
  const fs::path out = o.outDir;
  Bank common(sample::ReadFile(out / "Common.sndb"));
  Bank level(sample::ReadFile(out / "Level1.sndb"));
  Bank ja(sample::ReadFile(out / "Voice.ja.sndb"));
  SND_REQUIRE(common.ok && level.ok && ja.ok);
  SND_CHECK_EQ(level.h.platform, bf::kPlatformLinux);
  SND_CHECK_EQ(level.h.bankId, HashId("Level1"));
  SND_CHECK((level.h.flags & bf::kFileDevBuild) != 0);
  SND_CHECK_EQ(level.h.projectBuildHash, r.projectBuildHash);
  // Dependency on Common with Common's content hash.
  SND_REQUIRE_EQ(level.h.dependencyCount, 1u);
  bf::Dependency dep;
  std::memcpy(&dep, level.bytes.data() + level.h.dependencyOffset, sizeof(dep));
  SND_CHECK_EQ(dep.bankId, HashId("Common"));
  SND_CHECK_EQ(dep.buildHash, common.h.contentHash);
  // Language bank.
  SND_CHECK((ja.h.flags & bf::kFileLanguageBank) != 0);
  SND_CHECK_EQ(ja.h.languageId, HashId("ja"));
  SND_CHECK_EQ(ja.h.bankId, HashId("Voice"));
  // Buses in graph order: Reverb is fed by SFX's send, Master last.
  SND_REQUIRE_EQ(common.Rows(bf::chunk::BUSS), 3u);
  SND_CHECK_EQ(common.Cell(bf::chunk::BUSS, bf::col::BUSS_id, 0), HashId("SFX"));
  SND_CHECK_EQ(common.Cell(bf::chunk::BUSS, bf::col::BUSS_id, 1), HashId("Reverb"));
  SND_CHECK_EQ(common.Cell(bf::chunk::BUSS, bf::col::BUSS_id, 2), HashId("Master"));
  SND_CHECK_EQ(common.Cell(bf::chunk::BUSS, bf::col::BUSS_parent, 2), bf::kNone);
  // Level1 carries the buses its events use (copied rows, same ids).
  SND_CHECK(level.FindRow(bf::chunk::BUSS, bf::col::BUSS_id, HashId("SFX")) != bf::kNone);
  // Events sorted by name; nested clip points at the EVNT row.
  const std::uint32_t ex = level.FindRow(bf::chunk::EVNT, bf::col::EVNT_id, HashId("SE/Explosion"));
  const std::uint32_t nested = level.FindRow(bf::chunk::EVNT, bf::col::EVNT_id, HashId("SE/Nested"));
  SND_REQUIRE(ex != bf::kNone && nested != bf::kNone);
  SND_CHECK_EQ(level.Cell(bf::chunk::EVNT, bf::col::EVNT_priority, ex), 200u);
  SND_CHECK_EQ(level.Cell(bf::chunk::EVNT, bf::col::EVNT_sequenceKind, ex), 1u);
  const std::uint64_t tf = level.Cell(bf::chunk::EVNT, bf::col::EVNT_trackFirst, ex);
  const std::uint64_t cf = level.Cell(bf::chunk::TRAK, bf::col::TRAK_clipFirst, static_cast<std::uint32_t>(tf));
  SND_CHECK_EQ(level.Cell(bf::chunk::CLIP, bf::col::CLIP_kind, static_cast<std::uint32_t>(cf + 1)), 1u);
  SND_CHECK_EQ(level.Cell(bf::chunk::CLIP, bf::col::CLIP_target, static_cast<std::uint32_t>(cf + 1)), nested);
  SND_CHECK_EQ(level.Cell(bf::chunk::CLIP, bf::col::CLIP_kind, static_cast<std::uint32_t>(cf + 2)), 2u);
  // Assets: boom resampled 44100 -> 48000 (LNX_ platform setting), PCMF.
  const std::uint32_t boom = level.FindRow(bf::chunk::ASST, bf::col::ASST_id, HashId("wav/boom"));
  SND_REQUIRE(boom != bf::kNone);
  SND_CHECK_EQ(level.Cell(bf::chunk::ASST, bf::col::ASST_codec, boom), static_cast<std::uint64_t>(SND_CODEC_PCM_F32));
  SND_CHECK_EQ(level.Cell(bf::chunk::ASST, bf::col::ASST_sampleRate, boom), 48000u);
  SND_CHECK_EQ(level.Cell(bf::chunk::ASST, bf::col::ASST_totalFrames, boom), (1000u * 48000u + 44099u) / 44100u);
  SND_CHECK_EQ(level.Cell(bf::chunk::ASST, bf::col::ASST_dataOffset, boom) % 64, 0u);
  // Stream asset: STRM row, SPTH path, preload, seek table, loop.
  const std::uint32_t amb = level.FindRow(bf::chunk::ASST, bf::col::ASST_id, HashId("wav/amb"));
  SND_REQUIRE(amb != bf::kNone);
  SND_CHECK_EQ(level.Cell(bf::chunk::ASST, bf::col::ASST_loadForm, amb), 3u);
  SND_CHECK_EQ(level.Cell(bf::chunk::ASST, bf::col::ASST_loopStart, amb), 100u);
  SND_CHECK_EQ(level.Cell(bf::chunk::ASST, bf::col::ASST_loopEnd, amb), 4000u);
  SND_CHECK_EQ(level.Cell(bf::chunk::ASST, bf::col::ASST_preloadFrames, amb), 2400u);  // 50 ms
  SND_CHECK_EQ(level.Cell(bf::chunk::ASST, bf::col::ASST_preloadBytes, amb), 2400u * 4u);
  SND_CHECK(level.Cell(bf::chunk::ASST, bf::col::ASST_seekCount, amb) > 1u);
  const auto strm = static_cast<std::uint32_t>(level.Cell(bf::chunk::ASST, bf::col::ASST_stream, amb));
  const char* path = reinterpret_cast<const char*>(
      level.chunks[bf::chunk::SPTH].p + level.Cell(bf::chunk::STRM, bf::col::STRM_pathOffset, strm));
  const std::vector<std::uint8_t> sf = sample::ReadFile(out / path);
  SND_CHECK_EQ(sf.size(), level.Cell(bf::chunk::STRM, bf::col::STRM_fileBytes, strm));
  SND_CHECK_EQ(sf.size() % 2048, 0u);
  SND_CHECK_EQ(level.Cell(bf::chunk::STRM, bf::col::STRM_dataOffset, strm), 2048u);
  SND_CHECK(std::memcmp(sf.data() + 2048,
                        level.chunks[bf::chunk::PREL].p + level.Cell(bf::chunk::ASST, bf::col::ASST_preloadOffset, amb),
                        2400 * 4) == 0);
  // Music: two states, three rules in authored order, a stinger.
  SND_CHECK_EQ(level.Cell(bf::chunk::MSEG, bf::col::MSEG_flags, 0), 1u);  // entry preload at the head
  SND_CHECK_EQ(level.Rows(bf::chunk::MSTA), 2u);
  SND_CHECK_EQ(level.Rows(bf::chunk::MTRN), 3u);
  SND_CHECK_EQ(level.Cell(bf::chunk::MTRN, bf::col::MTRN_fromState, 1), 0u);  // "*"
  SND_CHECK_EQ(level.Cell(bf::chunk::MSTG, bf::col::MSTG_id, 0), HashId("Sting/Hit"));
  // Effects, DSP need, snapshot values.
  SND_CHECK_EQ(common.Rows(bf::chunk::EFFX), 2u);
  bf::DspPoolNeed need;
  std::memcpy(&need, common.chunks[bf::chunk::DSPN].p, sizeof(need));
  SND_CHECK_EQ(need.instanceCount, 2u);
  SND_CHECK_EQ(need.instanceBytes, 2u * bt::kProvisionalEffectStateBytes);
  SND_CHECK_EQ(common.Rows(bf::chunk::SNPV), 3u);
  SND_CHECK_EQ(common.Rows(bf::chunk::DUCK), 1u);
  // PHYS: pair (wood, metal) stored at min*N+max.
  SND_REQUIRE(level.Has(bf::chunk::PHYS));
  SND_CHECK((level.chunks[bf::chunk::PHYS].e.flags & bf::kChunkApplication) != 0);
  // Strings / GUIDs in a dev build.
  SND_CHECK(level.Has(bf::chunk::STRS) && level.Has(bf::chunk::NAME) && level.Has(bf::chunk::GUID));
  // ID table and game header.
  const std::vector<std::uint8_t> ids = sample::ReadFile(out / "ids.json");
  const std::string idsText(ids.begin(), ids.end());
  SND_CHECK(idsText.find("\"SE/Explosion\"") != std::string::npos);
  SND_CHECK(idsText.find("\"aliasOf\": \"SE/Explosion\"") != std::string::npos);
  const std::vector<std::uint8_t> hdr = sample::ReadFile(out / "sonus_ids.h");
  const std::string h(hdr.begin(), hdr.end());
  SND_CHECK(h.find("SE_Explosion = " + std::string("0x")) != std::string::npos);
  SND_CHECK(h.find("static_assert(event::SE_Explosion == snd::HashId(\"SE/Explosion\"));") != std::string::npos);
}

SND_TEST(BuildtoolBuild, ReleaseStripsStringsAndGuids) {
  const fs::path proj = sample::MakeProject("bt-release");
  bt::BuildOptions o = Options(proj, "out");
  o.devBuild = false;
  o.stripStrings = true;
  bt::BuildResult r;
  bt::Diagnostics d;
  SND_REQUIRE(BuildOk(o, &r, &d));
  Bank level(sample::ReadFile(fs::path(o.outDir) / "Level1.sndb"));
  SND_REQUIRE(level.ok);
  SND_CHECK(!level.Has(bf::chunk::STRS) && !level.Has(bf::chunk::NAME) && !level.Has(bf::chunk::GUID));
  SND_CHECK_EQ(level.h.flags & (bf::kFileDevBuild | bf::kFileHasStrings), 0u);
  SND_CHECK(level.Has(bf::chunk::SPTH));  // stream paths are never stripped
}

SND_TEST(BuildtoolBuild, DeterministicOutput) {
  const fs::path proj = sample::MakeProject("bt-determinism");
  bt::BuildResult r1, r2;
  bt::Diagnostics d1, d2;
  SND_REQUIRE(BuildOk(Options(proj, "out1"), &r1, &d1));
  SND_REQUIRE(BuildOk(Options(proj, "out2"), &r2, &d2));
  SND_CHECK_EQ(r1.projectBuildHash, r2.projectBuildHash);
  std::uint32_t files = 0;
  for (const auto& entry : fs::recursive_directory_iterator(proj.parent_path() / "out1")) {
    if (!entry.is_regular_file()) continue;
    const fs::path rel = fs::relative(entry.path(), proj.parent_path() / "out1");
    SND_CHECK_MSG(sample::ReadFile(entry.path()) == sample::ReadFile(proj.parent_path() / "out2" / rel), "%s differs",
                  rel.string().c_str());
    ++files;
  }
  SND_CHECK(files >= 8u);
}

SND_TEST(BuildtoolBuild, DifferentialBuild) {
  const fs::path proj = sample::MakeProject("bt-diff");
  bt::BuildOptions o = Options(proj, "out");
  o.cacheDir = (proj.parent_path() / "cache").string();
  bt::BuildResult r;
  bt::Diagnostics d;
  SND_REQUIRE(BuildOk(o, &r, &d));
  SND_CHECK_EQ(r.assetsEncoded, 5u);  // boom, amb, seg, hello(ja), hello(en)
  SND_CHECK_EQ(r.assetsFromCache, 0u);
  // Same inputs: everything from the cache, nothing rewritten.
  bt::BuildResult r2;
  bt::Diagnostics d2;
  SND_REQUIRE(BuildOk(o, &r2, &d2));
  SND_CHECK_EQ(r2.assetsEncoded, 0u);
  SND_CHECK_EQ(r2.assetsFromCache, 5u);
  SND_CHECK_EQ(r2.filesWritten, 0u);
  // One WAV changes: one conversion, the dependent banks change.
  sample::WriteFile(proj / "audio/boom.wav",
                    bt::WriteWav(sample::Tone(1, 44100, 1200, 440), bt::WavSampleFormat::Int16));
  bt::BuildResult r3;
  bt::Diagnostics d3;
  SND_REQUIRE(BuildOk(o, &r3, &d3));
  SND_CHECK_EQ(r3.assetsEncoded, 1u);
  SND_CHECK_EQ(r3.assetsFromCache, 4u);
  bool levelWritten = false, commonWritten = false;
  for (const bt::BuiltBank& b : r3.banks) {
    if (b.name == "Level1") levelWritten = b.written;
    if (b.name == "Common") commonWritten = b.written;
  }
  SND_CHECK(levelWritten);
  SND_CHECK(commonWritten);  // projectBuildHash in every header changed
  // Fresh output dir, warm cache: identical bytes to a cold build.
  bt::BuildOptions cold = Options(proj, "cold");
  bt::BuildResult r4;
  bt::Diagnostics d4;
  SND_REQUIRE(BuildOk(cold, &r4, &d4));
  SND_CHECK(sample::ReadFile(fs::path(o.outDir) / "Level1.sndb") == sample::ReadFile(fs::path(cold.outDir) / "Level1.sndb"));
}

SND_TEST(BuildtoolBuild, PluggableEncoder) {
  const fs::path proj = sample::MakeProject("bt-encoder");
  // ADPCM requested for the voice asset: no encoder in this build -> error naming the codec.
  AddObject(proj, "project.json",
            R"({"type": "project", "guid": "5a0e0000-0000-4000-8000-000000000001", "name": "Sample",
               "languages": ["ja", "en"], "platforms": {"default": {"codec": "ADPC"}}})");
  bt::BuildResult r;
  bt::Diagnostics d;
  bt::BuildOptions o = Options(proj, "out");
  SND_CHECK(!bt::Build(o, d, &r));
  SND_CHECK(d.Contains("no encoder for codec 'ADPC'"));
  SND_CHECK(!fs::exists(o.outDir));  // nothing written on error
  // Registering an encoder (as the codec module will) makes it build.
  bt::EncoderRegistry reg;
  reg.Register(bt::Encoder{SND_CODEC_ADPCM, "test-adpcm", 1,
                           [](const bt::EncodeInput& in, bt::EncodedAsset* out, std::string*) {
                             out->data.assign(static_cast<std::size_t>((in.frames + 63) / 64 * 36 * in.channels), 0);
                             out->framesPerPacket = 64;
                             out->blockAlign = 36 * in.channels;
                             out->totalFrames = in.frames;
                             for (std::uint64_t f = 0; f < in.frames; f += 64 * 16) out->seek.push_back({f, f / 64 * out->blockAlign});
                             return true;
                           }});
  o.encoders = &reg;
  bt::Diagnostics d2;
  SND_REQUIRE(BuildOk(o, &r, &d2));
  Bank level(sample::ReadFile(fs::path(o.outDir) / "Level1.sndb"));
  SND_REQUIRE(level.ok);
  const std::uint32_t amb = level.FindRow(bf::chunk::ASST, bf::col::ASST_id, HashId("wav/amb"));
  SND_CHECK_EQ(level.Cell(bf::chunk::ASST, bf::col::ASST_codec, amb), static_cast<std::uint64_t>(SND_CODEC_ADPCM));
  SND_CHECK_EQ(level.Cell(bf::chunk::ASST, bf::col::ASST_preloadFrames, amb), 2432u);  // 50 ms rounded up to 64
}

// ---------------------------------------------------------------------------
// Build errors: file:line:col and reason, nothing written.
// ---------------------------------------------------------------------------
namespace {
bool ExpectError(const char* testName, const std::string& rel, const std::string& json, const std::string& needle,
                 const std::string& where = "") {
  const fs::path proj = sample::MakeProject(testName);
  AddObject(proj, rel, json);
  bt::BuildOptions o = Options(proj, "out");
  bt::BuildResult r;
  bt::Diagnostics d;
  const bool ok = bt::Build(o, d, &r);
  bool found = false;
  for (const bt::Diagnostic& x : d.items()) {
    const std::string f = bt::Diagnostics::Format(x);
    if (x.severity == bt::Severity::Error && f.find(needle) != std::string::npos &&
        f.find(where) != std::string::npos) {
      found = true;
    }
  }
  if (!found) snd::test::Note("expected '%s' at '%s' in:\n%s", needle.c_str(), where.c_str(), DumpDiag(d).c_str());
  return !ok && found && !fs::exists(o.outDir);
}
}  // namespace

SND_TEST(BuildtoolErrors, IdCollisionIsAnError) {
  // "SE/Col72319" and "SE/Col204226" have the same FNV-1a 32 hash.
  static_assert(HashId("SE/Col72319") == HashId("SE/Col204226"));
  SND_CHECK(ExpectError("bt-collision", "events/Col.json",
                        R"({"type": "event", "guid": "6a0e0000-0000-4000-8000-000000000001", "name": "SE/Col72319",
  "bank": "Level1", "aliases": ["SE/Col204226"]})",
                        "id collision: 'SE/Col204226' and 'SE/Col72319'"));
  // Across kinds and files too.
  const fs::path proj = sample::MakeProject("bt-collision2");
  AddObject(proj, "events/C1.json",
            R"({"type": "event", "guid": "6a0e0000-0000-4000-8000-000000000002", "name": "SE/Col72319", "bank": "Level1"})");
  AddObject(proj, "buses/C2.json",
            R"({"type": "bus", "guid": "6a0e0000-0000-4000-8000-000000000003", "name": "SE/Col204226", "parent": "Master"})");
  bt::Diagnostics d;
  bt::BuildResult r;
  SND_CHECK(!bt::Build(Options(proj, "out"), d, &r));
  SND_CHECK(d.Contains("id collision"));
}

SND_TEST(BuildtoolErrors, NestedCycle) {
  SND_CHECK(ExpectError("bt-cycle", "events/Nested.json",
                        R"({"type": "event", "guid": "5a0e0000-0000-4000-8000-000000000014", "name": "SE/Nested",
  "bank": "Level1", "bus": "SFX",
  "tracks": [{"clips": [{"event": "SE/Explosion"}]}]})",
                        "nested event cycle: 'SE/Explosion' -> 'SE/Nested' -> 'SE/Explosion'", "events/"));
}

SND_TEST(BuildtoolErrors, BusCycleAndReferences) {
  SND_CHECK(ExpectError("bt-buscycle", "buses/Master.json",
                        R"({"type": "bus", "guid": "5a0e0000-0000-4000-8000-000000000005", "name": "Master",
  "bank": "Common", "master": true, "sends": [{"bus": "SFX"}]})",
                        "bus graph cycle"));
  SND_CHECK(ExpectError("bt-unknownref", "events/Bad.json",
                        "{\n  \"type\": \"event\",\n  \"guid\": \"6a0e0000-0000-4000-8000-000000000010\",\n"
                        "  \"name\": \"SE/Bad\",\n  \"bank\": \"Level1\",\n  \"bus\": \"NoSuchBus\"\n}",
                        "unknown bus 'NoSuchBus'", "events/Bad.json:6:10"));
  SND_CHECK(ExpectError("bt-syntax", "events/Syntax.json", "{\n  \"type\": \"event\",\n  \"name\" \"x\"\n}",
                        "expected ':'", "events/Syntax.json:3:10"));
  SND_CHECK(ExpectError("bt-dupguid", "events/Dup.json",
                        R"({"type": "event", "guid": "5a0e0000-0000-4000-8000-000000000017", "name": "SE/Other", "bank": "Level1"})",
                        "duplicate guid"));
  // Cross-bank use without a dependency.
  const fs::path proj = sample::MakeProject("bt-access2");
  AddObject(proj, "banks/Orphan.json",
            R"({"type": "bank", "guid": "6a0e0000-0000-4000-8000-000000000012", "name": "Orphan"})");
  AddObject(proj, "events/Orphan.json",
            R"({"type": "event", "guid": "6a0e0000-0000-4000-8000-000000000013", "name": "SE/Orphan", "bank": "Orphan", "bus": "SFX"})");
  bt::Diagnostics d;
  bt::BuildResult r;
  SND_CHECK(!bt::Build(Options(proj, "out"), d, &r));
  SND_CHECK(d.Contains("belongs to bank 'Common' but bank 'Orphan' does not depend on it"));
  // Language asset used from a normal bank.
  SND_CHECK(ExpectError("bt-langasset", "events/HelloLevel.json",
                        R"({"type": "event", "guid": "6a0e0000-0000-4000-8000-000000000014", "name": "SE/Hello2",
  "bank": "Level1", "tracks": [{"clips": [{"asset": "vo/hello"}]}]})",
                        "per-language files but is used by a non-language bank"));
}
