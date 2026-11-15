// Round trip: buildtool builds the sample project (buildtool/test/data/sample),
// the bank module validates and reads every bank, and BankManager loads them
// through the memory file system (dependencies, language switch).
// buildtool must not depend on bank, so this check lives on the bank side.
#include <cstring>
#include <string>
#include <vector>

#include "sample_project.hpp"
#include "snd/bank/bank_manager.hpp"
#include "snd/bank/validate.hpp"
#include "snd/buildtool/build.hpp"
#include "snd/id.hpp"
#include "snd/mocks/deterministic_job_system.hpp"
#include "snd/mocks/memory_file_system.hpp"
#include "snd/test/test.hpp"
#include "test_support.hpp"

namespace bf = snd::bank_format;
namespace ch = bf::chunk;
namespace fs = std::filesystem;
using snd::HashId;
using namespace snd::bank;

namespace {

struct Built {
  fs::path out;
  bool ok = false;
};

Built BuildSample(const char* name, bool dev, bool strip = false) {
  Built b;
  const fs::path proj = sample::MakeProject(name);
  snd::buildtool::BuildOptions o;
  o.projectDir = proj.string();
  o.outDir = (proj.parent_path() / "out").string();
  o.devBuild = dev;
  o.stripStrings = strip;
  snd::buildtool::Diagnostics d;
  snd::buildtool::BuildResult r;
  b.ok = snd::buildtool::Build(o, d, &r);
  for (const snd::buildtool::Diagnostic& x : d.items()) {
    if (x.severity == snd::buildtool::Severity::Error) snd::test::Note("%s", snd::buildtool::Diagnostics::Format(x).c_str());
  }
  b.out = o.outDir;
  return b;
}

struct Loaded {
  std::vector<std::uint8_t> bytes;
  std::uint8_t* mem = nullptr;
  BankView view;
  snd_result result = SND_ERR_IO;
  ParseError err;
  explicit Loaded(const fs::path& p) : bytes(sample::ReadFile(p)) {
    void* m = nullptr;
    if (posix_memalign(&m, 64, bytes.empty() ? 1 : bytes.size()) != 0) return;
    mem = static_cast<std::uint8_t*>(m);
    std::memcpy(mem, bytes.data(), bytes.size());
    result = ParseBank(mem, bytes.size(), ParseOptions{}, &view, &err);
    if (result != SND_OK) snd::test::Note("%s: %s", p.string().c_str(), err.message);
  }
  ~Loaded() { std::free(mem); }
};

}  // namespace

SND_TEST(BankRoundTrip, BuildtoolOutputValidatesAndReads) {
  const Built b = BuildSample("roundtrip", true);
  SND_REQUIRE(b.ok);
  Loaded common(b.out / "Common.sndb");
  Loaded level(b.out / "Level1.sndb");
  Loaded ja(b.out / "Voice.ja.sndb");
  Loaded en(b.out / "Voice.en.sndb");
  SND_REQUIRE_EQ(common.result, SND_OK);
  SND_REQUIRE_EQ(level.result, SND_OK);
  SND_REQUIRE_EQ(ja.result, SND_OK);
  SND_REQUIRE_EQ(en.result, SND_OK);
  const BankView& v = level.view;
  // Names, ids and the graph-ordered bus table.
  const std::uint32_t ex = v.FindRow<col::EVNT_id>(HashId("SE/Explosion"));
  SND_REQUIRE(ex != bf::kNone);
  SND_CHECK(std::strcmp(v.Name(HashId("SE/Explosion")), "SE/Explosion") == 0);
  SND_CHECK_EQ(v.Column<col::EVNT_priority>()[ex], 200u);
  SND_CHECK((v.Column<col::EVNT_flags>()[ex] & bf::kEvent3D) != 0);
  const std::uint32_t bus = v.Column<col::EVNT_bus>()[ex];
  SND_CHECK_EQ(v.Column<col::BUSS_id>()[bus], HashId("SFX"));
  SND_CHECK_EQ(v.Column<col::BUSS_id>()[v.Column<col::BUSS_parent>()[bus]], HashId("Master"));
  // Curves.
  const std::uint32_t cv = v.Column<col::EVNT_curveVolume>()[ex];
  SND_REQUIRE(cv != bf::kNone);
  SND_CHECK_EQ(v.Column<col::CURV_input>()[cv], bf::kInputDistance);
  SND_CHECK_EQ(v.Column<col::CPNT_y>()[v.Column<col::CURV_pointFirst>()[cv] + 1], -40.0f);
  // Stream asset: path, preload equals the stream file's first data bytes.
  const std::uint32_t amb = v.FindRow<col::ASST_id>(HashId("wav/amb"));
  SND_REQUIRE(amb != bf::kNone);
  const std::uint32_t strm = v.Column<col::ASST_stream>()[amb];
  const char* path = v.StreamPath(strm);
  SND_REQUIRE(path != nullptr);
  const std::vector<std::uint8_t> sf = sample::ReadFile(b.out / path);
  SND_REQUIRE_EQ(sf.size(), v.Column<col::STRM_fileBytes>()[strm]);
  const ByteSpan pre = v.Preload(amb);
  SND_REQUIRE(pre.bytes > 0);
  SND_CHECK(std::memcmp(sf.data() + v.Column<col::STRM_dataOffset>()[strm], pre.data, pre.bytes) == 0);
  const snd::RangeU32 seek = v.SeekRange(amb);
  SND_CHECK(seek.count > 1);
  SND_CHECK_EQ(v.Column<col::SEEK_frame>()[seek.first], 0u);
  // Resident PCM data.
  const std::uint32_t boom = v.FindRow<col::ASST_id>(HashId("wav/boom"));
  SND_CHECK_EQ(v.WaveData(boom).bytes, v.Column<col::ASST_totalFrames>()[boom] * 4u);
  // Music: initial state, playlist -> segment -> asset.
  const std::uint32_t music = v.FindRow<col::EVNT_id>(HashId("Music/Main"));
  const std::uint32_t st = v.Column<col::EVNT_musicState>()[music];
  SND_CHECK_EQ(v.Column<col::MSTA_id>()[st], HashId("Explore"));
  const std::uint32_t pl = v.Column<col::MSTA_playlist>()[st];
  const std::uint32_t seg = v.Column<col::MPLE_segment>()[v.Column<col::MPLS_entryFirst>()[pl]];
  SND_CHECK_EQ(v.Column<col::MSEG_id>()[seg], HashId("Seg/A"));
  SND_CHECK_EQ(v.Column<col::MMRK_id>()[v.Column<col::MSEG_markerFirst>()[seg] + 1], HashId("Exit"));
  // Physics table: the (wood, metal) override at min*N+max, through the application chunk.
  PhysTableView pt;
  SND_REQUIRE(v.PhysTable(0, &pt));
  SND_CHECK_EQ(pt.materialCount, 2u);
  SND_CHECK_EQ(pt.pair[0 * 2 + 1], HashId("SE/Sting"));
  SND_CHECK_EQ(pt.single[1 * 4 + 1], HashId("SE/Nested"));  // wood slide
  SND_CHECK(!v.ApplicationChunk(ch::PHYS).empty());
  // GUIDs (dev build).
  std::uint64_t lo = 0, hi = 0;
  SND_CHECK(v.Guid(HashId("SE/Explosion"), &lo, &hi));
  SND_CHECK_EQ(hi, 0x5a0e000000004000ull);
  // DSP need in the bus bank.
  SND_CHECK_EQ(common.view.DspNeed().instanceCount, 1u);  // stage 1.5: only snd.fx.lowpass is sized (A1-11)
  // Language banks.
  SND_CHECK_EQ(ja.view.languageId(), HashId("ja"));
  SND_CHECK_EQ(en.view.bankId(), HashId("Voice"));
  const std::uint32_t hello = ja.view.FindRow<col::ASST_id>(HashId("vo/hello"));
  SND_CHECK_EQ(ja.view.Column<col::ASST_totalFrames>()[hello], 900u);
  SND_CHECK_EQ(en.view.Column<col::ASST_totalFrames>()[en.view.FindRow<col::ASST_id>(HashId("vo/hello"))], 700u);
}

SND_TEST(BankRoundTrip, ManagerLoadsBuiltBanks) {
  const Built b = BuildSample("roundtrip-manager", true);
  SND_REQUIRE(b.ok);
  testsupport::CountingAllocator alloc;
  snd::mock::MemoryFileSystem memFs;
  snd::mock::DeterministicJobSystem jobs;
  for (const auto& e : fs::directory_iterator(b.out)) {
    if (e.path().extension() == ".sndb") memFs.AddFile(e.path().filename().string(), sample::ReadFile(e.path()));
  }
  BankManagerDesc d;
  d.allocator = &alloc;
  d.fileSystem = &memFs;
  d.ioMode = snd::bank::BankIoMode::DirectFileSystem;  // stage 1.5: the default is the IO scheduler
  d.jobs = &jobs;
  d.prepare.devOverlay = true;
  BankManager* m = nullptr;
  SND_REQUIRE_EQ(BankManager::Create(d, &m), SND_OK);
  auto settle = [&] {
    snd::cmd::BankRegister reg{};
    for (int i = 0; i < 4; ++i) {
      m->Update();
      while (m->PopRegistration(&reg)) m->MarkRegistered(reg.bank);
    }
  };
  // Level1 first: it waits for Common (requested in the same frame).
  const snd::HandleRef level = m->RequestLoad("Level1.sndb");
  const snd::HandleRef common = m->RequestLoad("Common.sndb");
  const snd::HandleRef ja = m->RequestLoad("Voice.ja.sndb");
  settle();
  SND_CHECK_EQ(m->State(common), SND_BANK_STATE_LOADED);
  SND_CHECK_EQ(m->State(level), SND_BANK_STATE_LOADED);
  SND_CHECK_EQ(m->State(ja), SND_BANK_STATE_LOADED);
  SND_CHECK_EQ(m->DependentCount(common), 2u);
  SND_CHECK(m->Prepared(level)->view.writable());  // dev build -> override layer
  // Language switch to "en" ("en" declared by the game; "ja" was learned from the path).
  SND_CHECK_EQ(m->DeclareLanguage("en"), SND_OK);
  LanguageSwitch plan[2];
  snd_result r = SND_OK;
  SND_CHECK_EQ(m->SetLanguage(HashId("en"), plan, 2, &r), 1u);
  settle();
  const snd::HandleRef en = m->FindBank(HashId("Voice"), HashId("en"));
  SND_CHECK_EQ(m->State(en), SND_BANK_STATE_LOADED);
  SND_CHECK_EQ(m->BeginUnload(plan[0].oldBank, SND_UNLOAD_IF_UNUSED, false), SND_OK);
  SND_CHECK_EQ(m->Release(plan[0].oldBank), SND_OK);
  // Common cannot go while Level1 and Voice.en depend on it.
  SND_CHECK_EQ(m->CheckUnload(common, SND_UNLOAD_STOP_AND_UNLOAD, false), SND_ERR_BANK_IN_USE);
  BankManager::Destroy(m);
  SND_CHECK_EQ(alloc.Live(), 0u);
}

// Bank format 2.0: rename aliases survive a release build without strings (ALIS),
// nested events are after their owner.
SND_TEST(BankRoundTrip, AliasesResolveInStrippedReleaseBanks) {
  const Built b = BuildSample("roundtrip-alias", false, true);
  SND_REQUIRE(b.ok);
  Loaded level(b.out / "Level1.sndb");
  SND_REQUIRE_EQ(level.result, SND_OK);
  SND_CHECK(!level.view.HasChunk(ch::NAME));
  SND_CHECK_EQ(level.view.ResolveAlias(HashId("SE/Boom")), HashId("SE/Explosion"));
  SND_CHECK_EQ(level.view.ResolveAlias(HashId("SE/Explosion")), 0u);
  Loaded common(b.out / "Common.sndb");
  SND_REQUIRE_EQ(common.result, SND_OK);
  SND_CHECK_EQ(common.view.ResolveAlias(HashId("SE/Boom")), 0u);  // not in this bank
  const std::uint32_t ex = level.view.FindRow<col::EVNT_id>(HashId("SE/Explosion"));
  const std::uint32_t nested = level.view.FindRow<col::EVNT_id>(HashId("SE/Nested"));
  SND_CHECK(ex < nested);
}
