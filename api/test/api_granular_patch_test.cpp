// D-07: JSON -> SNDB -> public C API -> captured 48 kHz device samples.
#include <algorithm>
#include <cmath>
#include <complex>
#include <cstring>
#include <vector>
#include <atomic>
#include <new>
#include <thread>
#include "api_rig.hpp"
#include "mini_project.hpp"
#include "snd/format/bank_format.hpp"

namespace {
using apitest::Rig;
using Bytes = std::vector<std::uint8_t>;
namespace bf = snd::bank_format;
constexpr double tau = 6.283185307179586;
constexpr unsigned rate = 48000;

void Event(mini::Project& p, const std::string& name, const std::string& synth,
           const std::string& settings = "") {
  p.Add("events/" + name + ".json", "event", name, R"("bank":"Main",)" + settings +
        R"("tracks":[{"clips":[{"synth":)" + synth + "}]}]");
}

std::string Patch(unsigned hz) {
  // Out of declaration order; independently check frequency and gain on the device.
  return R"({"kind":"patch","output":"out","nodes":[{"id":"out","op":"output","inputs":["amp"]},
    {"id":"amp","op":"multiply","inputs":["osc","g"]},{"id":"g","op":"constant","args":[0.25]},
    {"id":"osc","op":"oscillator","waveform":"sine","args":[)" + std::to_string(hz) + ",0.5]}]}";
}

Bytes Build(const std::string& tag, unsigned patchHz = 937, const char* codec = "PC16", double amplitude = 0.4) {
  mini::Project p("d07-" + tag);
  p.Add("banks/Main.json", "bank", "Main");
  p.Add("buses/Master.json", "bus", "Master", R"("master":true,"channels":2)");
  const unsigned materialRate = 24000;
  auto pcm = mini::Tone(1, materialRate, materialRate * 2);
  for (unsigned i = 0; i < pcm.frames; ++i)
    pcm.samples[i] = static_cast<float>(amplitude * std::sin(tau * (i < materialRate ? 600 : 1500) * i / materialRate));
  p.Wav("grain.wav", pcm, snd::buildtool::WavSampleFormat::Float32);
  p.Add("assets/PCM.json", "asset", "PCM", std::string(R"("file":"grain.wav","loadForm":"pcm","convert":{"default":{"codec":")") +
        codec + R"(","sampleRate":24000}})");
  Event(p, "Patch", Patch(patchHz), R"("priority":240,)");
  const std::string grain = R"({"kind":"granular","asset":"PCM","window":"rectangle",)";
  Event(p, "Continuous", grain + R"("density":1,"lengthMs":1000})", R"("priority":1,)");
  p.Add("parameters/Control.json", "parameter", "Control", R"("min":0,"max":1,"default":0)");
  struct Curve { const char* target; double offset; const char* base; };
  const Curve curves[] = {
    {"grainDensity",20,R"("density":0,"lengthMs":50)"},
    {"grainLengthMs",47,R"("density":20,"lengthMs":3)"},
    {"grainPosition",0.6,R"("density":20,"lengthMs":50)"},
    {"grainPitchJitterCents",1600,R"("density":20,"lengthMs":50)"}
  };
  for (const auto& c : curves) {
    const auto setting = std::string(R"("curves":[{"parameter":"Control","target":")") + c.target +
        R"(","points":[[0,0],[1,)" + std::to_string(c.offset) + "]]}],";
    Event(p, c.target, grain + c.base + "}", setting);
  }
  snd::buildtool::Diagnostics diag;
  const bool ok = p.Build(&diag);
  SND_CHECK_MSG(ok, "%s", mini::Dump(diag).c_str());
  return ok ? mini::ReadBytes(p.out() / "Main.sndb") : Bytes{};
}

snd_playback_handle Play(Rig& r, const char* name) {
  return snd_play(r.sys, snd_hash_id(name), {}, nullptr);
}
double Tail(const Rig& r, double hz, unsigned n = 4096) {
  return apitest::Level(r, r.Frames() - n, n, hz);
}
double TailRms(const Rig& r, unsigned n) {
  double squares = 0;
  for (auto i = r.Frames() - n; i < r.Frames(); ++i) squares += double(r.At(i)) * r.At(i);
  return std::sqrt(squares / n);
}

// Project the captured tone onto the oscillator at the absolute device clock.
std::complex<double> Phase(const Rig& r, double hz) {
  std::complex<double> sum{};
  for (auto i = r.Frames() - 2400; i < r.Frames(); ++i)
    sum += double(r.At(i)) * std::polar(1.0, -tau * hz * double(i) / rate);
  return sum / 1200.0;
}

SND_TEST(ApiGranularPatch, VirtualReturnPreservesMaterialClock) {
  const auto bank = Build("virtual");
  // A noninteger source-frame advance exercises the restored Q32 phase.
  for (float pitch : {1.f, 1.00390625f, 1.0010376f}) {
    auto cfg = apitest::SmallConfig();
    cfg.max_real_voices = 1;
    cfg.fade_reserve_voices = 1;
    Rig r(cfg);
    SND_REQUIRE_EQ(r.created, SND_OK);
    SND_REQUIRE(!bank.empty() && r.LoadBank("Main.sndb", bank));
    const auto grains = Play(r, "Continuous");
    SND_REQUIRE(grains.value);
    SND_REQUIRE_EQ(snd_set_playback_pitch(r.sys, grains, pitch), SND_OK);
    r.Step(32);
    const auto before = Phase(r, 600 * pitch);
    SND_REQUIRE(std::abs(before) > 0.37);
    SND_CHECK(Tail(r, 1200) < 0.005);
    const auto blocker = Play(r, "Patch");
    r.Step(37);
    snd_stats st{};
    SND_REQUIRE_EQ(snd_get_stats(r.sys, &st), SND_OK);
    SND_REQUIRE_EQ(st.virtual_voices, 1u);
    SND_CHECK(Tail(r, 600) < 0.005);
    SND_CHECK_NEAR(Tail(r, 937), 0.25, 0.008);
    SND_REQUIRE_EQ(snd_stop(r.sys, blocker, 0, nullptr), SND_OK);
    r.Step(29);
    SND_CHECK_EQ(snd_get_playback_state(r.sys, grains), SND_PLAYBACK_STATE_PLAYING);
    SND_CHECK_NEAR(std::abs(Phase(r, 600 * pitch) - before), 0.0, 0.008);
  }
}

SND_TEST(ApiGranularPatch, ParameterSetWhileVirtualAffectsFirstReturningGrain) {
  // Long grains born while inaudible must already have the new position when
  // promoted. Their audible pitch identifies which half of the PCM they use.
  mini::Project p("d07-hidden-param");
  p.Minimal();
  auto pcm = mini::Tone(1, 24000, 96000);
  for (unsigned f = 0; f < pcm.frames; ++f)
    pcm.samples[f] = static_cast<float>(0.4 * std::sin(tau * (f < 48000 ? 600 : 1500) * f / 24000.0));
  p.Wav("pcm.wav", pcm);
  p.Add("assets/PCM.json", "asset", "PCM", R"("file":"pcm.wav","loadForm":"pcm")");
  p.Add("parameters/Pos.json", "parameter", "Pos", R"("min":0,"max":1)");
  Event(p, "Low", R"({"kind":"granular","asset":"PCM","window":2,"density":4,"lengthMs":250})",
        R"("priority":1,"curves":[{"parameter":"Pos","target":"grainPosition","points":[[0,0],[1,0.6]]}],)");
  Event(p, "Block", Patch(937), R"("priority":240,)");
  snd::buildtool::Diagnostics diag;
  SND_REQUIRE(p.Build(&diag));
  auto cfg = apitest::SmallConfig();
  cfg.max_real_voices = 1;
  cfg.fade_reserve_voices = 1;
  Rig r(cfg);
  SND_REQUIRE(r.LoadBank("Main.sndb", mini::ReadBytes(p.out() / "Main.sndb")));
  const auto block = Play(r, "Block");
  const auto low = Play(r, "Low");
  r.Step(12);
  snd_stats stats{};
  SND_REQUIRE_EQ(snd_get_stats(r.sys, &stats), SND_OK);
  SND_REQUIRE_EQ(stats.virtual_voices, 1u);
  SND_REQUIRE_EQ(snd_set_param_playback(r.sys, low, snd_hash_id("Pos"), 1), SND_OK);
  r.Step(46);  // the second birth (250 ms) falls in the virtual interval
  SND_REQUIRE_EQ(snd_stop(r.sys, block, 0, nullptr), SND_OK);
  r.Step(20);  // before the third birth (500 ms)
  SND_CHECK_NEAR(Tail(r, 1500, 2048), 0.4, 0.015);
  SND_CHECK(Tail(r, 600, 2048) < 0.01);
}

SND_TEST(ApiGranularPatch, VirtualPatchHonorsGateAndReturnPolicy) {
  mini::Project project("d07-patch-gate");
  project.Minimal();
  const auto gated = R"({"kind":"patch","gateFrames":24576,"output":"out","nodes":[
    {"id":"vco","op":"oscillator","waveform":"sine","args":[700,0.5]},
    {"id":"env","op":"adsr","args":[0,0,1,0]},{"id":"amp","op":"multiply","inputs":["vco","env"]},
    {"id":"out","op":"output","inputs":["amp"]}]})";
  Event(project, "Continue", gated, R"("priority":1,"virtualReturn":"continue",)");
  Event(project, "Restart", gated, R"("priority":1,"virtualReturn":"restart",)");
  Event(project, "Block", Patch(937), R"("priority":240,)");
  snd::buildtool::Diagnostics diag;
  SND_REQUIRE(project.Build(&diag));
  const auto bank = mini::ReadBytes(project.out() / "Main.sndb");
  for (bool restart : {false, true}) {
    auto cfg = apitest::SmallConfig();
    cfg.max_real_voices = 1;
    cfg.fade_reserve_voices = 1;
    Rig r(cfg);
    SND_REQUIRE(r.LoadBank("Main.sndb", bank));
    const auto note = Play(r, restart ? "Restart" : "Continue");
    r.Step(20);
    SND_REQUIRE(Tail(r, 700, 2048) > 0.95);
    const auto block = Play(r, "Block");
    r.Step(32);
    snd_stats st{};
    SND_REQUIRE_EQ(snd_get_stats(r.sys, &st), SND_OK);
    SND_REQUIRE_EQ(st.virtual_voices, 1u);
    SND_REQUIRE_EQ(snd_stop(r.sys, block, 0, nullptr), SND_OK);
    r.Step(20);
    SND_CHECK(Tail(r, 700, 2048) > 0.95);
    r.Step(44);  // original gate elapsed, restarted gate still open
    SND_CHECK_EQ(Tail(r, 700, 2048) > 0.95, restart);
    SND_CHECK_EQ(snd_get_playback_state(r.sys, note) == SND_PLAYBACK_STATE_PLAYING, restart);
    r.Step(44);
    SND_CHECK(Tail(r, 700, 2048) < 0.005);
  }
}

SND_TEST(ApiGranularPatch, PlaybackParameterChangesEachGrainControl) {
  const auto bank = Build("curves", 937, "PCMF");
  SND_REQUIRE(!bank.empty());
  for (const std::string target : {"grainDensity","grainLengthMs","grainPosition","grainPitchJitterCents"}) {
    Rig r;
    SND_REQUIRE_EQ(r.created, SND_OK);
    SND_REQUIRE(r.LoadBank("Main.sndb", bank));
    const auto h = Play(r, target.c_str());
    r.Step(70);
    const double before = TailRms(r, 12000);
    if (target == "grainPosition" || target == "grainPitchJitterCents")
      SND_CHECK_NEAR(Tail(r, 600), 0.4, 0.01);
    SND_REQUIRE_EQ(snd_set_param_playback(r.sys, h, snd_hash_id("Control"), 1), SND_OK);
    r.Step(96);
    const auto after = TailRms(r, 12000);
    if (target == "grainDensity") {
      SND_CHECK(before < 0.001 && after > 0.26);
    } else if (target == "grainLengthMs") {
      SND_CHECK(before > 0.04 && before < 0.09 && after > 0.26);
    } else if (target == "grainPosition") {
      SND_CHECK(before > 0.26);
      SND_CHECK_NEAR(Tail(r, 1500), 0.4, 0.01);
      SND_CHECK(Tail(r, 600) < 0.01);
    } else {
      SND_CHECK(before > 0.26 && after > 0.25);
      SND_CHECK(Tail(r, 600, 12000) < 0.13);
    }
  }
}

SND_TEST(ApiGranularPatch, UnloadPlayingVoicesAndReloadDifferentBytes) {
  auto cfg = apitest::SmallConfig();
  cfg.max_banks = 1;  // force reuse of the old slot
  Rig r(cfg);
  SND_REQUIRE_EQ(r.created, SND_OK);
  snd_bank_handle previous{};
  for (unsigned generation = 0; generation < 2; ++generation) {
    const unsigned hz = generation ? 1703 : 937;
    const double amp = generation ? 0.16 : 0.4;
    const auto bytes = Build("reload" + std::to_string(generation), hz, generation ? "PCMF" : "PC16", amp);
    snd_bank_handle h{};
    SND_REQUIRE(!bytes.empty() && r.LoadBank("Main.sndb", bytes, &h));
    SND_CHECK(h.value != previous.value);
    const auto a = Play(r, "Continuous");
    const auto b = Play(r, "Patch");
    r.Step(36);
    SND_CHECK_NEAR(Tail(r, 600), amp, 0.01);
    SND_CHECK_NEAR(Tail(r, hz), 0.25, 0.01);
    SND_REQUIRE_EQ(snd_unload_bank(r.sys, h, SND_UNLOAD_STOP_AND_UNLOAD), SND_OK);
    for (int i = 0; i < 400 && snd_get_bank_state(r.sys, h) != SND_BANK_STATE_UNLOADED; ++i) r.Step();
    SND_REQUIRE_EQ(snd_get_bank_state(r.sys, h), SND_BANK_STATE_UNLOADED);
    for (auto voice : {a, b}) {
      SND_CHECK(std::count_if(r.ev.ends.begin(), r.ev.ends.end(), [&](const auto& end) {
        return end.handle == voice.value && end.reason == SND_END_DISCARDED;
      }) == 1);
    }
    previous = h;
  }
}

// A test-only pass-through voice effect holds a worker inside a real render
// chunk. init runs in serial preparation; the audio thread itself never waits.
struct Gate {
  std::atomic<bool> closed{false}, entered{false};
  static std::uint32_t SND_CALL Bytes(void*, const snd_effect_desc*) { return 64; }
  static void SND_CALL Init(void*, void* state, const snd_effect_desc*, std::uint32_t) {
    new (state) std::thread::id(std::this_thread::get_id());
  }
  static void SND_CALL Process(void* user, void* state, const float* const* in, float* const* out,
                               std::uint32_t channels, std::uint32_t frames, const snd_param_block*) {
    auto& gate = *static_cast<Gate*>(user);
    if (gate.closed.load() && *static_cast<std::thread::id*>(state) != std::this_thread::get_id()) {
      gate.entered.store(true);
      while (gate.closed.load()) std::this_thread::yield();
    }
    for (unsigned c = 0; c < channels; ++c)
      if (in[c] != out[c]) std::copy_n(in[c], frames, out[c]);
  }
  static std::int32_t SND_CALL Tail(void*, const void*) { return 1; }
};

SND_TEST(ApiGranularPatch, HeldRenderDelaysBankRelease) {
  Gate gate;
  snd_dsp_effect fx{&gate, snd_hash_id("d07.TestGate"), 0, Gate::Bytes, Gate::Init, Gate::Process, Gate::Tail};
  auto cfg = apitest::SmallConfig();
  cfg.max_banks = 1;
  cfg.max_voice_effects = 12;
  cfg.jobs.realtime_workers = 2;
  cfg.jobs.voices_per_chunk = 1;
  cfg.plugins.effects = &fx;
  cfg.plugins.effect_count = 1;
  Rig r(cfg);
  struct OpenOnExit { Gate& g; ~OpenOnExit() { g.closed.store(false); } } open{gate};
  SND_REQUIRE_EQ(r.created, SND_OK);
  const auto bytes = Build("held");
  snd_bank_handle bank{};
  SND_REQUIRE(!bytes.empty() && r.LoadBank("Main.sndb", bytes, &bank));
  for (auto event : {"Continuous", "Patch"}) {
    SND_REQUIRE_EQ(snd_set_event_voice_effect(r.sys, snd_hash_id(event), 0, fx.effect_type, nullptr, 0), SND_OK);
    for (unsigned i = 0; i < 6; ++i) SND_REQUIRE(Play(r, event).value);
  }
  r.Step(12);
  gate.closed.store(true);
  for (unsigned i = 0; i < 200 && !gate.entered.load(); ++i) r.Step();
  SND_REQUIRE(gate.entered.load());
  SND_REQUIRE_EQ(snd_unload_bank(r.sys, bank, SND_UNLOAD_STOP_AND_UNLOAD), SND_OK);
  r.Step(20);
  SND_CHECK_EQ(snd_get_bank_state(r.sys, bank), SND_BANK_STATE_UNLOADING);
  snd_stats stats{};
  SND_REQUIRE_EQ(snd_get_stats(r.sys, &stats), SND_OK);
  SND_CHECK(stats.late_chunks > 0);
  gate.closed.store(false);
  for (unsigned i = 0; i < 400 && snd_get_bank_state(r.sys, bank) != SND_BANK_STATE_UNLOADED; ++i) r.Step();
  SND_REQUIRE_EQ(snd_get_bank_state(r.sys, bank), SND_BANK_STATE_UNLOADED);
  SND_CHECK(r.LoadBank("Main.sndb", bytes));
}

// Locate fields in a known-good fixture. Recompute both CRCs after each edit so
// each load reaches the semantic checks, including with valid framing/invalid op.
template<class T> T Read(const Bytes& b, std::size_t offset) {
  T value;
  std::memcpy(&value, b.data() + offset, sizeof(value));
  return value;
}
void Checksums(Bytes& b) {
  auto h = Read<bf::FileHeader>(b, 0);
  h.checksum = bf::Crc32c(b.data() + h.headerBytes, b.size() - h.headerBytes);
  std::memcpy(b.data(), &h, sizeof(h));
  h.headerChecksum = bf::HeaderChecksum(b.data());
  std::memcpy(b.data(), &h, sizeof(h));
}
SND_TEST(ApiGranularPatch, NamedPatchAndLegacyXpchOnlyBank) {
  mini::Project p("d07-legacy");
  p.Minimal();
  p.Add("patches/Beep.json", "patch", "Beep",
        R"("nodes":[{"id":"o","kind":"oscillator","params":{"frequencyHz":800}},{"id":"out","kind":"output"}],
           "connections":[{"from":"o","to":"out"}])");
  for (auto name : {"Beep1","Beep2"}) Event(p, name, R"({"kind":"patch","patch":"Beep"})");
  snd::buildtool::Diagnostics diag;
  SND_REQUIRE(p.Build(&diag));
  auto bytes = mini::ReadBytes(p.out() / "Main.sndb");
  SND_REQUIRE(!bytes.empty());
  {
    Rig r;
    SND_REQUIRE(r.LoadBank("Named.sndb", bytes));
    SND_REQUIRE(Play(r, "Beep2").value);
    r.Step(34);
    SND_CHECK_NEAR(Tail(r, 800), 1.0, 0.02);
  }
  // Reconstruct the former writer's view: the genuine editor XPCH remains,
  // with neither a runtime PTCH nor any programBytes in SYNP.
  const auto h = Read<bf::FileHeader>(bytes, 0);
  bool droppedProgram = false, droppedColumn = false;
  for (unsigned i = 0; i < h.chunkCount; ++i) {
    const auto at = h.chunkTableOffset + i * sizeof(bf::ChunkEntry);
    const auto chunk = Read<bf::ChunkEntry>(bytes, at);
    if (chunk.fourcc == bf::chunk::PTCH) { bytes[at] = 'Z'; droppedProgram = true; }
    if (chunk.fourcc != bf::chunk::SYNP) continue;
    const auto t = Read<bf::TableChunkHeader>(bytes, chunk.offset);
    for (unsigned j = 0; j < t.columnCount; ++j) {
      const auto entry = chunk.offset + sizeof(t) + j * sizeof(bf::ColumnEntry);
      const auto col = Read<bf::ColumnEntry>(bytes, entry);
      if (col.columnId != bf::col::SYNP_programBytes) continue;
      std::fill_n(bytes.begin() + chunk.offset + col.offset, t.rowCount * col.elemBytes, 0);
      droppedColumn = true;
    }
  }
  SND_REQUIRE(droppedProgram && droppedColumn);
  Checksums(bytes);
  Rig r;
  SND_REQUIRE(r.LoadBank("Legacy.sndb", bytes));
  const auto legacy = Play(r, "Beep1");
  SND_REQUIRE(legacy.value);
  r.Step(24);
  SND_CHECK(std::any_of(r.ev.ends.begin(), r.ev.ends.end(), [&](const auto& e) {
    return e.handle == legacy.value && e.detail == SND_END_DETAIL_NONE;
  }));
  SND_CHECK(TailRms(r, 4096) < 0.001);
  SND_REQUIRE(Play(r, "E").value);
  r.Step(24);
  const auto& pcm = r.device.Captured();
  SND_CHECK(std::any_of(pcm.begin(), pcm.end(), [](float x) { return std::abs(x) > 0.05f; }));
}

SND_TEST(ApiGranularPatch, CorruptProgramsRejectedAtBankLoad) {
  const auto bank = Build("corrupt");
  SND_REQUIRE(!bank.empty());
  const auto header = Read<bf::FileHeader>(bank, 0);
  std::size_t payload = 0, chunkTag = 0, sizeCell = 0;
  for (unsigned i = 0; i < header.chunkCount; ++i) {
    const auto entry = header.chunkTableOffset + i * sizeof(bf::ChunkEntry);
    const auto chunk = Read<bf::ChunkEntry>(bank, entry);
    if (chunk.fourcc == bf::chunk::PTCH) { payload = chunk.offset; chunkTag = entry; }
    if (chunk.fourcc != bf::chunk::SYNP) continue;
    const auto table = Read<bf::TableChunkHeader>(bank, chunk.offset);
    for (unsigned j = 0; j < table.columnCount; ++j) {
      const auto col = Read<bf::ColumnEntry>(bank, chunk.offset + sizeof(table) + j * sizeof(bf::ColumnEntry));
      if (col.columnId != bf::col::SYNP_programBytes) continue;
      for (unsigned row = 0; row < table.rowCount; ++row) {
        const auto cell = chunk.offset + col.offset + row * col.elemBytes;
        if (Read<std::uint32_t>(bank, cell)) sizeCell = cell;
      }
    }
  }
  SND_REQUIRE(payload && chunkTag && sizeCell);
  for (unsigned fault = 0; fault < 4; ++fault) {
    auto bad = bank;
    switch (fault) {
      case 0: std::fill_n(bad.begin() + sizeCell, 4, 0xff); break;
      case 1: bad[payload + 12] = 0xfe; break;
      case 2: bad[chunkTag] = 'X'; break;  // missing PTCH
      case 3: std::fill_n(bad.begin() + sizeCell, 4, 0); break;
    }
    Checksums(bad);
    Rig r;
    snd_bank_handle h{};
    SND_REQUIRE_EQ(r.created, SND_OK);
    SND_CHECK(!r.LoadBank("Bad.sndb", bad, &h));
    SND_CHECK(std::any_of(r.ev.banks.begin(), r.ev.banks.end(), [&](auto result) {
      return result.first == h.value && result.second == SND_ERR_CORRUPT;
    }));
  }
}
}  // namespace
