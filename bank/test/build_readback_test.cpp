// buildtool -> bank read-back (independent tester): an "everything" project
// that sets every member documented in spec/interface/project-format.md to a
// non-default value; the built bank must pass ParseBank and every value must
// be readable back from the columns (a value that does not come back is a
// setting silently dropped by the build). Plus: release / stripped banks,
// stream files (sector alignment, seek table, preload), material symmetry,
// and builds that the runtime would reject.
//
// Tests named Defect_* fail on the current implementation on purpose.
#include <cstring>
#include <string>
#include <vector>

#include "mini_project.hpp"
#include "snd/bank/validate.hpp"
#include "snd/buildtool/build.hpp"
#include "snd/id.hpp"
#include "snd/test/test.hpp"
#include "spec_walk.hpp"

namespace bf = snd::bank_format;
namespace ch = bf::chunk;
namespace fs = std::filesystem;
namespace bt = snd::buildtool;
using mini::Project;
using snd::HashId;
using namespace snd::bank;

#define REQUIRE_MSG(cond, ...)       \
  do {                               \
    const bool ok_ = (cond);         \
    SND_CHECK_MSG(ok_, __VA_ARGS__); \
    if (!ok_) return;                \
  } while (0)

namespace {

struct Loaded {
  std::vector<std::uint8_t> bytes;
  std::uint8_t* mem = nullptr;
  BankView v;
  snd_result result = SND_ERR_IO;
  ParseError err;
  explicit Loaded(const fs::path& p) : bytes(mini::ReadBytes(p)) {
    void* m = nullptr;
    if (bytes.empty() || posix_memalign(&m, 64, bytes.size()) != 0) return;
    mem = static_cast<std::uint8_t*>(m);
    std::memcpy(mem, bytes.data(), bytes.size());
    result = ParseBank(mem, bytes.size(), ParseOptions{}, &v, &err);
  }
  ~Loaded() { std::free(mem); }
  Loaded(const Loaded&) = delete;
  Loaded& operator=(const Loaded&) = delete;
};

template <class C>
typename C::Type Cell(const BankView& v, std::uint32_t row) {
  return v.Column<C>().Get(row);
}

// The "everything" project. Returns false if building failed.
void WriteEverything(Project& p) {
  p.Add("project.json", "project", "P",
        "\"formatVersion\": 1, \"languages\": [\"ja\", \"en\"], "
        "\"platforms\": {\"default\": {\"codec\": \"PC16\", \"sectorAlign\": 4096}}");
  p.Add("banks/Main.json", "bank", "Main");
  p.Add("banks/Voice.json", "bank", "Voice", "\"language\": true, \"dependsOn\": [\"Main\"]");
  p.Add("buses/Master.json", "bus", "Master",
        "\"bank\": \"Main\", \"master\": true, \"meter\": true, \"volumeDb\": -1.5, \"channels\": 2, \"lodPriority\": 3");
  p.Add("buses/Rev.json", "bus", "Rev",
        "\"parent\": \"Master\", \"output\": true, \"lodBypassable\": true, \"volumeDb\": -2.5, \"channels\": 1, "
        "\"effects\": [{\"type\": \"Conv\", \"params\": [0.25, 0.75], \"blob\": \"audio/ir.bin\", \"flags\": 5}]");
  p.Add("buses/Sfx.json", "bus", "Sfx",
        "\"parent\": \"Master\", \"sends\": [{\"bus\": \"Rev\", \"levelDb\": -7.5, \"preFader\": true}], "
        "\"effects\": [{\"type\": \"Lowpass\", \"params\": [1234]}]");
  mini::WriteBytes(p.dir() / "audio/ir.bin", std::vector<std::uint8_t>(77, 0x5C));
  p.Add("categories/CatP.json", "category", "CatP");
  p.Add("categories/Cat.json", "category", "Cat",
        "\"parent\": \"CatP\", \"ignorePause\": true, \"muteOnFocusLoss\": true, \"volumeDb\": -4.5");
  p.Add("parameters/Speed.json", "parameter", "Speed",
        "\"kind\": \"switch\", \"scope\": [\"emitter\"], \"min\": -2, \"max\": 9, \"default\": 3, \"rampRate\": 1.5");
  p.Add("snapshots/Snap.json", "snapshot", "Snap",
        "\"bank\": \"Main\", \"values\": [{\"kind\": \"busVolume\", \"bus\": \"Sfx\", \"value\": -9}, "
        "{\"kind\": \"effectParam\", \"bus\": \"Rev\", \"effect\": 0, \"param\": 1, \"value\": 0.5}, "
        "{\"kind\": \"sendLevel\", \"bus\": \"Sfx\", \"send\": 0, \"value\": -3}]");
  p.Add("groups/G.json", "voiceLimitGroup", "G", "\"maxVoices\": 6, \"steal\": \"quietest\"");
  p.Add("mix/Duck.json", "ducking", "",
        "\"bank\": \"Main\", \"kind\": \"rule\", \"source\": \"Cat\", \"target\": \"Sfx\", \"amountDb\": -8, "
        "\"attackMs\": 12, \"releaseMs\": 345, \"thresholdDb\": -20");
  p.Wav("audio/r.wav", mini::Tone(1, 48000, 4800));
  p.Wav("audio/s.wav", mini::Tone(2, 48000, 48000));
  p.Wav("audio/m.wav", mini::Tone(2, 48000, 96000));
  p.Wav("audio/ja.wav", mini::Tone(1, 48000, 900));
  p.Wav("audio/en.wav", mini::Tone(1, 48000, 700));
  p.Add("assets/r.json", "asset", "r",
        "\"file\": \"audio/r.wav\", \"loop\": {\"start\": 100, \"end\": 4000}, \"stallPolicy\": \"sound\", "
        "\"virtualReturn\": \"restart\", \"ioPriority\": \"music\", \"convert\": {\"default\": {\"codec\": \"PCMF\"}}");
  p.Add("assets/s.json", "asset", "s",
        "\"file\": \"audio/s.wav\", \"loadForm\": \"stream\", \"preloadMs\": 50, \"seekIntervalMs\": 250");
  p.Add("assets/s0.json", "asset", "s0", "\"file\": \"audio/s.wav\", \"loadForm\": \"stream\", \"preloadMs\": 0");
  p.Add("assets/c.json", "asset", "c",
        "\"file\": \"audio/r.wav\", \"loadForm\": \"compressed\", \"ioPriority\": \"voice\", \"virtualReturn\": \"never\", "
        "\"convert\": {\"default\": {\"sampleRate\": 24000, \"channels\": 2}}");
  p.Add("assets/v.json", "asset", "v", "\"languages\": {\"ja\": \"audio/ja.wav\", \"en\": \"audio/en.wav\"}");
  p.Add("assets/m.json", "asset", "m", "\"file\": \"audio/m.wav\", \"loadForm\": \"stream\"");
  p.Add("events/E.json", "event", "E",
        "\"bank\": \"Main\", \"bus\": \"Sfx\", \"category\": \"Cat\", \"limitGroup\": \"G\", \"priority\": 201, "
        "\"sequence\": \"switch\", \"is3D\": true, \"ignorePause\": true, \"maxInstances\": 4, \"retriggerMs\": 33, "
        "\"fadeOutMs\": 44, \"comboWindowMs\": 55, \"maxDistance\": 66, "
        "\"cone\": {\"innerDeg\": 30, \"outerDeg\": 90, \"outerDb\": -6}, \"virtualReturn\": \"never\", "
        "\"distanceCurves\": {\"volume\": {\"points\": [[0, 0], [10, -1]]}, \"lowpass\": {\"points\": [[0, 0], [10, -2]]}, "
        "\"spread\": {\"points\": [[0, 0], [10, -3]]}, \"reverbSend\": {\"points\": [[0, 0], [10, -4]]}, "
        "\"blend3D\": {\"points\": [[0, 0], [10, -5]]}}, "
        "\"curves\": [{\"parameter\": \"Speed\", \"target\": \"pan\", \"compose\": \"max\", \"points\": [[0, 0], [1, 1, 2]]}], "
        "\"tracks\": [{\"bus\": \"Rev\", \"layerParameter\": \"Speed\", \"flags\": 3, \"clips\": ["
        "{\"asset\": \"r\", \"startMs\": 11, \"weight\": 2.5, \"volumeDb\": -1, \"pitchCents\": 12, "
        "\"random\": {\"volumeDb\": 1, \"pitchCents\": 2, \"pan\": 0.3, \"startMs\": 4, \"filterHz\": 500}, "
        "\"switchValue\": \"Grass\", \"layer\": [0.1, 0.9], \"fadeInMs\": 7, \"fadeOutMs\": 8, \"loopCount\": 0}, "
        "{\"event\": \"N\", \"switchValue\": 7}, "
        "{\"synth\": {\"kind\": \"oscillator\", \"waveform\": \"pulse\", \"frequencyHz\": 330, \"duty\": 0.25, "
        "\"noiseColor\": \"pink\", \"channels\": 2}}, "
        "{\"synth\": {\"kind\": \"wavetable\", \"asset\": \"r\"}}], "
        "\"actions\": [{\"timeMs\": 100, \"action\": \"applySnapshot\", \"target\": \"Snap\", \"value\": 1}, "
        "{\"timeMs\": 5, \"action\": \"play\", \"target\": \"N\"}]}]");
  p.Add("events/N.json", "event", "N", "\"bank\": \"Main\", \"stinger\": true, \"tracks\": [{\"clips\": [{\"asset\": \"c\"}]}]");
  p.Add("events/S.json", "event", "S", "\"bank\": \"Main\", \"tracks\": [{\"clips\": [{\"asset\": \"s\"}]}]");
  p.Add("events/P.json", "event", "P", "\"bank\": \"Main\", \"tracks\": [{\"clips\": [{\"asset\": \"s0\"}]}]");
  p.Add("events/Music.json", "event", "Music",
        "\"bank\": \"Main\", \"music\": {\"initialState\": \"Battle\", "
        "\"states\": [{\"name\": \"Explore\", \"playlist\": \"PL\"}, {\"name\": \"Battle\", \"playlist\": \"PL\"}], "
        "\"transitions\": [{\"from\": \"Explore\", \"to\": \"Battle\", \"timing\": \"barsAfter\", \"timingCount\": 2, "
        "\"fadeOutBeats\": 0.5, \"fadeInBeats\": 1.5, \"bridge\": \"Seg2\", \"start\": \"marker\", \"startMarker\": \"M1\", "
        "\"interrupt\": \"afterBridge\", \"late\": \"cancel\", \"prepare\": true}, "
        "{\"from\": \"*\", \"to\": \"*\", \"timing\": \"nextBar\"}], "
        "\"stingers\": [{\"name\": \"Sting1\", \"event\": \"N\", \"timing\": \"grid\", \"overlap\": \"stack\"}]}");
  p.Add("music/Seg.json", "segment", "Seg",
        "\"asset\": \"m\", \"bars\": 4, \"tempo\": [{\"tick\": 0, \"bpm\": 100, \"beatsPerBar\": 3, \"beatUnit\": 8}, "
        "{\"tick\": 3840, \"bpm\": 140}], \"entryFrame\": 0, \"exitFrame\": 90000, "
        "\"markers\": [{\"name\": \"M1\", \"frame\": 2000, \"kind\": \"pickup\"}], "
        "\"stems\": [{\"channelFirst\": 1, \"channelCount\": 1, "
        "\"volumeCurve\": {\"parameter\": \"Speed\", \"target\": \"volume\", \"points\": [[0, -60], [1, 0]]}, \"asset\": \"m\"}], "
        "\"entryPreload\": true");
  p.Add("music/Seg2.json", "segment", "Seg2", "\"asset\": \"m\", \"bars\": 1");
  p.Add("music/PL.json", "playlist", "PL",
        "\"mode\": \"shuffle\", \"loopCount\": 3, \"entries\": [{\"segment\": \"Seg\", \"weight\": 2, \"repeat\": 3}, "
        "{\"segment\": \"Seg2\"}]");
  p.Add("physics/Phys.json", "materialTable", "Phys",
        "\"bank\": \"Main\", \"materials\": [\"metal\", \"wood\", \"glass\"], "
        "\"single\": {\"metal\": {\"impact\": \"E\", \"slide\": \"N\", \"roll\": \"S\", \"break\": \"P\"}}, "
        "\"pairs\": [{\"a\": \"glass\", \"b\": \"metal\", \"event\": \"N\"}], \"footsteps\": {\"wood\": \"E\"}, "
        "\"defaults\": {\"impact\": \"S\"}, "
        "\"curves\": {\"wood\": {\"impulseToVelocity\": [1, 2, 3, 4], \"massToPitchCents\": [5, 6], \"speedToVolumeDb\": [7, 8]}}");
  p.Add("events/VO.json", "event", "VO", "\"bank\": \"Voice\", \"tracks\": [{\"clips\": [{\"asset\": \"v\"}]}]");
}

}  // namespace

SND_TEST(BuildReadback, EveryDocumentedFieldComesBack) {
  Project p("everything");
  WriteEverything(p);
  bt::Diagnostics d;
  const bool ok = p.Build(&d);
  REQUIRE_MSG(ok, "%s", mini::Dump(d).c_str());
  if (d.warnings() > 0) snd::test::Note("warnings:\n%s", mini::Dump(d).c_str());
  // Every member used here is documented: no "unknown member" warning may appear.
  SND_CHECK_MSG(!d.Contains("unknown member"), "documented fields warned as unknown:\n%s", mini::Dump(d).c_str());
  Loaded L(p.out() / "Main.sndb");
  REQUIRE_MSG(L.result == SND_OK, "Main.sndb rejected: %s", L.err.message);
  const BankView& v = L.v;
  SND_CHECK(specwalk::CheckInvariants(v).empty());
  (void)specwalk::DeepWalk(v);
  std::vector<std::string> missing;
  auto expect = [&](bool cond, const char* what) {
    if (!cond) missing.push_back(what);
  };
  // Buses.
  const std::uint32_t master = v.FindRow<col::BUSS_id>(HashId("Master"));
  const std::uint32_t rev = v.FindRow<col::BUSS_id>(HashId("Rev"));
  const std::uint32_t sfx = v.FindRow<col::BUSS_id>(HashId("Sfx"));
  SND_REQUIRE(master != bf::kNone && rev != bf::kNone && sfx != bf::kNone);
  expect(Cell<col::BUSS_flags>(v, master) == (bf::kBusMaster | bf::kBusMeter), "bus.master/meter");
  expect(Cell<col::BUSS_volumeDb>(v, master) == -1.5f, "bus.volumeDb");
  expect(Cell<col::BUSS_channels>(v, master) == 2, "bus.channels");
  expect(Cell<col::BUSS_lodPriority>(v, master) == 3, "bus.lodPriority");
  expect(Cell<col::BUSS_flags>(v, rev) == (bf::kBusOutput | bf::kBusLodBypassable), "bus.output/lodBypassable");
  expect(Cell<col::BUSS_parent>(v, rev) == master, "bus.parent");
  expect(Cell<col::BUSS_sendCount>(v, sfx) == 1, "bus.sends");
  if (Cell<col::BUSS_sendCount>(v, sfx) == 1) {
    const std::uint32_t s = Cell<col::BUSS_sendFirst>(v, sfx);
    expect(Cell<col::SEND_targetBus>(v, s) == rev, "sends[].bus");
    expect(Cell<col::SEND_levelDb>(v, s) == -7.5f, "sends[].levelDb");
    expect(Cell<col::SEND_preFader>(v, s) == 1, "sends[].preFader");
  }
  expect(Cell<col::BUSS_effectCount>(v, rev) == 1, "bus.effects");
  if (Cell<col::BUSS_effectCount>(v, rev) == 1) {
    const std::uint32_t fx = Cell<col::BUSS_effectFirst>(v, rev);
    expect(Cell<col::EFFX_effectType>(v, fx) == HashId("Conv"), "effects[].type");
    expect(Cell<col::EFFX_paramCount>(v, fx) == 2 &&
               Cell<col::EFPV_value>(v, Cell<col::EFFX_paramFirst>(v, fx) + 1) == 0.75f,
           "effects[].params");
    expect(Cell<col::EFFX_blobBytes>(v, fx) == 77 &&
               v.Chunk(ch::EFBL).data[Cell<col::EFFX_blobOffset>(v, fx)] == 0x5C,
           "effects[].blob");
    expect(Cell<col::EFFX_flags>(v, fx) == 5, "effects[].flags");
    // Stage 1.5 (A1-11): unknown (non-standard, no plugin) types are not sized.
    expect(Cell<col::EFFX_stateBytes>(v, fx) == 0, "EFFX.stateBytes");
  }
  expect(v.DspNeed().instanceCount == 0, "DSPN.instanceCount");
  // Categories.
  const std::uint32_t cat = v.FindRow<col::CATG_id>(HashId("Cat"));
  const std::uint32_t catp = v.FindRow<col::CATG_id>(HashId("CatP"));
  expect(cat != bf::kNone && Cell<col::CATG_parent>(v, cat) == catp, "category.parent");
  expect(Cell<col::CATG_flags>(v, cat) == (bf::kCategoryIgnorePause | bf::kCategoryMuteOnFocusLoss),
         "category.ignorePause/muteOnFocusLoss");
  expect(Cell<col::CATG_volumeDb>(v, cat) == -4.5f, "category.volumeDb");
  // Parameter.
  const std::uint32_t sp = v.FindRow<col::PARM_id>(HashId("Speed"));
  expect(sp != bf::kNone, "parameter present");
  expect(Cell<col::PARM_kind>(v, sp) == 1, "parameter.kind");
  expect(Cell<col::PARM_scopeMask>(v, sp) == 2, "parameter.scope");
  expect(Cell<col::PARM_minValue>(v, sp) == -2 && Cell<col::PARM_maxValue>(v, sp) == 9, "parameter.min/max");
  expect(Cell<col::PARM_defaultValue>(v, sp) == 3, "parameter.default");
  expect(Cell<col::PARM_rampRate>(v, sp) == 1.5f, "parameter.rampRate");
  // Snapshot.
  const std::uint32_t snap = v.FindRow<col::SNAP_id>(HashId("Snap"));
  expect(snap != bf::kNone && Cell<col::SNAP_valueCount>(v, snap) == 3, "snapshot.values");
  if (snap != bf::kNone && Cell<col::SNAP_valueCount>(v, snap) == 3) {
    const std::uint32_t f = Cell<col::SNAP_valueFirst>(v, snap);
    expect(Cell<col::SNPV_targetKind>(v, f) == 0 && Cell<col::SNPV_targetRow>(v, f) == sfx &&
               Cell<col::SNPV_value>(v, f) == -9,
           "values[busVolume]");
    expect(Cell<col::SNPV_targetKind>(v, f + 1) == 1 &&
               Cell<col::SNPV_targetRow>(v, f + 1) == Cell<col::BUSS_effectFirst>(v, rev) &&
               Cell<col::SNPV_paramIndex>(v, f + 1) == 1 && Cell<col::SNPV_value>(v, f + 1) == 0.5f,
           "values[effectParam]");
    expect(Cell<col::SNPV_targetKind>(v, f + 2) == 2 &&
               Cell<col::SNPV_targetRow>(v, f + 2) == Cell<col::BUSS_sendFirst>(v, sfx) &&
               Cell<col::SNPV_value>(v, f + 2) == -3,
           "values[sendLevel]");
  }
  // Voice limit group, ducking.
  const std::uint32_t g = v.FindRow<col::VLGR_id>(HashId("G"));
  expect(g != bf::kNone && Cell<col::VLGR_maxVoices>(v, g) == 6 && Cell<col::VLGR_stealMode>(v, g) == 1,
         "voiceLimitGroup");
  expect(v.Rows(ch::DUCK) == 1 && Cell<col::DUCK_kind>(v, 0) == 1 && Cell<col::DUCK_source>(v, 0) == cat &&
             Cell<col::DUCK_targetBus>(v, 0) == sfx && Cell<col::DUCK_amountDb>(v, 0) == -8 &&
             Cell<col::DUCK_attackMs>(v, 0) == 12 && Cell<col::DUCK_releaseMs>(v, 0) == 345 &&
             Cell<col::DUCK_thresholdDb>(v, 0) == -20,
         "ducking");
  // Assets.
  const std::uint32_t r = v.FindRow<col::ASST_id>(HashId("r"));
  const std::uint32_t s = v.FindRow<col::ASST_id>(HashId("s"));
  const std::uint32_t c = v.FindRow<col::ASST_id>(HashId("c"));
  SND_REQUIRE(r != bf::kNone && s != bf::kNone && c != bf::kNone);
  expect(Cell<col::ASST_codec>(v, r) == SND_CODEC_PCM_F32, "asset.convert.codec");
  expect(Cell<col::ASST_loopStart>(v, r) == 100 && Cell<col::ASST_loopEnd>(v, r) == 4000, "asset.loop");
  expect((Cell<col::ASST_flags>(v, r) & bf::kAssetStallSoundPriority) != 0, "asset.stallPolicy");
  expect((Cell<col::ASST_flags>(v, r) & bf::kAssetVirtualRestart) != 0, "asset.virtualReturn restart");
  expect((Cell<col::ASST_flags>(v, r) & bf::kAssetPriorityMusic) != 0, "asset.ioPriority music");
  expect((Cell<col::ASST_flags>(v, c) & bf::kAssetVirtualKill) != 0, "asset.virtualReturn never");
  expect((Cell<col::ASST_flags>(v, c) & bf::kAssetPriorityVoice) != 0, "asset.ioPriority voice");
  expect(Cell<col::ASST_loadForm>(v, c) == 1, "asset.loadForm compressed");
  expect(Cell<col::ASST_sampleRate>(v, c) == 24000 && Cell<col::ASST_channels>(v, c) == 2, "asset.convert rate/channels");
  expect(Cell<col::ASST_loadForm>(v, s) == 3 && Cell<col::ASST_stream>(v, s) != bf::kNone, "asset.loadForm stream");
  expect(Cell<col::ASST_preloadFrames>(v, s) >= 2400 && Cell<col::ASST_preloadFrames>(v, s) < 2400 + 64, "asset.preloadMs");
  expect(Cell<col::ASST_seekCount>(v, s) >= 4, "asset.seekIntervalMs");
  // Events.
  const std::uint32_t e = v.FindRow<col::EVNT_id>(HashId("E"));
  SND_REQUIRE(e != bf::kNone);
  expect(Cell<col::EVNT_bus>(v, e) == sfx, "event.bus");
  expect(Cell<col::EVNT_category>(v, e) == cat, "event.category");
  expect(Cell<col::EVNT_limitGroup>(v, e) == g, "event.limitGroup");
  expect(Cell<col::EVNT_priority>(v, e) == 201, "event.priority");
  expect(Cell<col::EVNT_sequenceKind>(v, e) == 5, "event.sequence");
  expect(Cell<col::EVNT_flags>(v, e) == (bf::kEvent3D | bf::kEventIgnorePause), "event.is3D/ignorePause");
  expect(Cell<col::EVNT_maxInstances>(v, e) == 4, "event.maxInstances");
  expect(Cell<col::EVNT_retriggerMs>(v, e) == 33, "event.retriggerMs");
  expect(Cell<col::EVNT_fadeOutMs>(v, e) == 44, "event.fadeOutMs");
  expect(Cell<col::EVNT_comboWindowMs>(v, e) == 55, "event.comboWindowMs");
  expect(Cell<col::EVNT_maxDistance>(v, e) == 66, "event.maxDistance");
  expect(Cell<col::EVNT_coneInnerDeg>(v, e) == 30 && Cell<col::EVNT_coneOuterDeg>(v, e) == 90 &&
             Cell<col::EVNT_coneOuterDb>(v, e) == -6,
         "event.cone");
  expect(Cell<col::EVNT_virtualReturn>(v, e) == 2, "event.virtualReturn");
  {
    const std::uint32_t cols[5] = {Cell<col::EVNT_curveVolume>(v, e), Cell<col::EVNT_curveLowpass>(v, e),
                                   Cell<col::EVNT_curveSpread>(v, e), Cell<col::EVNT_curveReverbSend>(v, e),
                                   Cell<col::EVNT_curveBlend3D>(v, e)};
    const std::uint32_t targets[5] = {0, 2, 7, 8, 9};
    for (int k = 0; k < 5; ++k) {
      const bool okc = cols[k] != bf::kNone && Cell<col::CURV_input>(v, cols[k]) == bf::kInputDistance &&
                       Cell<col::CURV_target>(v, cols[k]) == targets[k] &&
                       Cell<col::CPNT_y>(v, Cell<col::CURV_pointFirst>(v, cols[k]) + 1) == static_cast<float>(-1 - k);
      expect(okc, "event.distanceCurves.*");
    }
  }
  expect(Cell<col::EVNT_curveCount>(v, e) == 1, "event.curves");
  if (Cell<col::EVNT_curveCount>(v, e) == 1) {
    const std::uint32_t cv = Cell<col::EVNT_curveFirst>(v, e);
    expect(Cell<col::CURV_input>(v, cv) == HashId("Speed") && Cell<col::CURV_target>(v, cv) == 4 &&
               Cell<col::CURV_compose>(v, cv) == 3,
           "curves[].parameter/target/compose");
    expect(Cell<col::CURV_pointCount>(v, cv) == 2 &&
               Cell<col::CPNT_shape>(v, Cell<col::CURV_pointFirst>(v, cv) + 1) == 2,
           "curves[].points shape");
  }
  expect(Cell<col::EVNT_trackCount>(v, e) == 1, "event.tracks");
  const std::uint32_t tk = Cell<col::EVNT_trackFirst>(v, e);
  expect(Cell<col::TRAK_bus>(v, tk) == rev, "tracks[].bus");
  expect(Cell<col::TRAK_layerParam>(v, tk) == HashId("Speed"), "tracks[].layerParameter");
  expect(Cell<col::TRAK_flags>(v, tk) == 3, "tracks[].flags");
  expect(Cell<col::TRAK_clipCount>(v, tk) == 4, "tracks[].clips");
  const std::uint32_t c0 = Cell<col::TRAK_clipFirst>(v, tk);
  expect(Cell<col::CLIP_kind>(v, c0) == 0 && Cell<col::CLIP_target>(v, c0) == r, "clip.asset");
  expect(Cell<col::CLIP_startMs>(v, c0) == 11 && Cell<col::CLIP_weight>(v, c0) == 2.5f &&
             Cell<col::CLIP_volumeDb>(v, c0) == -1 && Cell<col::CLIP_pitchCents>(v, c0) == 12,
         "clip.startMs/weight/volumeDb/pitchCents");
  expect(Cell<col::CLIP_randVolumeDb>(v, c0) == 1 && Cell<col::CLIP_randPitchCents>(v, c0) == 2 &&
             Cell<col::CLIP_randPan>(v, c0) == 0.3f && Cell<col::CLIP_randStartMs>(v, c0) == 4 &&
             Cell<col::CLIP_randFilterHz>(v, c0) == 500,
         "clip.random.*");
  expect(Cell<col::CLIP_switchValue>(v, c0) == HashId("Grass"), "clip.switchValue (name)");
  expect(Cell<col::CLIP_switchValue>(v, c0 + 1) == 7, "clip.switchValue (integer)");
  expect(Cell<col::CLIP_layerMin>(v, c0) == 0.1f && Cell<col::CLIP_layerMax>(v, c0) == 0.9f, "clip.layer");
  expect(Cell<col::CLIP_fadeInMs>(v, c0) == 7 && Cell<col::CLIP_fadeOutMs>(v, c0) == 8, "clip.fadeIn/OutMs");
  expect(Cell<col::CLIP_loopCount>(v, c0) == 0, "clip.loopCount 0");
  const std::uint32_t n = v.FindRow<col::EVNT_id>(HashId("N"));
  expect(Cell<col::CLIP_kind>(v, c0 + 1) == 1 && Cell<col::CLIP_target>(v, c0 + 1) == n, "clip.event");
  expect(Cell<col::CLIP_kind>(v, c0 + 2) == 2, "clip.synth");
  {
    const std::uint32_t sy = Cell<col::CLIP_target>(v, c0 + 2);
    expect(Cell<col::SYNP_kind>(v, sy) == 0 && Cell<col::SYNP_waveform>(v, sy) == 4 &&
               Cell<col::SYNP_frequencyHz>(v, sy) == 330 && Cell<col::SYNP_duty>(v, sy) == 0.25f &&
               Cell<col::SYNP_noiseColor>(v, sy) == 1 && Cell<col::SYNP_channels>(v, sy) == 2,
           "synth.*");
    const std::uint32_t wt = Cell<col::CLIP_target>(v, c0 + 3);
    expect(Cell<col::SYNP_kind>(v, wt) == 4 && Cell<col::SYNP_asset>(v, wt) == r, "synth.asset");
  }
  expect(Cell<col::TRAK_actionCount>(v, tk) == 2, "tracks[].actions");
  {
    const std::uint32_t a = Cell<col::TRAK_actionFirst>(v, tk);
    expect(Cell<col::ACTN_timeMs>(v, a) == 100 && Cell<col::ACTN_action>(v, a) == 3 &&
               Cell<col::ACTN_target>(v, a) == HashId("Snap") && Cell<col::ACTN_value>(v, a) == 1,
           "actions[] applySnapshot");
    expect(Cell<col::ACTN_action>(v, a + 1) == 0 && Cell<col::ACTN_target>(v, a + 1) == HashId("N"), "actions[] play");
  }
  expect((Cell<col::EVNT_flags>(v, n) & bf::kEventStinger) != 0, "event.stinger");
  const std::uint32_t pe = v.FindRow<col::EVNT_id>(HashId("P"));
  expect((Cell<col::EVNT_flags>(v, pe) & bf::kEventNeedsPriming) != 0, "preloadMs 0 -> needs priming");
  const std::uint32_t se = v.FindRow<col::EVNT_id>(HashId("S"));
  expect((Cell<col::EVNT_flags>(v, se) & bf::kEventNeedsPriming) == 0, "preloadMs 50 -> no priming");
  // Music.
  const std::uint32_t mu = v.FindRow<col::EVNT_id>(HashId("Music"));
  expect((Cell<col::EVNT_flags>(v, mu) & bf::kEventMusic) != 0, "music flag");
  const std::uint32_t battle = v.FindRow<col::MSTA_id>(HashId("Battle"));
  expect(Cell<col::EVNT_musicState>(v, mu) == battle, "music.initialState");
  const std::uint32_t pl = v.FindRow<col::MPLS_id>(HashId("PL"));
  expect(Cell<col::MSTA_playlist>(v, battle) == pl && Cell<col::MSTA_musicEvent>(v, battle) == mu, "music.states");
  expect(v.Rows(ch::MTRN) == 2, "music.transitions");
  if (v.Rows(ch::MTRN) == 2) {
    const std::uint32_t seg2 = v.FindRow<col::MSEG_id>(HashId("Seg2"));
    expect(Cell<col::MTRN_fromState>(v, 0) == HashId("Explore") && Cell<col::MTRN_toState>(v, 0) == HashId("Battle"),
           "transitions[] order / from / to");
    expect(Cell<col::MTRN_timing>(v, 0) == 3 && Cell<col::MTRN_timingCount>(v, 0) == 2, "transitions[].timing");
    expect(Cell<col::MTRN_fadeOutBeats>(v, 0) == 0.5f && Cell<col::MTRN_fadeInBeats>(v, 0) == 1.5f, "transitions[].fade");
    expect(Cell<col::MTRN_bridgeSegment>(v, 0) == seg2, "transitions[].bridge");
    expect(Cell<col::MTRN_startPosition>(v, 0) == 2 && Cell<col::MTRN_startMarker>(v, 0) == HashId("M1"),
           "transitions[].start/startMarker");
    expect(Cell<col::MTRN_interrupt>(v, 0) == 1 && Cell<col::MTRN_lateRule>(v, 0) == 2 &&
               Cell<col::MTRN_flags>(v, 0) == 1,
           "transitions[].interrupt/late/prepare");
    expect(Cell<col::MTRN_fromState>(v, 1) == 0 && Cell<col::MTRN_toState>(v, 1) == 0, "transitions[] wildcard");
  }
  expect(v.Rows(ch::MSTG) == 1 && Cell<col::MSTG_id>(v, 0) == HashId("Sting1") && Cell<col::MSTG_event>(v, 0) == n &&
             Cell<col::MSTG_timing>(v, 0) == 4 && Cell<col::MSTG_overlap>(v, 0) == 1,
         "music.stingers");
  const std::uint32_t seg = v.FindRow<col::MSEG_id>(HashId("Seg"));
  const std::uint32_t m = v.FindRow<col::ASST_id>(HashId("m"));
  expect(seg != bf::kNone && Cell<col::MSEG_asset>(v, seg) == m && Cell<col::MSEG_bars>(v, seg) == 4, "segment.asset/bars");
  expect(Cell<col::MSEG_tempoCount>(v, seg) == 2, "segment.tempo");
  {
    const std::uint32_t t0 = Cell<col::MSEG_tempoFirst>(v, seg);
    expect(Cell<col::MTMP_bpm>(v, t0) == 100 && Cell<col::MTMP_beatsPerBar>(v, t0) == 3 &&
               Cell<col::MTMP_beatUnit>(v, t0) == 8 && Cell<col::MTMP_tick>(v, t0 + 1) == 3840 &&
               Cell<col::MTMP_bpm>(v, t0 + 1) == 140,
           "segment.tempo[]");
  }
  expect(Cell<col::MSEG_exitFrame>(v, seg) == 90000, "segment.exitFrame");
  expect(Cell<col::MSEG_markerCount>(v, seg) == 1 &&
             Cell<col::MMRK_id>(v, Cell<col::MSEG_markerFirst>(v, seg)) == HashId("M1") &&
             Cell<col::MMRK_frame>(v, Cell<col::MSEG_markerFirst>(v, seg)) == 2000 &&
             Cell<col::MMRK_kind>(v, Cell<col::MSEG_markerFirst>(v, seg)) == 3,
         "segment.markers");
  expect(Cell<col::MSEG_flags>(v, seg) == 1, "segment.entryPreload");
  expect(v.Rows(ch::MSTM) == 1 && Cell<col::MSTM_segment>(v, 0) == seg && Cell<col::MSTM_channelFirst>(v, 0) == 1 &&
             Cell<col::MSTM_channelCount>(v, 0) == 1 && Cell<col::MSTM_asset>(v, 0) == m &&
             Cell<col::MSTM_volumeCurve>(v, 0) != bf::kNone,
         "segment.stems");
  expect(pl != bf::kNone && Cell<col::MPLS_mode>(v, pl) == 2 && Cell<col::MPLS_loopCount>(v, pl) == 3 &&
             Cell<col::MPLS_entryCount>(v, pl) == 2,
         "playlist.mode/loopCount/entries");
  if (pl != bf::kNone) {
    const std::uint32_t e0 = Cell<col::MPLS_entryFirst>(v, pl);
    expect(Cell<col::MPLE_segment>(v, e0) == seg && Cell<col::MPLE_weight>(v, e0) == 2 &&
               Cell<col::MPLE_repeat>(v, e0) == 3,
           "playlist.entries[]");
  }
  // Material table (9.3): pair (glass, metal) lands at min*N+max = 0*3+2.
  {
    PhysTableView pt;
    expect(v.PhysTableCount() == 1 && v.PhysTable(0, &pt), "materialTable");
    if (v.PhysTableCount() == 1 && v.PhysTable(0, &pt)) {
      expect(pt.id == HashId("Phys") && pt.materialCount == 3, "materialTable.materials");
      expect(pt.single[0] == HashId("E") && pt.single[1] == HashId("N") && pt.single[2] == HashId("S") &&
                 pt.single[3] == HashId("P"),
             "materialTable.single");
      expect(pt.pair[0 * 3 + 2] == HashId("N") && pt.pair[2 * 3 + 0] == 0, "materialTable.pairs (min*N+max only)");
      expect(pt.footstep[1] == HashId("E"), "materialTable.footsteps");
      expect(pt.defaultEvent[0] == HashId("S"), "materialTable.defaults");
      expect(pt.curves[1].impulseToVelocity[3] == 4 && pt.curves[1].massToPitchCents[1] == 6 &&
                 pt.curves[1].speedToVolumeDb[0] == 7,
             "materialTable.curves");
    }
  }
  // Names, aliases, GUIDs (dev build default).
  expect(v.Name(HashId("E")) != nullptr && std::strcmp(v.Name(HashId("E")), "E") == 0, "NAME");
  std::uint64_t lo = 0, hi = 0;
  expect(v.Guid(HashId("E"), &lo, &hi) && hi == 0x7e57000000004000ull, "GUID (hi = first 16 hex digits)");
  std::string list;
  for (const std::string& s2 : missing) list += "  " + s2 + "\n";
  SND_CHECK_MSG(missing.empty(), "%zu setting(s) did not come back from the bank:\n%s", missing.size(), list.c_str());

  // Language banks: one per language, same bankId, languageId = hash(language), own asset.
  for (const char* lang : {"ja", "en"}) {
    Loaded lb(p.out() / (std::string("Voice.") + lang + ".sndb"));
    REQUIRE_MSG(lb.result == SND_OK, "Voice.%s: %s", lang, lb.err.message);
    SND_CHECK_EQ(lb.v.bankId(), HashId("Voice"));
    SND_CHECK_EQ(lb.v.languageId(), HashId(lang));
    SND_CHECK_EQ(lb.v.dependencyCount(), 1u);
    SND_CHECK_EQ(lb.v.dependency(0).bankId, HashId("Main"));
    SND_CHECK_EQ(lb.v.dependency(0).buildHash, v.contentHash());
    const std::uint32_t va = lb.v.FindRow<col::ASST_id>(HashId("v"));
    SND_CHECK_EQ(Cell<col::ASST_totalFrames>(lb.v, va), std::strcmp(lang, "ja") == 0 ? 900u : 700u);
  }
}

// Streams: sector-aligned data, file size multiple of the sector, SEEK byte
// offsets inside the data and on frame boundaries, preload = head of the data.
SND_TEST(BuildReadback, StreamFilesSeekTablesAndPreload) {
  Project p("streams");
  WriteEverything(p);
  bt::Diagnostics d;
  REQUIRE_MSG(p.Build(&d), "%s", mini::Dump(d).c_str());
  Loaded L(p.out() / "Main.sndb");
  REQUIRE_MSG(L.result == SND_OK, "%s", L.err.message);
  const BankView& v = L.v;
  SND_REQUIRE(v.Rows(ch::ASST) > 0);
  int streams = 0;
  for (std::uint32_t a = 0; a < v.Rows(ch::ASST); ++a) {
    if (Cell<col::ASST_loadForm>(v, a) != 3) continue;
    ++streams;
    const std::uint32_t sr = Cell<col::ASST_stream>(v, a);
    const char* path = v.StreamPath(sr);
    SND_REQUIRE(path != nullptr);
    const std::vector<std::uint8_t> f = mini::ReadBytes(p.out() / path);
    const std::uint64_t align = Cell<col::STRM_sectorAlign>(v, sr);
    const std::uint64_t dataOff = Cell<col::STRM_dataOffset>(v, sr);
    SND_CHECK_EQ(align, 4096u);  // project.platforms.default.sectorAlign
    SND_CHECK_EQ(f.size(), Cell<col::STRM_fileBytes>(v, sr));
    SND_CHECK_EQ(f.size() % align, 0u);
    SND_CHECK_EQ(dataOff % align, 0u);
    std::uint32_t magic = 0;
    std::memcpy(&magic, f.data(), 4);
    SND_CHECK_EQ(magic, snd::Fourcc('S', 'N', 'D', 'S'));
    std::uint64_t dataBytes = 0;
    std::memcpy(&dataBytes, f.data() + 40, 8);
    SND_CHECK(dataOff + dataBytes <= f.size());
    const std::uint32_t blockAlign = Cell<col::ASST_blockAlign>(v, a);
    SND_CHECK_EQ(dataBytes, Cell<col::ASST_totalFrames>(v, a) * blockAlign);
    // Seek table.
    const snd::RangeU32 sk = v.SeekRange(a);
    std::uint64_t lastFrame = 0;
    for (std::uint32_t i = 0; i < sk.count; ++i) {
      const std::uint64_t fr = Cell<col::SEEK_frame>(v, sk.first + i);
      const std::uint64_t off = Cell<col::SEEK_byteOffset>(v, sk.first + i);
      SND_CHECK(i == 0 || fr > lastFrame);
      SND_CHECK_MSG(off == fr * blockAlign, "seek %u: frame %llu at byte %llu (blockAlign %u)", i,
                    static_cast<unsigned long long>(fr), static_cast<unsigned long long>(off), blockAlign);
      SND_CHECK(off < dataBytes);
      lastFrame = fr;
    }
    // Preload: the first preloadBytes of the data.
    const ByteSpan pre = v.Preload(a);
    if (pre.bytes > 0) {
      SND_CHECK(pre.bytes <= dataBytes);
      SND_CHECK(std::memcmp(pre.data, f.data() + dataOff, pre.bytes) == 0);
      SND_CHECK_EQ(pre.bytes, std::uint64_t{Cell<col::ASST_preloadFrames>(v, a)} * blockAlign);
    }
  }
  SND_CHECK(streams >= 3);
}

// Release + strip strings: no GUID chunk, no dev flag, no STRS / NAME; ids still resolve.
SND_TEST(BuildReadback, ReleaseAndStrippedBanks) {
  Project p("release");
  WriteEverything(p);
  bt::BuildOptions o = p.Options();
  o.devBuild = false;
  o.stripStrings = true;
  bt::Diagnostics d;
  REQUIRE_MSG(p.Build(o, &d), "%s", mini::Dump(d).c_str());
  Loaded L(p.out() / "Main.sndb");
  REQUIRE_MSG(L.result == SND_OK, "%s", L.err.message);
  SND_CHECK(!L.v.devBuild());
  SND_CHECK(!L.v.HasChunk(ch::GUID));
  SND_CHECK(!L.v.HasChunk(ch::STRS));
  SND_CHECK(!L.v.HasChunk(ch::NAME));
  SND_CHECK((L.v.header().flags & bf::kFileHasStrings) == 0u);
  SND_CHECK(L.v.FindRow<col::EVNT_id>(HashId("E")) != bf::kNone);
  SND_CHECK(L.v.Name(HashId("E")) == nullptr);
  // Stream paths survive stripping (SPTH is not strippable).
  const std::uint32_t s = L.v.FindRow<col::ASST_id>(HashId("s"));
  SND_CHECK(L.v.StreamPath(Cell<col::ASST_stream>(L.v, s)) != nullptr);
  // Release-but-named: NAME present, GUID absent.
  o.stripStrings = false;
  std::error_code ec;
  fs::remove_all(p.out(), ec);
  REQUIRE_MSG(p.Build(o, &d), "%s", mini::Dump(d).c_str());
  Loaded L2(p.out() / "Main.sndb");
  SND_REQUIRE(L2.result == SND_OK);
  SND_CHECK(L2.v.HasChunk(ch::NAME) && !L2.v.HasChunk(ch::GUID));
}

// Every bank the build accepts must load in the runtime. A stem whose
// channels exceed the asset is accepted by the build and rejected by
// ParseBank (MSTM channel range).
// Defect: build.cpp does not check stems[].channelFirst/channelCount against
// the (converted) channel count of the segment / stem asset.
SND_TEST(BuildReadback, Defect_BuildAcceptsStemsTheRuntimeRejects) {
  Project p("stems");
  p.Add("banks/Main.json", "bank", "Main");
  p.Add("buses/Master.json", "bus", "Master", "\"master\": true");
  p.Wav("audio/m.wav", mini::Tone(2, 48000, 4800));
  p.Add("assets/m.json", "asset", "m", "\"file\": \"audio/m.wav\"");
  p.Add("music/Seg.json", "segment", "Seg", "\"asset\": \"m\", \"bars\": 1, \"stems\": [{\"channelFirst\": 1, \"channelCount\": 2}]");
  p.Add("music/PL.json", "playlist", "PL", "\"entries\": [{\"segment\": \"Seg\"}]");
  p.Add("events/M.json", "event", "M", "\"bank\": \"Main\", \"music\": {\"states\": [{\"name\": \"A\", \"playlist\": \"PL\"}]}");
  bt::Diagnostics d;
  const bool ok = p.Build(&d);
  if (!ok) {
    snd::test::Note("build rejected the stem (fine): %s", mini::Dump(d).c_str());
    return;
  }
  Loaded L(p.out() / "Main.sndb");
  SND_CHECK_MSG(L.result == SND_OK, "the build wrote a bank the runtime rejects: %s", L.err.message);
}

// Defect: references by runtime id (action targets, material table events)
// skip the bank access rule of project-format.md §3 ("using an object of
// bank A from bank B requires B to depend on A"): the build succeeds although
// the referenced event is in a bank the user does not depend on, so the
// runtime can never resolve it.
SND_TEST(BuildReadback, Defect_RuntimeIdReferencesSkipTheBankAccessRule) {
  for (int variant = 0; variant < 2; ++variant) {
    Project p("access");
    p.Add("banks/Main.json", "bank", "Main");
    p.Add("banks/Other.json", "bank", "Other");
    p.Add("buses/Master.json", "bus", "Master", "\"master\": true");
    p.Add("events/X.json", "event", "X", "\"bank\": \"Other\"");
    if (variant == 0) {
      p.Add("events/U.json", "event", "U",
            "\"bank\": \"Main\", \"tracks\": [{\"actions\": [{\"action\": \"play\", \"target\": \"X\"}]}]");
    } else {
      p.Add("physics/T.json", "materialTable", "T",
            "\"bank\": \"Main\", \"materials\": [\"m\"], \"defaults\": {\"impact\": \"X\"}");
    }
    bt::Diagnostics d;
    const bool ok = p.Build(&d);
    SND_CHECK_MSG(!ok, "%s referencing an event of a bank 'Main' does not depend on was accepted",
                  variant == 0 ? "an action" : "a material table");
  }
}

// 0-frame WAV, > 8 channels, sample rate 0 conversions: errors, never a bank
// the runtime rejects.
SND_TEST(BuildReadback, DegenerateWavInputs) {
  struct Case {
    const char* what;
    snd::buildtool::PcmData pcm;
    const char* convert;
  };
  std::vector<Case> cases = {
      {"0 frames", mini::Tone(1, 48000, 0), ""},
      {"16 channels kept", mini::Tone(16, 48000, 100), ""},
      {"16 channels -> 2", mini::Tone(16, 48000, 100), ", \"convert\": {\"default\": {\"channels\": 2}}"},
      {"1 Hz", mini::Tone(1, 1, 3), ""},
      {"upsample 8k -> 384k", mini::Tone(1, 8000, 100), ", \"convert\": {\"default\": {\"sampleRate\": 384000}}"},
  };
  for (Case& c : cases) {
    Project p("degen");
    p.Add("banks/Main.json", "bank", "Main");
    p.Add("buses/Master.json", "bus", "Master", "\"master\": true");
    p.Wav("audio/a.wav", c.pcm);
    p.Add("assets/a.json", "asset", "a", std::string("\"file\": \"audio/a.wav\"") + c.convert);
    p.Add("events/E.json", "event", "E", "\"bank\": \"Main\", \"tracks\": [{\"clips\": [{\"asset\": \"a\"}]}]");
    bt::Diagnostics d;
    const bool ok = p.Build(&d);
    if (!ok) {
      snd::test::Note("%s: build error (%s)", c.what, d.items().empty() ? "" : d.items()[0].message.c_str());
      continue;
    }
    Loaded L(p.out() / "Main.sndb");
    SND_CHECK_MSG(L.result == SND_OK, "%s: build ok but ParseBank: %s", c.what, L.err.message);
    if (L.result == SND_OK) {
      SND_CHECK(specwalk::CheckInvariants(L.v).empty());
      (void)specwalk::DeepWalk(L.v);
    }
  }
}

// Structure-aware fuzz on a real buildtool bank (the everything project):
// the same boundary sweep as bank_struct_fuzz_test, different layout.
SND_TEST(BuildReadback, FuzzTheBuiltBank) {
  Project p("fuzzbuilt");
  WriteEverything(p);
  bt::Diagnostics d;
  REQUIRE_MSG(p.Build(&d), "%s", mini::Dump(d).c_str());
  const std::vector<std::uint8_t> base = mini::ReadBytes(p.out() / "Main.sndb");
  bf::FileHeader h;
  std::memcpy(&h, base.data(), sizeof h);
  std::uint32_t accepted = 0, violations = 0;
  std::string first;
  std::uint64_t s = 0x5EEDBA5Eull;
  auto rnd = [&]() {
    s ^= s << 13;
    s ^= s >> 7;
    s ^= s << 17;
    return s;
  };
  static const std::uint64_t vals[] = {0, 1, 63, 64, 0x7FFFFFFFu, 0xFFFFFFFEu, 0xFFFFFFFFu, 0xFFFFFFFFFFFFFFFFull};
  for (int it = 0; it < 20000; ++it) {
    std::vector<std::uint8_t> w = base;
    // Pick a chunk, then a 4- or 8-byte aligned word in its first 512 bytes or anywhere.
    bf::ChunkEntry e;
    std::memcpy(&e, base.data() + h.chunkTableOffset + (rnd() % h.chunkCount) * 32, sizeof e);
    const std::uint64_t span = e.bytes == 0 ? 1 : (e.bytes < 512 || rnd() % 2 ? std::min<std::uint64_t>(e.bytes, 512) : e.bytes);
    const std::uint64_t pos = (e.offset + rnd() % span) & ~std::uint64_t{3};
    const std::uint8_t size = (rnd() % 2) ? 4 : 8;
    if (pos + size > w.size()) continue;
    const std::uint64_t val = vals[rnd() % (sizeof vals / sizeof vals[0])];
    std::memcpy(w.data() + pos, &val, size);
    // Re-sign.
    bf::FileHeader wh;
    std::memcpy(&wh, w.data(), sizeof wh);
    wh.checksum = bf::Crc32c(w.data() + 128, w.size() - 128);
    std::memcpy(w.data(), &wh, sizeof wh);
    wh.headerChecksum = bf::HeaderChecksum(w.data());  // format 2.0 (was Crc32c(w, 52))
    std::memcpy(w.data(), &wh, sizeof wh);
    void* m = nullptr;
    SND_REQUIRE(posix_memalign(&m, 64, w.size()) == 0);
    std::memcpy(m, w.data(), w.size());
    BankView v;
    ParseError err;
    if (ParseBank(m, w.size(), ParseOptions{}, &v, &err) == SND_OK) {
      ++accepted;
      const std::string bad = specwalk::CheckInvariants(v);
      if (!bad.empty()) {
        ++violations;
        if (first.empty()) first = bad;
      } else {
        (void)specwalk::DeepWalk(v);
      }
    }
    std::free(m);
  }
  snd::test::Note("built-bank fuzz: %u accepted of 20000", accepted);
  SND_CHECK_MSG(violations == 0, "%u violations, first: %s", violations, first.c_str());
}

// Rename aliases (10.1, project-format §1): the NAME table keeps the old name
// (kind Alias). Reports which id the alias row carries and whether an old id
// can be resolved at runtime (release builds strip NAME / STRS).
SND_TEST(BuildReadback, AliasRows) {
  Project p("alias");
  p.Minimal();
  p.Add("events/R.json", "event", "New/Name", "\"bank\": \"Main\", \"aliases\": [\"Old/Name\"]");
  bt::Diagnostics d;
  REQUIRE_MSG(p.Build(&d), "%s", mini::Dump(d).c_str());
  Loaded L(p.out() / "Main.sndb");
  REQUIRE_MSG(L.result == SND_OK, "%s", L.err.message);
  const BankView& v = L.v;
  bool found = false;
  for (std::uint32_t r = 0; r < v.Rows(ch::NAME); ++r) {
    if (Cell<col::NAME_kind>(v, r) != static_cast<std::uint32_t>(bf::NameKind::Alias)) continue;
    const char* s = v.String(Cell<col::NAME_stringOffset>(v, r));
    if (s != nullptr && std::strcmp(s, "Old/Name") == 0) {
      found = true;
      const std::uint32_t id = Cell<col::NAME_id>(v, r);
      snd::test::Note("alias row: id %08x (%s), string 'Old/Name'", id,
                      id == HashId("New/Name") ? "the current id" : (id == HashId("Old/Name") ? "the old id" : "?"));
    }
  }
  SND_CHECK(found);
  SND_CHECK(v.Name(HashId("New/Name")) != nullptr && std::strcmp(v.Name(HashId("New/Name")), "New/Name") == 0);
  SND_CHECK(v.FindRow<col::EVNT_id>(HashId("Old/Name")) == bf::kNone);
}
