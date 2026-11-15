// buildtool <-> codec and buildtool <-> dsp (stage 1.5 integration, A1-11).
//  - The build encodes with the codec module's encoders (PCMF / PC16 / ADPC);
//    the bank reads back and the codec module decodes within each codec's error.
//  - SEEK rows are codec::encode::BuildSeekTable's.
//  - EFFX.stateBytes / DSPN equal dsp::ComputeDspPoolNeed over the effects as
//    the runtime builds them from the bank; effects are validated against dsp.
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <string>
#include <vector>

#include "pipeline_util.hpp"
#include "snd/bank/bank_manager.hpp"
#include "snd/buildtool/build.hpp"
#include "snd/codec/encode.hpp"
#include "snd/mocks/deterministic_job_system.hpp"
#include "snd/mocks/memory_file_system.hpp"
#include "snd/test/test.hpp"

namespace bt = snd::buildtool;
namespace bk = snd::bank;
namespace bf = snd::bank_format;
using snd::HashId;

namespace {

bt::PcmData TwoTones(std::uint32_t channels, std::uint32_t rate, std::uint64_t frames) {
  bt::PcmData p;
  p.channels = channels;
  p.sampleRate = rate;
  p.frames = frames;
  p.samples.resize(static_cast<std::size_t>(frames * channels));
  for (std::uint64_t f = 0; f < frames; ++f) {
    const double t = static_cast<double>(f) / rate;
    for (std::uint32_t c = 0; c < channels; ++c) {
      p.samples[static_cast<std::size_t>(f * channels + c)] =
          static_cast<float>(0.45 * std::sin(6.283185307179586 * (300.0 + 200.0 * c) * t) +
                             0.2 * std::sin(6.283185307179586 * 2500.0 * t));
    }
  }
  return p;
}

std::vector<float> Channel(const bt::PcmData& p, std::uint32_t c) {
  std::vector<float> v(static_cast<std::size_t>(p.frames));
  for (std::uint64_t f = 0; f < p.frames; ++f) v[static_cast<std::size_t>(f)] = p.samples[f * p.channels + c];
  return v;
}

struct CodecCase {
  const char* codec;
  std::uint32_t fourcc;
};

}  // namespace

SND_TEST(BuildPipeline, EveryCodecRoundTripsThroughTheBank) {
  mini::Project proj("codec-rt");
  proj.Add("banks/Main.json", "bank", "Main");
  proj.Add("buses/Master.json", "bus", "Master", "\"master\": true");
  const bt::PcmData src = TwoTones(2, 48000, 12345);
  proj.Wav("audio/t.wav", src, bt::WavSampleFormat::Float32);
  const CodecCase cases[] = {{"PCMF", SND_CODEC_PCM_F32}, {"PC16", SND_CODEC_PCM_S16}, {"ADPC", SND_CODEC_ADPCM}};
  for (const CodecCase& cc : cases) {
    const std::string name = std::string("a_") + cc.codec;
    proj.Add("assets/" + name + ".json", "asset", name,
             std::string("\"file\": \"audio/t.wav\", \"loadForm\": \"pcm\", \"convert\": {\"default\": {\"codec\": \"") +
                 cc.codec + "\"}}");
    proj.Add("events/E_" + name + ".json", "event", "E_" + name,
             "\"bank\": \"Main\", \"tracks\": [{\"clips\": [{\"asset\": \"" + name + "\"}]}]");
  }
  bt::BuildOptions o = proj.Options();
  o.encoders = &bt::CodecEncoders();  // what sonus-build uses by default
  bt::Diagnostics d;
  SND_REQUIRE_MSG(proj.Build(o, &d), "%s", mini::Dump(d).c_str());
  pipeline::ReadBank bank(mini::ReadBytes(proj.out() / "Main.sndb"));
  SND_REQUIRE_MSG(bank.ok, "%s", bank.error.message);
  for (const CodecCase& cc : cases) {
    const std::uint32_t row = bank.view.FindRow<bk::col::ASST_id>(HashId(std::string("a_") + cc.codec));
    SND_REQUIRE(row != bf::kNone);
    const snd::CodecHeader h = pipeline::AssetHeader(bank.view, row);
    SND_CHECK_EQ(h.codec, cc.fourcc);
    SND_CHECK(snd::codec::ValidateHeader(h) == snd::codec::HeaderError::None);
    SND_CHECK_EQ(h.total_frames, src.frames);
    // ADPCM blocks: the codec default, at most 1024 bytes per channel (bank-format.md 6.5).
    if (cc.fourcc == SND_CODEC_ADPCM) {
      SND_CHECK_EQ(h.block_align, bt::kAdpcmBlockBytesPerChannel * h.channels);
      SND_CHECK(h.block_align <= 1024u * h.channels);
      SND_CHECK(snd::codec::BuiltinCodec(h.codec)->StateBytes(h) <= 32768u);  // default decoderSlotBytes
    }
    // The bytes are the codec module's encoding (byte-identical).
    const bk::ByteSpan data = bank.view.WaveData(row);
    std::vector<std::vector<float>> planar(src.channels);
    std::vector<const float*> ptr;
    for (std::uint32_t c = 0; c < src.channels; ++c) {
      planar[c] = Channel(src, c);
      ptr.push_back(planar[c].data());
    }
    std::vector<std::uint8_t> ref(static_cast<std::size_t>(snd::codec::encode::EncodedBytes(h)));
    std::uint64_t written = 0;
    SND_REQUIRE(snd::codec::encode::Encode(h, ptr.data(), src.frames, ref.data(), ref.size(), &written) ==
                snd::codec::encode::EncodeStatus::Ok);
    SND_REQUIRE_EQ(data.bytes, written);
    SND_CHECK(std::memcmp(data.data, ref.data(), ref.size()) == 0);
    // Decode in odd pieces and compare with the source.
    std::vector<std::vector<float>> out;
    SND_REQUIRE(pipeline::DecodeAll(h, data.data, data.bytes, 777, &out));
    for (std::uint32_t c = 0; c < src.channels; ++c) {
      const std::vector<float> want = Channel(src, c);
      if (cc.fourcc == SND_CODEC_PCM_F32) {
        SND_CHECK(out[c] == want);
      } else if (cc.fourcc == SND_CODEC_PCM_S16) {
        float worst = 0;
        for (std::size_t i = 0; i < want.size(); ++i) worst = std::fmax(worst, std::fabs(out[c][i] - want[i]));
        SND_CHECK_MSG(worst <= 0.5f / 32768.0f + 1e-7f, "PC16 max error %g", static_cast<double>(worst));
      } else {
        const double snr = pipeline::SnrDb(want, out[c]);
        SND_CHECK_MSG(snr > 25.0, "ADPCM channel %u SNR %.2f dB", c, snr);
      }
    }
  }
}

SND_TEST(BuildPipeline, SeekTableIsTheCodecs) {
  mini::Project proj("codec-seek");
  proj.Add("banks/Main.json", "bank", "Main");
  proj.Add("buses/Master.json", "bus", "Master", "\"master\": true");
  proj.Wav("audio/t.wav", TwoTones(1, 44100, 50000));
  proj.Add("assets/a.json", "asset", "a",
           "\"file\": \"audio/t.wav\", \"loadForm\": \"compressed\", \"seekIntervalMs\": 100, "
           "\"convert\": {\"default\": {\"codec\": \"ADPC\"}}");
  proj.Add("events/E.json", "event", "E", "\"bank\": \"Main\", \"tracks\": [{\"clips\": [{\"asset\": \"a\"}]}]");
  bt::BuildOptions o = proj.Options();
  o.encoders = &bt::CodecEncoders();
  bt::Diagnostics d;
  SND_REQUIRE_MSG(proj.Build(o, &d), "%s", mini::Dump(d).c_str());
  pipeline::ReadBank bank(mini::ReadBytes(proj.out() / "Main.sndb"));
  SND_REQUIRE(bank.ok);
  const std::uint32_t row = bank.view.FindRow<bk::col::ASST_id>(HashId("a"));
  const snd::CodecHeader h = pipeline::AssetHeader(bank.view, row);
  const snd::RangeU32 seek = bank.view.SeekRange(row);
  const std::uint32_t n = snd::codec::encode::BuildSeekTable(h, 4410, nullptr, nullptr, 0);
  SND_REQUIRE(n > 3u);
  std::vector<std::uint64_t> frames(n), offsets(n);
  snd::codec::encode::BuildSeekTable(h, 4410, frames.data(), offsets.data(), n);
  SND_REQUIRE_EQ(seek.count, n);
  for (std::uint32_t i = 0; i < n; ++i) {
    SND_CHECK_EQ(bank.view.Column<bk::col::SEEK_frame>()[seek.first + i], frames[i]);
    SND_CHECK_EQ(bank.view.Column<bk::col::SEEK_byteOffset>()[seek.first + i], offsets[i]);
  }
}

namespace {
struct Reserve {
  std::uint64_t bytes = 0;
  std::uint32_t count = 0;
  std::uint32_t calls = 0;
};
bool ReserveHook(void* u, const bf::DspPoolNeed& n) {
  auto* r = static_cast<Reserve*>(u);
  r->bytes += n.instanceBytes;
  r->count += n.instanceCount;
  ++r->calls;
  return true;
}
void ReleaseHook(void* u, const bf::DspPoolNeed& n) {
  auto* r = static_cast<Reserve*>(u);
  r->bytes -= n.instanceBytes;
  r->count -= n.instanceCount;
}

void EffectProject(mini::Project& proj) {
  proj.Add("banks/Main.json", "bank", "Main");
  proj.Add("buses/Master.json", "bus", "Master",
           "\"master\": true, \"effects\": [{\"type\": \"snd.fx.limiter\", \"params\": [-1, 50, 5]}]");
  proj.Add("buses/Wide.json", "bus", "Wide",
           "\"parent\": \"Master\", \"channels\": 6, \"effects\": [{\"type\": \"snd.fx.reverb\"}, "
           "{\"type\": \"snd.fx.echo\", \"params\": [250, 0.4, 1, 0.5, 3000]}]");
  proj.Add("buses/Child.json", "bus", "Child",
           "\"parent\": \"Wide\", \"effects\": [{\"type\": \"snd.fx.compressor\", \"params\": [-24, 3]}, "
           "{\"type\": \"snd.fx.parameq\"}]");
  proj.Wav("audio/a.wav", mini::Tone(1, 48000, 480));
  proj.Add("assets/a.json", "asset", "a", "\"file\": \"audio/a.wav\"");
  proj.Add("events/E.json", "event", "E", "\"bank\": \"Main\", \"bus\": \"Child\", \"tracks\": [{\"clips\": [{\"asset\": \"a\"}]}]");
}

// Channels a bus's effects run at: its own, else the nearest parent's, else 2 (buildtool's rule).
std::uint32_t BusChannels(const bk::BankView& v, std::uint32_t row) {
  for (int depth = 0; row != bf::kNone && depth < 64; ++depth) {
    const std::uint32_t ch = v.Column<bk::col::BUSS_channels>()[row];
    if (ch != 0) return ch;
    row = v.Column<bk::col::BUSS_parent>()[row];
  }
  return 2;
}
}  // namespace

SND_TEST(BuildPipeline, DspNeedEqualsTheRuntimeSizing) {
  for (std::uint32_t rate : {48000u, 96000u}) {
    mini::Project proj("dsp-need-" + std::to_string(rate));
    EffectProject(proj);
    bt::BuildOptions o = proj.Options();
    o.dspSampleRate = rate;
    bt::Diagnostics d;
    SND_REQUIRE_MSG(proj.Build(o, &d), "%s", mini::Dump(d).c_str());
    SND_CHECK_MSG(d.items().empty(), "%s", mini::Dump(d).c_str());  // all standard, all in range
    const std::vector<std::uint8_t> bytes = mini::ReadBytes(proj.out() / "Main.sndb");
    pipeline::ReadBank bank(bytes);
    SND_REQUIRE(bank.ok);
    // What core will do at load: one desc per EFFX row, bus channels, Config rate / block size.
    std::vector<snd::EffectDesc> descs;
    std::vector<std::vector<float>> params;
    params.reserve(64);
    for (std::uint32_t b = 0; b < bank.view.Rows(bf::chunk::BUSS); ++b) {
      const std::vector<snd::EffectDesc> fx =
          pipeline::BusEffectDescs(bank.view, b, BusChannels(bank.view, b), rate, 256, &params);
      descs.insert(descs.end(), fx.begin(), fx.end());
    }
    SND_REQUIRE_EQ(descs.size(), 5u);
    std::uint32_t unknown = 9;
    const bf::DspPoolNeed runtime = snd::dsp::ComputeDspPoolNeed(descs.data(), static_cast<std::uint32_t>(descs.size()),
                                                                 rate, nullptr, 0, &unknown);
    SND_CHECK_EQ(unknown, 0u);
    const bf::DspPoolNeed built = bank.view.DspNeed();
    SND_CHECK_EQ(built.instanceCount, runtime.instanceCount);
    SND_CHECK_EQ(built.instanceBytes, runtime.instanceBytes);
    for (std::uint32_t i = 0; i < descs.size(); ++i) {
      SND_CHECK_EQ(bank.view.Column<bk::col::EFFX_stateBytes>()[i],
                   snd::dsp::EffectInstanceBytes(descs[i], rate, nullptr, 0));
    }
    // The bank manager reserves exactly that (BankManagerDesc.dsp, 3.3).
    snd::mock::MemoryFileSystem fs;
    snd::mock::DeterministicJobSystem jobs;
    fs.AddFile("Main.sndb", bytes);
    Reserve pool;
    bk::BankManagerDesc md;
    md.allocator = &snd::base::DefaultAllocator();
    md.fileSystem = &fs;
    md.ioMode = bk::BankIoMode::DirectFileSystem;
    md.jobs = &jobs;
    md.dsp.user = &pool;
    md.dsp.reserve = &ReserveHook;
    md.dsp.release = &ReleaseHook;
    bk::BankManager* m = nullptr;
    SND_REQUIRE_EQ(bk::BankManager::Create(md, &m), SND_OK);
    const snd::HandleRef h = m->RequestLoad("Main.sndb");
    for (int i = 0; i < 16; ++i) {
      m->Update();
      jobs.RunAll();
    }
    m->Update();
    SND_CHECK_EQ(m->State(h), SND_BANK_STATE_LOADING);  // registration queued, not applied
    SND_CHECK_EQ(pool.calls, 1u);
    SND_CHECK_EQ(pool.bytes, runtime.instanceBytes);
    SND_CHECK_EQ(pool.count, runtime.instanceCount);
    // The effects initialise in exactly that much pool memory.
    std::vector<std::uint64_t> poolMem(static_cast<std::size_t>(runtime.instanceBytes / 8 + 8));
    auto* at = reinterpret_cast<std::uint8_t*>(poolMem.data());
    while (reinterpret_cast<std::uintptr_t>(at) % 64 != 0) ++at;
    for (const snd::EffectDesc& dsc : descs) {
      const std::uint32_t b = snd::dsp::EffectInstanceBytes(dsc, rate, nullptr, 0);
      SND_CHECK(snd::dsp::EffectInit(at, b, dsc, rate));
      at += b;
    }
    bk::BankManager::Destroy(m);
  }
}

SND_TEST(BuildPipeline, EffectsAreValidatedAgainstDsp) {
  // Unknown type: warning, not sized, not in DSPN.
  {
    mini::Project proj("fx-unknown");
    proj.Minimal();
    proj.Add("buses/B.json", "bus", "B", "\"bank\": \"Main\", \"parent\": \"Master\", \"effects\": [{\"type\": \"my.plugin\"}]");
    bt::Diagnostics d;
    SND_REQUIRE(proj.Build(&d));
    SND_CHECK(mini::HasDiag(d, bt::Severity::Warning, "unknown effect type 'my.plugin'"));
    pipeline::ReadBank bank(mini::ReadBytes(proj.out() / "Main.sndb"));
    SND_REQUIRE(bank.ok);
    SND_CHECK_EQ(bank.view.DspNeed().instanceCount, 0u);
    // ... unless the plugin is given to the build: sized through its table (rate in the desc, D-48).
    struct Plugin {
      static std::uint32_t SND_CALL Bytes(void*, const snd_effect_desc* dsc) { return 100u * dsc->channels + dsc->sample_rate / 1000u; }
      static void SND_CALL Init(void*, void*, const snd_effect_desc*, std::uint32_t) {}
      static void SND_CALL Process(void*, void*, const float* const*, float* const*, std::uint32_t, std::uint32_t,
                                   const snd_param_block*) {}
      static std::int32_t SND_CALL Tail(void*, const void*) { return 1; }
    };
    bt::BuildOptions o = proj.Options();
    o.pluginEffects.push_back(snd_dsp_effect{nullptr, HashId("my.plugin"), 0u, &Plugin::Bytes, &Plugin::Init,
                                             &Plugin::Process, &Plugin::Tail});
    bt::Diagnostics d2;
    SND_REQUIRE(proj.Build(o, &d2));
    SND_CHECK(!mini::HasDiag(d2, bt::Severity::Warning, "unknown effect type"));
    pipeline::ReadBank b2(mini::ReadBytes(proj.out() / "Main.sndb"));
    SND_REQUIRE(b2.ok);
    SND_CHECK_EQ(b2.view.DspNeed().instanceCount, 1u);
    SND_CHECK_EQ(b2.view.DspNeed().instanceBytes, 256u);  // 2 ch * 100 + 48 = 248 -> 256
  }
  // Too many parameters: error, nothing written.
  {
    mini::Project proj("fx-params");
    proj.Minimal();
    proj.Add("buses/B.json", "bus", "B",
             "\"bank\": \"Main\", \"parent\": \"Master\", \"effects\": [{\"type\": \"snd.fx.lowpass\", \"params\": [1000, 1, 3]}]");
    bt::Diagnostics d;
    SND_CHECK(!proj.Build(&d));
    SND_CHECK(mini::HasDiag(d, bt::Severity::Error, "takes 2 parameters, 3 given"));
  }
  // Value outside the range: warning naming the parameter (the runtime clamps); --werror fails.
  {
    mini::Project proj("fx-range");
    proj.Minimal();
    proj.Add("buses/B.json", "bus", "B",
             "\"bank\": \"Main\", \"parent\": \"Master\", \"effects\": [{\"type\": \"snd.fx.lowpass\", \"params\": [50000, 1]}]");
    bt::Diagnostics d;
    SND_REQUIRE(proj.Build(&d));
    SND_CHECK(mini::HasDiag(d, bt::Severity::Warning, "parameter 0 (CutoffHz): 50000 is outside 10..22000"));
    bt::BuildOptions o = proj.Options();
    o.warningsAsErrors = true;
    bt::Diagnostics d2;
    SND_CHECK(!proj.Build(o, &d2));
  }
}

// bank-format.md 6.4 lists the standard effect ids: they are dsp's (A1-6).
SND_TEST(BuildPipeline, BankFormatEffectTableMatchesDsp) {
  const std::string md = mini::ReadText(std::string(SND_SPEC_DIR) + "/interface/bank-format.md");
  SND_REQUIRE(!md.empty());
  const std::uint32_t kinds = static_cast<std::uint32_t>(snd::dsp::EffectKind::Count);
  std::uint32_t found = 0;
  for (std::uint32_t k = 0; k < kinds; ++k) {
    const snd::dsp::EffectInfo& info = snd::dsp::GetEffectInfo(static_cast<snd::dsp::EffectKind>(k));
    char row[96];
    std::snprintf(row, sizeof(row), "| `%s` | `0x%08X` |", info.name, info.typeId);
    const std::size_t at = md.find(row);
    SND_CHECK_MSG(at != std::string::npos, "bank-format.md 6.4 lacks %s", row);
    if (at == std::string::npos) continue;
    ++found;
    // ... and the parameter count column.
    const std::size_t end = md.find('\n', at);
    const std::string line = md.substr(at, end - at);
    SND_CHECK_MSG(line.find("| " + std::to_string(info.paramCount) + " |") != std::string::npos, "%s: %u params",
                  info.name, info.paramCount);
  }
  SND_CHECK_EQ(found, kinds);
}

// D-54: the bank manager reserves the RUNTIME's need (hook = dsp::ComputeDspPoolNeed
// with the runtime Config), not the bank's DSPN; a difference is not an error.
namespace {
bool ComputeHook(void*, const snd_effect_desc* descs, std::uint32_t count, std::uint32_t rate, bf::DspPoolNeed* out) {
  std::uint32_t unknown = 0;
  *out = snd::dsp::ComputeDspPoolNeed(descs, count, rate, nullptr, 0, &unknown);
  return true;
}
}  // namespace

SND_TEST(BuildPipeline, BankManagerReservesTheRuntimeNeed) {
  mini::Project proj("dsp-runtime");
  EffectProject(proj);
  // Master is 0 channels up to the root: the runtime's output channel count decides.
  bt::Diagnostics d;
  SND_REQUIRE_MSG(proj.Build(proj.Options(), &d), "%s", mini::Dump(d).c_str());  // built for 48 kHz
  const std::vector<std::uint8_t> bytes = mini::ReadBytes(proj.out() / "Main.sndb");
  pipeline::ReadBank bank(bytes);
  SND_REQUIRE(bank.ok);
  struct Case {
    std::uint32_t rate, block, outputChannels;
  };
  for (const Case c : {Case{48000, 256, 2}, Case{96000, 512, 2}, Case{48000, 256, 8}}) {
    // Expected: the descs as the runtime builds them.
    std::vector<snd::EffectDesc> descs;
    std::vector<std::vector<float>> params;
    params.reserve(64);
    for (std::uint32_t b = 0; b < bank.view.Rows(bf::chunk::BUSS); ++b) {
      std::uint32_t ch = BusChannels(bank.view, b);
      // BusChannels() falls back to 2 (the build's rule); the runtime uses outputChannels.
      std::uint32_t at = b, own = 0;
      for (int k = 0; k < 64 && at != bf::kNone; ++k) {
        own = bank.view.Column<bk::col::BUSS_channels>()[at];
        if (own != 0) break;
        at = bank.view.Column<bk::col::BUSS_parent>()[at];
      }
      if (own == 0) ch = c.outputChannels;
      const std::vector<snd::EffectDesc> fx = pipeline::BusEffectDescs(bank.view, b, ch, c.rate, c.block, &params);
      descs.insert(descs.end(), fx.begin(), fx.end());
    }
    const bf::DspPoolNeed want = snd::dsp::ComputeDspPoolNeed(descs.data(), static_cast<std::uint32_t>(descs.size()),
                                                              c.rate, nullptr, 0);
    snd::mock::MemoryFileSystem fs;
    snd::mock::DeterministicJobSystem jobs;
    fs.AddFile("Main.sndb", bytes);
    Reserve pool;
    bk::BankManagerDesc md;
    md.allocator = &snd::base::DefaultAllocator();
    md.fileSystem = &fs;
    md.ioMode = bk::BankIoMode::DirectFileSystem;
    md.jobs = &jobs;
    md.dsp.user = &pool;
    md.dsp.reserve = &ReserveHook;
    md.dsp.release = &ReleaseHook;
    md.dsp.compute = &ComputeHook;
    md.sampleRate = c.rate;
    md.blockSize = c.block;
    md.outputChannels = c.outputChannels;
    bk::BankManager* m = nullptr;
    SND_REQUIRE_EQ(bk::BankManager::Create(md, &m), SND_OK);
    const snd::HandleRef h = m->RequestLoad("Main.sndb");
    for (int i = 0; i < 16; ++i) {
      m->Update();
      jobs.RunAll();
    }
    m->Update();
    SND_CHECK_MSG(m->Result(h) == SND_OK, "%s", m->FailureReason(h));
    SND_CHECK_EQ(pool.bytes, want.instanceBytes);
    SND_CHECK_EQ(pool.count, want.instanceCount);
    const bk::PreparedBank* pb = m->Prepared(h);
    SND_REQUIRE(pb != nullptr);
    SND_CHECK_EQ(pb->builtDspNeed.instanceBytes, bank.view.DspNeed().instanceBytes);  // kept for reference
    if (c.rate != 48000 || c.outputChannels != 2) SND_CHECK(pb->dspNeed.instanceBytes != pb->builtDspNeed.instanceBytes);
    bk::BankManager::Destroy(m);
  }
}
