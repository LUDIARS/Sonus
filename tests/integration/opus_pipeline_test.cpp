// Opus through the whole build pipeline (CONTRACTS.md D-56..D-60,
// bank-format.md §6.5): buildtool encodes Opus assets -> bank parses the bank
// (ParseBank, the runtime's validation) -> codec decodes from the bank's bytes
// and its setup data (ASST.extra*) -> analytic checks (tone levels, channel
// identity, sample-exact seeks through the SEEK rows, stream files and
// preload). With SND_CODEC_OPUS=OFF the build must reject OPUS assets.
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "pipeline_util.hpp"
#include "snd/buildtool/build.hpp"
#include "snd/codec/codec.hpp"
#include "snd/codec/encode.hpp"
#include "snd/test/test.hpp"

namespace bt = snd::buildtool;
namespace bk = snd::bank;
namespace bf = snd::bank_format;
using snd::HashId;

namespace {

constexpr double kTwoPi = 6.283185307179586;

bt::PcmData Tones(std::uint32_t rate, std::uint64_t frames, const std::vector<double>& hz, double amp) {
  bt::PcmData p;
  p.channels = static_cast<std::uint32_t>(hz.size());
  p.sampleRate = rate;
  p.frames = frames;
  p.samples.resize(static_cast<std::size_t>(frames * p.channels));
  for (std::uint64_t f = 0; f < frames; ++f) {
    for (std::uint32_t c = 0; c < p.channels; ++c) {
      p.samples[static_cast<std::size_t>(f * p.channels + c)] =
          static_cast<float>(amp * std::sin(kTwoPi * hz[c] * static_cast<double>(f) / rate));
    }
  }
  return p;
}

// Amplitude of the integer frequency `hz` over one second at 48 kHz from `start`.
double ToneAmplitude(const std::vector<float>& x, std::size_t start, double hz) {
  double re = 0, im = 0;
  for (std::size_t i = 0; i < 48000; ++i) {
    const double ph = kTwoPi * hz * static_cast<double>(i) / 48000.0;
    re += x[start + i] * std::cos(ph);
    im -= x[start + i] * std::sin(ph);
  }
  return 2.0 * std::sqrt(re * re + im * im) / 48000.0;
}

double Db(double r) { return 20.0 * std::log10(r); }

// Header of an ASST row including the Opus setup data (ASST.extraOffset / extraBytes in WAVE).
snd::CodecHeader FullHeader(const bk::BankView& v, std::uint32_t row) {
  snd::CodecHeader h = pipeline::AssetHeader(v, row);
  const bk::ByteSpan extra = v.AssetExtra(row);
  h.extra = extra.data;
  h.extra_bytes = static_cast<std::uint32_t>(extra.bytes);
  return h;
}

std::uint32_t AssetRow(const pipeline::ReadBank& b, const char* name) {
  return b.view.FindRow<bk::col::ASST_id>(HashId(name));
}

void BaseProject(mini::Project& proj) {
  proj.Add("banks/Main.json", "bank", "Main");
  proj.Add("buses/Master.json", "bus", "Master", "\"master\": true");
}

void AddEvent(mini::Project& proj, const std::string& asset) {
  proj.Add("events/E_" + asset + ".json", "event", "E_" + asset,
           "\"bank\": \"Main\", \"tracks\": [{\"clips\": [{\"asset\": \"" + asset + "\"}]}]");
}

}  // namespace

SND_TEST(OpusPipeline, BuildWithoutOpusRejectsOpusAssets) {
  if (snd::codec::OpusAvailable()) return;  // SND_CODEC_OPUS=OFF only
  mini::Project proj("opus-off");
  BaseProject(proj);
  proj.Wav("audio/t.wav", Tones(48000, 4800, {1000}, 0.5));
  proj.Add("assets/a.json", "asset", "a", "\"file\": \"audio/t.wav\", \"convert\": {\"default\": {\"codec\": \"OPUS\"}}");
  AddEvent(proj, "a");
  bt::BuildOptions o = proj.Options();
  o.encoders = &bt::CodecEncoders();
  bt::Diagnostics d;
  SND_CHECK(!proj.Build(o, &d));
  SND_CHECK_MSG(mini::HasDiag(d, bt::Severity::Error, "no encoder for codec 'OPUS'"), "%s", mini::Dump(d).c_str());
  SND_CHECK(bt::CodecEncoders().Find(SND_CODEC_OPUS) == nullptr);
}

// Resident (compressed) assets: mono from a 44.1 kHz WAV (converted to 48 kHz),
// stereo with a different tone per channel, 6 channels, and per-asset settings.
SND_TEST(OpusPipeline, ResidentAssetsDecodeFromTheBank) {
  if (!snd::codec::OpusAvailable()) return;
  mini::Project proj("opus-resident");
  BaseProject(proj);
  proj.Wav("audio/mono441.wav", Tones(44100, 66150, {1000}, 0.5), bt::WavSampleFormat::Float32);  // 1.5 s
  proj.Wav("audio/stereo.wav", Tones(48000, 72000, {440, 1000}, 0.4), bt::WavSampleFormat::Float32);
  proj.Wav("audio/six.wav", Tones(48000, 60000, {300, 700, 1100, 60, 1900, 2300}, 0.3), bt::WavSampleFormat::Float32);
  proj.Add("assets/mono.json", "asset", "mono",
           "\"file\": \"audio/mono441.wav\", \"loadForm\": \"compressed\", "
           "\"convert\": {\"default\": {\"codec\": \"OPUS\"}}");
  proj.Add("assets/stereo.json", "asset", "stereo",
           "\"file\": \"audio/stereo.wav\", \"loadForm\": \"compressed\", "
           "\"convert\": {\"default\": {\"codec\": \"OPUS\"}}");
  proj.Add("assets/stereoLow.json", "asset", "stereoLow",
           "\"file\": \"audio/stereo.wav\", \"loadForm\": \"compressed\", "
           "\"convert\": {\"default\": {\"codec\": \"OPUS\", \"opus\": {\"bitrate\": 48000, \"complexity\": 5, "
           "\"vbr\": \"cbr\", \"signal\": \"music\"}}}");
  proj.Add("assets/six.json", "asset", "six",
           "\"file\": \"audio/six.wav\", \"loadForm\": \"compressed\", "
           "\"convert\": {\"default\": {\"codec\": \"OPUS\"}}");
  for (const char* a : {"mono", "stereo", "stereoLow", "six"}) AddEvent(proj, a);
  bt::BuildOptions o = proj.Options();
  o.encoders = &bt::CodecEncoders();  // sonus-build's default
  bt::Diagnostics d;
  SND_REQUIRE_MSG(proj.Build(o, &d), "%s", mini::Dump(d).c_str());
  // D-58: the 6-channel asset needs more than the default 64 KiB slot (warned at
  // build time); the 1-2 channel assets fit and are not warned about.
  SND_CHECK_MSG(mini::HasDiag(d, bt::Severity::Warning, "asset 'six': Opus with 6 channels needs a decoder slot of 135552 bytes"),
                "%s", mini::Dump(d).c_str());
  for (const char* quiet : {"asset 'mono'", "asset 'stereo'", "asset 'stereoLow'"}) {
    SND_CHECK_MSG(!mini::HasDiag(d, bt::Severity::Warning, quiet), "%s", mini::Dump(d).c_str());
  }
  pipeline::ReadBank bank(mini::ReadBytes(proj.out() / "Main.sndb"));
  SND_REQUIRE_MSG(bank.ok, "%s", bank.error.message);

  struct Expect {
    const char* name;
    std::vector<double> hz;
    double amp;
    std::uint64_t frames;
  };
  const Expect cases[] = {{"mono", {1000}, 0.5, 72000},  // 66150 frames at 44.1 kHz -> 72000 at 48 kHz
                          {"stereo", {440, 1000}, 0.4, 72000},
                          {"stereoLow", {440, 1000}, 0.4, 72000},
                          {"six", {300, 700, 1100, 60, 1900, 2300}, 0.3, 60000}};
  std::uint64_t stereoBytes = 0, stereoLowBytes = 0;
  for (const Expect& e : cases) {
    const std::uint32_t row = AssetRow(bank, e.name);
    SND_REQUIRE(row != bf::kNone);
    const snd::CodecHeader h = FullHeader(bank.view, row);
    SND_CHECK_EQ(h.codec, SND_CODEC_OPUS);
    SND_CHECK_EQ(h.sample_rate, 48000u);
    SND_CHECK_EQ(h.channels, static_cast<std::uint32_t>(e.hz.size()));
    SND_CHECK_EQ(h.frames_per_packet, 960u);
    SND_CHECK_EQ(h.block_align, 0u);
    SND_CHECK_EQ(h.pre_skip, 312u);
    SND_CHECK_EQ(h.pre_roll, 3840u);
    SND_CHECK_EQ(h.total_frames, e.frames);
    SND_CHECK_EQ(h.extra_bytes, 4u + h.channels);
    SND_REQUIRE(snd::codec::ValidateHeader(h) == snd::codec::HeaderError::None);
    // Resident data starts 64-byte aligned in WAVE (bank-format.md 6.2), extra too.
    SND_CHECK_EQ(bank.view.Column<bk::col::ASST_dataOffset>()[row] % 64u, 0u);
    SND_CHECK_EQ(bank.view.Column<bk::col::ASST_extraOffset>()[row] % 64u, 0u);
    const bk::ByteSpan data = bank.view.WaveData(row);
    if (std::string(e.name) == "stereo") stereoBytes = data.bytes;
    if (std::string(e.name) == "stereoLow") stereoLowBytes = data.bytes;
    std::vector<std::vector<float>> out;
    SND_REQUIRE_MSG(pipeline::DecodeAll(h, data.data, data.bytes, 777, &out), "%s: decode failed", e.name);
    for (std::uint32_t c = 0; c < h.channels; ++c) {
      const double own = ToneAmplitude(out[c], 6000, e.hz[c]);
      const double ownDb = Db(own / e.amp);
      SND_CHECK_MSG(std::fabs(ownDb) <= 0.5, "%s ch %u: %.0f Hz at %+.2f dB", e.name, c, e.hz[c], ownDb);
      for (std::uint32_t k = 0; k < h.channels; ++k) {
        if (k == c) continue;
        const double other = Db(ToneAmplitude(out[c], 6000, e.hz[k]) / e.amp);
        const double limit = (h.channels >= 6 && c == 3) ? -30.0 : -40.0;  // LFE stream: see codec_opus_test
        SND_CHECK_MSG(other < limit, "%s ch %u: %.1f dB of channel %u's tone", e.name, c, other, k);
      }
    }
    // The decoder state fits the slot sizes of D-58.
    const std::uint32_t sb = snd::codec::BuiltinCodec(SND_CODEC_OPUS)->StateBytes(h);
    if (h.channels <= 2) SND_CHECK(sb <= 64u * 1024u);
    SND_CHECK(sb <= 192u * 1024u);
  }
  // Settings reach the encoder: 48 kbit/s CBR is smaller than the default 128 kbit/s VBR.
  SND_CHECK_MSG(stereoLowBytes * 2 < stereoBytes, "48k: %llu bytes, default: %llu bytes",
                static_cast<unsigned long long>(stereoLowBytes), static_cast<unsigned long long>(stereoBytes));
  // CBR: 48 kbit/s * 1.5 s / 8 = 9000 bytes of payload (+2 bytes prefix per 20 ms packet).
  const double perSecond = static_cast<double>(stereoLowBytes) / ((72000.0 + 312.0) / 48000.0);
  SND_CHECK_MSG(std::fabs(perSecond - (6000.0 + 100.0)) < 300.0, "CBR bytes per second %.0f", perSecond);
}

// Stream form: the stream file holds the packets, SEEK rows are
// BuildOpusSeekTable's, the preload is a prefix cut at a seek point, and seeks
// through the bank's SEEK rows are sample-exact.
SND_TEST(OpusPipeline, StreamSeekAndPreload) {
  if (!snd::codec::OpusAvailable()) return;
  mini::Project proj("opus-stream");
  BaseProject(proj);
  const bt::PcmData src = Tones(48000, 144000, {523, 1250}, 0.4);  // 3 s
  proj.Wav("audio/music.wav", src, bt::WavSampleFormat::Float32);
  proj.Add("assets/music.json", "asset", "music",
           "\"file\": \"audio/music.wav\", \"loadForm\": \"stream\", \"preloadMs\": 150, \"seekIntervalMs\": 100, "
           "\"convert\": {\"default\": {\"codec\": \"OPUS\"}}");
  AddEvent(proj, "music");
  bt::BuildOptions o = proj.Options();
  o.encoders = &bt::CodecEncoders();
  bt::Diagnostics d;
  bt::BuildResult res;
  SND_REQUIRE_MSG(proj.Build(o, &d, &res), "%s", mini::Dump(d).c_str());
  pipeline::ReadBank bank(mini::ReadBytes(proj.out() / "Main.sndb"));
  SND_REQUIRE_MSG(bank.ok, "%s", bank.error.message);
  const std::uint32_t row = AssetRow(bank, "music");
  SND_REQUIRE(row != bf::kNone);
  const snd::CodecHeader h = FullHeader(bank.view, row);
  SND_REQUIRE(snd::codec::ValidateHeader(h) == snd::codec::HeaderError::None);
  SND_CHECK_EQ(bank.view.Column<bk::col::ASST_loadForm>()[row], 3u);
  // Stream file.
  SND_REQUIRE_EQ(res.streams.size(), std::size_t{1});
  const std::vector<std::uint8_t> file = mini::ReadBytes(proj.out() / res.streams[0]);
  SND_REQUIRE(file.size() >= sizeof(bf::StreamFileHeader));
  bf::StreamFileHeader sh{};
  std::memcpy(&sh, file.data(), sizeof(sh));
  SND_CHECK_EQ(sh.codec, SND_CODEC_OPUS);
  SND_CHECK_EQ(sh.channels, 2u);
  SND_CHECK_EQ(sh.sampleRate, 48000u);
  SND_CHECK_EQ(sh.totalFrames, src.frames);
  SND_REQUIRE(sh.dataOffset + sh.dataBytes <= file.size());
  const std::uint8_t* data = file.data() + sh.dataOffset;
  const std::uint64_t bytes = sh.dataBytes;
  // SEEK rows.
  const snd::RangeU32 seek = bank.view.SeekRange(row);
  const std::uint32_t n = snd::codec::encode::BuildOpusSeekTable(h, data, bytes, 4800, nullptr, nullptr, 0);
  SND_REQUIRE(n > 20u);
  std::vector<std::uint64_t> frames(n), offsets(n);
  snd::codec::encode::BuildOpusSeekTable(h, data, bytes, 4800, frames.data(), offsets.data(), n);
  SND_REQUIRE_EQ(seek.count, n);
  for (std::uint32_t i = 0; i < n; ++i) {
    SND_CHECK_EQ(bank.view.Column<bk::col::SEEK_frame>()[seek.first + i], frames[i]);
    SND_CHECK_EQ(bank.view.Column<bk::col::SEEK_byteOffset>()[seek.first + i], offsets[i]);
    SND_CHECK(i == 0 || (frames[i] + h.pre_skip) % 960u == 0u);
  }
  // Preload: the first seek point at or after 150 ms, as a byte prefix of the stream data.
  const bk::ByteSpan pre = bank.view.Preload(row);
  const std::uint32_t preFrames = bank.view.Column<bk::col::ASST_preloadFrames>()[row];
  SND_REQUIRE(pre.data != nullptr);
  SND_CHECK(preFrames >= 7200u);
  bool atSeekPoint = false;
  for (std::uint32_t i = 0; i < n; ++i) atSeekPoint = atSeekPoint || (frames[i] == preFrames && offsets[i] == pre.bytes);
  SND_CHECK(atSeekPoint);
  SND_REQUIRE(pre.bytes <= bytes);
  SND_CHECK(std::memcmp(pre.data, data, static_cast<std::size_t>(pre.bytes)) == 0);
  // Decoding the preload alone yields exactly preloadFrames frames.
  {
    snd::CodecHeader hp = h;
    std::vector<std::vector<float>> pout;
    (void)pipeline::DecodeAll(hp, pre.data, pre.bytes, 1000, &pout);  // false: shorter than total_frames
    SND_CHECK_EQ(static_cast<std::uint64_t>(pout[0].size()), static_cast<std::uint64_t>(preFrames));
  }
  // Continuous decode: tones per channel.
  std::vector<std::vector<float>> ref;
  SND_REQUIRE(pipeline::DecodeAll(h, data, bytes, 2048, &ref));
  SND_CHECK(std::fabs(Db(ToneAmplitude(ref[0], 9000, 523) / 0.4)) <= 0.5);
  SND_CHECK(std::fabs(Db(ToneAmplitude(ref[1], 9000, 1250) / 0.4)) <= 0.5);
  // Seeks through the bank's SEEK rows with the bank's pre-roll: sample-exact.
  snd::ICodec* c = snd::codec::BuiltinCodec(SND_CODEC_OPUS);
  std::vector<std::uint64_t> stateMem(c->StateBytes(h) / 8 + 16);
  auto* st = reinterpret_cast<std::uint8_t*>(stateMem.data());
  while (reinterpret_cast<std::uintptr_t>(st) % 64 != 0) ++st;
  c->Init(st, h);
  std::vector<std::uint64_t> sf(seek.count), so(seek.count);  // the bank's SEEK rows of the asset
  for (std::uint32_t i = 0; i < seek.count; ++i) {
    sf[i] = bank.view.Column<bk::col::SEEK_frame>()[seek.first + i];
    so[i] = bank.view.Column<bk::col::SEEK_byteOffset>()[seek.first + i];
  }
  for (std::uint64_t target : {0ull, 5000ull, 47999ull, 48000ull, 100000ull, 143999ull}) {
    const std::uint32_t e = snd::codec::FindSeekPoint(sf.data(), seek.count, target, h.pre_roll, h.total_frames);
    SND_REQUIRE(e != snd::codec::kNoSeekPoint);
    c->Seek(st, sf[e]);
    std::vector<float> l(1024), r(1024);
    float* dst[2] = {l.data(), r.data()};
    std::uint64_t pos = sf[e], off = so[e];
    std::uint64_t compared = 0;
    bool same = true;
    while (pos < h.total_frames) {
      const std::uint32_t len = static_cast<std::uint32_t>(std::min<std::uint64_t>(3000, bytes - off));
      std::uint32_t got = c->Decode(st, len != 0 ? data + off : nullptr, len, dst, 1024);
      off += len;
      for (;;) {
        SND_REQUIRE(got != snd::kDecodeError);
        for (std::uint32_t i = 0; i < got; ++i, ++pos) {
          if (pos >= target) {
            same = same && l[i] == ref[0][static_cast<std::size_t>(pos)] && r[i] == ref[1][static_cast<std::size_t>(pos)];
            ++compared;
          }
        }
        if (got < 1024u) break;
        got = c->Decode(st, nullptr, 0, dst, 1024);
      }
      if (len == 0) break;
    }
    SND_CHECK_EQ(pos, h.total_frames);
    SND_CHECK_EQ(compared, h.total_frames - target);
    SND_CHECK_MSG(same, "seek to %llu (from seek point %llu) differs from the continuous decode",
                  static_cast<unsigned long long>(target), static_cast<unsigned long long>(sf[e]));
  }
}

// Differential build: the Opus settings and the encoder version are part of the
// cache key; a sampleRate other than 48000 is a warning (Opus is always 48 kHz).
SND_TEST(OpusPipeline, CacheKeyAndRateWarning) {
  if (!snd::codec::OpusAvailable()) return;
  mini::Project proj("opus-cache");
  BaseProject(proj);
  proj.Wav("audio/t.wav", Tones(48000, 24000, {800}, 0.4));
  auto asset = [&](const std::string& opus) {
    proj.Add("assets/a.json", "asset", "a",
             "\"file\": \"audio/t.wav\", \"loadForm\": \"compressed\", "
             "\"convert\": {\"default\": {\"codec\": \"OPUS\", \"sampleRate\": 44100" + opus + "}}");
  };
  asset("");
  AddEvent(proj, "a");
  bt::BuildOptions o = proj.Options();
  o.encoders = &bt::CodecEncoders();
  o.cacheDir = (proj.root() / "cache").string();
  bt::Diagnostics d1;
  bt::BuildResult r1;
  SND_REQUIRE_MSG(proj.Build(o, &d1, &r1), "%s", mini::Dump(d1).c_str());
  SND_CHECK_MSG(mini::HasDiag(d1, bt::Severity::Warning, "Opus is always encoded at 48000 Hz"), "%s",
                mini::Dump(d1).c_str());
  SND_CHECK_EQ(r1.assetsEncoded, 1u);
  bt::Diagnostics d2;
  bt::BuildResult r2;
  SND_REQUIRE(proj.Build(o, &d2, &r2));
  SND_CHECK_EQ(r2.assetsFromCache, 1u);
  SND_CHECK_EQ(r2.assetsEncoded, 0u);
  asset(", \"opus\": {\"bitrate\": 24000}");  // same file name: replaces the asset
  bt::Diagnostics d3;
  bt::BuildResult r3;
  SND_REQUIRE_MSG(proj.Build(o, &d3, &r3), "%s", mini::Dump(d3).c_str());
  SND_CHECK_EQ(r3.assetsEncoded, 1u);
  // Out-of-range settings are build errors.
  asset(", \"opus\": {\"complexity\": 11}");
  bt::Diagnostics d4;
  SND_CHECK(!proj.Build(o, &d4));
  SND_CHECK_MSG(mini::HasDiag(d4, bt::Severity::Error, "opus.complexity"), "%s", mini::Dump(d4).c_str());
  asset(", \"opus\": {\"vbr\": \"sometimes\"}");
  bt::Diagnostics d5;
  SND_CHECK(!proj.Build(o, &d5));
}

// D-57: Opus has no resident-PCM form; the default loadForm "pcm" is a build error naming the choices.
SND_TEST(OpusPipeline, PcmLoadFormIsABuildError) {
  if (!snd::codec::OpusAvailable()) return;
  mini::Project proj("opus-pcmform");
  BaseProject(proj);
  proj.Wav("audio/t.wav", Tones(48000, 4800, {1000}, 0.5));
  proj.Add("assets/a.json", "asset", "a", "\"file\": \"audio/t.wav\", \"convert\": {\"default\": {\"codec\": \"OPUS\"}}");
  AddEvent(proj, "a");
  bt::BuildOptions o = proj.Options();
  o.encoders = &bt::CodecEncoders();
  bt::Diagnostics d;
  SND_CHECK(!proj.Build(o, &d));
  SND_CHECK_MSG(mini::HasDiag(d, bt::Severity::Error, "codec OPUS cannot use loadForm \"pcm\""), "%s",
                mini::Dump(d).c_str());
  SND_CHECK(mini::HasDiag(d, bt::Severity::Error, "\"compressed\""));
}
