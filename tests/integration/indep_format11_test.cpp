// Independent tests: buildtool output vs bank-format.md §6.3 / §6.5 and the dsp
// sizing (§6.2 DSPN, D-48), checked against the SPEC with independent
// reference code (not against codec's own encoder / seek-table functions):
//  - PCMF bytes = the float32 LE bits of the input, PC16 = round-to-nearest
//    clamp(x * 32768) as int16 LE, interleaved; header fields of §6.5.
//  - ADPC: block geometry (blockAlign <= 1024 x ch, 4ch(1+k), frames per block
//    formula, padded last block), block headers {s16, step <= 88, 0}, and a
//    reference WAV/IMA decoder written here decodes the bank bytes to exactly
//    what codec's built-in decoder returns; quality against the source.
//  - SEEK rows: {0,0} first, packet boundaries, byteOffset = packet * blockAlign,
//    and decoding from each row (codec Seek) equals the continuous decode.
//  - Stream files (§6.3): header, sector padding, PREL = data prefix at a packet boundary.
//  - DSPN = sum of roundup64 of dsp sizing over the bank's effects with bus
//    channels inherited from parents, [init] parameters out of range, odd
//    rate / block sizes; desc.sample_rate other than 0 / the rate fails (D-48).
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "pipeline_util.hpp"
#include "snd/buildtool/build.hpp"
#include "snd/codec/codec.hpp"
#include "snd/format/bank_format.hpp"
#include "snd/test/test.hpp"

namespace bt = snd::buildtool;
namespace bk = snd::bank;
namespace bf = snd::bank_format;
using snd::HashId;

namespace {

constexpr double kTwoPi = 6.283185307179586;

std::uint16_t Le16(const std::uint8_t* p) { return static_cast<std::uint16_t>(p[0] | (p[1] << 8)); }
std::uint32_t Le32(const std::uint8_t* p) {
  return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) |
         (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[3]) << 24);
}
std::uint64_t Le64(const std::uint8_t* p) { return Le32(p) | (static_cast<std::uint64_t>(Le32(p + 4)) << 32); }

// --- Reference IMA ADPCM (WAV/IMA block layout, bank-format.md §6.5) ------------------------
const int kStep[89] = {7,     8,     9,     10,    11,    12,    13,    14,    16,    17,    19,    21,    23,
                       25,    28,    31,    34,    37,    41,    45,    50,    55,    60,    66,    73,    80,
                       88,    97,    107,   118,   130,   143,   157,   173,   190,   209,   230,   253,   279,
                       307,   337,   371,   408,   449,   494,   544,   598,   658,   724,   796,   876,   963,
                       1060,  1166,  1282,  1411,  1552,  1707,  1878,  2066,  2272,  2499,  2749,  3024,  3327,
                       3660,  4026,  4428,  4871,  5358,  5894,  6484,  7132,  7845,  8630,  9493,  10442, 11487,
                       12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767};
const int kIndex[16] = {-1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8};

struct ImaChannel {
  int pred;
  int index;
  int Next(int nibble) {
    const int step = kStep[index];
    int diff = step >> 3;
    if (nibble & 4) diff += step;
    if (nibble & 2) diff += step >> 1;
    if (nibble & 1) diff += step >> 2;
    pred += (nibble & 8) ? -diff : diff;
    if (pred > 32767) pred = 32767;
    if (pred < -32768) pred = -32768;
    index += kIndex[nibble];
    if (index < 0) index = 0;
    if (index > 88) index = 88;
    return pred;
  }
};

// Decodes whole blocks into planar int16 values (no clamp to total_frames).
// false if a block header is malformed (step > 88 or the reserved byte != 0).
bool ReferenceImaDecode(const std::uint8_t* data, std::uint64_t bytes, std::uint32_t ch, std::uint32_t blockAlign,
                        std::vector<std::vector<int>>* out, std::string* why) {
  out->assign(ch, {});
  const std::uint32_t groups = (blockAlign - 4u * ch) / (4u * ch);
  for (std::uint64_t at = 0; at + blockAlign <= bytes; at += blockAlign) {
    const std::uint8_t* b = data + at;
    std::vector<ImaChannel> st(ch);
    for (std::uint32_t c = 0; c < ch; ++c) {
      st[c].pred = static_cast<std::int16_t>(Le16(b + 4 * c));
      st[c].index = b[4 * c + 2];
      if (st[c].index > 88 || b[4 * c + 3] != 0) {
        *why = "block at byte " + std::to_string(at) + " channel " + std::to_string(c) + ": step " +
               std::to_string(b[4 * c + 2]) + ", reserved " + std::to_string(b[4 * c + 3]);
        return false;
      }
      (*out)[c].push_back(st[c].pred);
    }
    const std::uint8_t* p = b + 4 * ch;
    for (std::uint32_t g = 0; g < groups; ++g) {
      for (std::uint32_t c = 0; c < ch; ++c) {
        for (int k = 0; k < 4; ++k) {
          const std::uint8_t v = *p++;
          (*out)[c].push_back(st[c].Next(v & 0x0F));  // low nibble first
          (*out)[c].push_back(st[c].Next(v >> 4));
        }
      }
    }
  }
  return true;
}

bt::PcmData Chirp(std::uint32_t channels, std::uint32_t rate, std::uint64_t frames) {
  bt::PcmData p;
  p.channels = channels;
  p.sampleRate = rate;
  p.frames = frames;
  p.samples.resize(static_cast<std::size_t>(frames * channels));
  for (std::uint64_t f = 0; f < frames; ++f) {
    const double t = static_cast<double>(f) / rate;
    for (std::uint32_t c = 0; c < channels; ++c) {
      const double hz = 200.0 + 150.0 * c + 800.0 * t;
      p.samples[static_cast<std::size_t>(f * channels + c)] = static_cast<float>(0.6 * std::sin(kTwoPi * hz * t));
    }
  }
  return p;
}

std::vector<float> Planar(const bt::PcmData& p, std::uint32_t c) {
  std::vector<float> v(static_cast<std::size_t>(p.frames));
  for (std::uint64_t f = 0; f < p.frames; ++f) v[static_cast<std::size_t>(f)] = p.samples[f * p.channels + c];
  return v;
}

std::string AssetJson(const std::string& codec, const std::string& form, const std::string& extra = "") {
  return "\"file\": \"audio/" + codec + ".wav\", \"loadForm\": \"" + form + "\", \"convert\": {\"default\": {\"codec\": \"" +
         codec + "\"}}" + extra;
}

}  // namespace

// §6.5 PCMF / PC16: the exact byte values, computed here from the source samples.
SND_TEST(IndepFormat11, PcmBytesAreTheSpecLayout) {
  mini::Project proj("indep-pcm-layout");
  proj.Add("banks/Main.json", "bank", "Main");
  proj.Add("buses/Master.json", "bus", "Master", "\"master\": true");
  bt::PcmData src;
  src.channels = 3;
  src.sampleRate = 32000;
  src.frames = 777;
  src.samples.resize(src.frames * src.channels);
  for (std::size_t i = 0; i < src.samples.size(); ++i) {
    const double k = static_cast<double>(static_cast<int>(i % 200) - 100);
    const double frac = (i % 3 == 0) ? 0.3 : (i % 3 == 1 ? 0.7 : 0.49);
    float v = static_cast<float>((k * 97.0 + frac) / 32768.0);  // between two int16 steps, never a tie
    if (i % 101 == 5) v = 1.5f;       // clamps to 32767
    if (i % 101 == 6) v = -1.25f;     // clamps to -32768
    if (i % 101 == 7) v = -1.0f;      // exactly -32768
    if (i % 101 == 8) v = 0.99999f;
    src.samples[i] = v;
  }
  proj.Wav("audio/PCMF.wav", src, bt::WavSampleFormat::Float32);
  proj.Wav("audio/PC16.wav", src, bt::WavSampleFormat::Float32);
  for (const char* c : {"PCMF", "PC16"}) {
    proj.Add(std::string("assets/") + c + ".json", "asset", c, AssetJson(c, "pcm"));
    proj.Add(std::string("events/E") + c + ".json", "event", std::string("E") + c,
             std::string("\"bank\": \"Main\", \"tracks\": [{\"clips\": [{\"asset\": \"") + c + "\"}]}]");
  }
  bt::Diagnostics d;
  SND_REQUIRE_MSG(proj.Build(&d), "%s", mini::Dump(d).c_str());  // library default registry: PCMF + PC16
  pipeline::ReadBank bank(mini::ReadBytes(proj.out() / "Main.sndb"));
  SND_REQUIRE_MSG(bank.ok, "%s", bank.error.message);
  const bk::BankView& v = bank.view;
  for (const char* c : {"PCMF", "PC16"}) {
    const bool f32 = std::strcmp(c, "PCMF") == 0;
    const std::uint32_t row = v.FindRow<bk::col::ASST_id>(HashId(c));
    SND_REQUIRE(row != bf::kNone);
    const snd::CodecHeader h = pipeline::AssetHeader(v, row);
    SND_CHECK_EQ(h.codec, f32 ? SND_CODEC_PCM_F32 : SND_CODEC_PCM_S16);
    SND_CHECK_EQ(h.channels, 3u);
    SND_CHECK_EQ(h.sample_rate, 32000u);
    SND_CHECK_EQ(h.total_frames, 777u);
    SND_CHECK_EQ(h.pre_skip, 0u);
    SND_CHECK_EQ(h.pre_roll, 0u);
    SND_CHECK(h.frames_per_packet == 0u || h.frames_per_packet == 1u);
    SND_CHECK(h.block_align == 0u || h.block_align == (f32 ? 4u : 2u) * 3u);
    SND_CHECK_EQ(v.Column<bk::col::ASST_extraBytes>()[row], 0u);
    const bk::ByteSpan data = v.WaveData(row);
    SND_REQUIRE_EQ(data.bytes, static_cast<std::uint64_t>(src.samples.size()) * (f32 ? 4u : 2u));
    SND_CHECK_EQ(reinterpret_cast<std::uintptr_t>(data.data) % 64u, 0u);  // §6.2: each asset 64-byte aligned
    std::size_t bad = 0, firstBad = SIZE_MAX;
    for (std::size_t i = 0; i < src.samples.size(); ++i) {  // interleaved: frame-major, channel-minor
      bool ok;
      if (f32) {
        std::uint32_t bits;
        std::memcpy(&bits, &src.samples[i], 4);
        ok = Le32(data.data + 4 * i) == bits;
      } else {
        double s = std::floor(static_cast<double>(src.samples[i]) * 32768.0 + 0.5);
        if (s > 32767) s = 32767;
        if (s < -32768) s = -32768;
        ok = static_cast<std::int16_t>(Le16(data.data + 2 * i)) == static_cast<int>(s);
      }
      if (!ok && bad++ == 0) firstBad = i;
    }
    SND_CHECK_MSG(bad == 0, "%s: %zu of %zu samples differ from the spec layout (first at %zu)", c, bad,
                  src.samples.size(), firstBad);
  }
}

// §6.5 ADPC: geometry, block headers, a reference WAV/IMA decoder, seek rows.
SND_TEST(IndepFormat11, AdpcmBytesAreWavImaAndSeekRowsArePacketBoundaries) {
  for (std::uint32_t ch : {1u, 2u, 3u, 8u}) {
    mini::Project proj("indep-adpcm-" + std::to_string(ch));
    proj.Add("banks/Main.json", "bank", "Main");
    proj.Add("buses/Master.json", "bus", "Master", "\"master\": true");
    const std::uint64_t frames = 1017u * 7u + 333u;  // not a whole number of blocks
    const bt::PcmData src = Chirp(ch, 44100, frames);
    proj.Wav("audio/ADPC.wav", src, bt::WavSampleFormat::Float32);
    proj.Add("assets/ADPC.json", "asset", "ADPC", AssetJson("ADPC", "compressed", ", \"seekIntervalMs\": 30"));
    proj.Add("events/E.json", "event", "E", "\"bank\": \"Main\", \"tracks\": [{\"clips\": [{\"asset\": \"ADPC\"}]}]");
    bt::BuildOptions o = proj.Options();
    o.encoders = &bt::CodecEncoders();
    bt::Diagnostics d;
    SND_REQUIRE_MSG(proj.Build(o, &d), "%s", mini::Dump(d).c_str());
    pipeline::ReadBank bank(mini::ReadBytes(proj.out() / "Main.sndb"));
    SND_REQUIRE_MSG(bank.ok, "%s", bank.error.message);
    const bk::BankView& v = bank.view;
    const std::uint32_t row = v.FindRow<bk::col::ASST_id>(HashId("ADPC"));
    SND_REQUIRE(row != bf::kNone);
    const snd::CodecHeader h = pipeline::AssetHeader(v, row);
    SND_CHECK_EQ(h.codec, SND_CODEC_ADPCM);
    SND_CHECK_EQ(h.channels, ch);
    SND_CHECK_EQ(h.total_frames, frames);
    SND_CHECK_EQ(h.pre_skip, 0u);
    SND_CHECK_EQ(h.pre_roll, 0u);  // blocks are independent
    SND_CHECK_EQ(v.Column<bk::col::ASST_extraBytes>()[row], 0u);
    // Geometry (§6.5).
    SND_CHECK(h.block_align <= 1024u * ch);
    SND_CHECK_EQ(h.block_align % (4u * ch), 0u);
    SND_CHECK(h.block_align / (4u * ch) >= 2u);  // 4ch(1 + k), k >= 1
    const std::uint32_t fpb = 1u + (h.block_align - 4u * ch) / (4u * ch) * 8u;
    SND_CHECK(h.frames_per_packet == 0u || h.frames_per_packet == fpb);
    SND_CHECK_EQ(h.block_align, 512u * ch);  // the documented default
    SND_CHECK_EQ(fpb, 1017u);
    const std::uint64_t blocks = (frames + fpb - 1) / fpb;
    const bk::ByteSpan data = v.WaveData(row);
    SND_REQUIRE_EQ(data.bytes, blocks * h.block_align);
    // Default decoder slot (32 KiB) holds the state up to 8 channels (§6.5).
    SND_CHECK(snd::codec::BuiltinCodec(SND_CODEC_ADPCM)->StateBytes(h) <= 32768u);
    // Reference decoder (written from the spec) vs codec's decoder.
    std::vector<std::vector<int>> ref;
    std::string why;
    SND_REQUIRE_MSG(ReferenceImaDecode(data.data, data.bytes, ch, h.block_align, &ref, &why), "%s", why.c_str());
    SND_REQUIRE_EQ(ref[0].size(), blocks * fpb);
    std::vector<std::vector<float>> dec;
    SND_REQUIRE(pipeline::DecodeAll(h, data.data, data.bytes, 1000, &dec));
    for (std::uint32_t c = 0; c < ch; ++c) {
      std::size_t mismatch = 0;
      for (std::uint64_t f = 0; f < frames; ++f) {
        const float want = static_cast<float>(ref[c][static_cast<std::size_t>(f)]) / 32768.0f;
        if (dec[c][static_cast<std::size_t>(f)] != want) ++mismatch;
      }
      SND_CHECK_MSG(mismatch == 0, "ch %u/%u: %zu frames differ from the reference IMA decoder", c, ch, mismatch);
      const double snr = pipeline::SnrDb(Planar(src, c), dec[c]);
      SND_CHECK_MSG(snr > 22.0, "ch %u/%u: ADPCM SNR %.2f dB", c, ch, snr);
    }
    // SEEK rows: {0,0} first, packet boundaries, spacing >= 30 ms, decodable.
    const snd::RangeU32 seek = v.SeekRange(row);
    SND_REQUIRE(seek.count >= 2u);
    const std::uint64_t interval = 44100u * 30u / 1000u;
    snd::ICodec* codec = snd::codec::BuiltinCodec(SND_CODEC_ADPCM);
    std::vector<std::uint64_t> stateMem((codec->StateBytes(h) + 7) / 8 + 8);
    auto* st = reinterpret_cast<std::uint8_t*>(stateMem.data());
    while (reinterpret_cast<std::uintptr_t>(st) % 64 != 0) ++st;
    std::uint64_t prev = 0;
    for (std::uint32_t i = 0; i < seek.count; ++i) {
      const std::uint64_t f = v.Column<bk::col::SEEK_frame>()[seek.first + i];
      const std::uint64_t b = v.Column<bk::col::SEEK_byteOffset>()[seek.first + i];
      if (i == 0) {
        SND_CHECK(f == 0u && b == 0u);
      } else {
        SND_CHECK_MSG(f > prev, "row %u frame %llu not increasing", i, static_cast<unsigned long long>(f));
        SND_CHECK_MSG(f - prev >= interval || f % fpb == 0u, "row %u spacing %llu", i,
                      static_cast<unsigned long long>(f - prev));
      }
      SND_CHECK_MSG(f % fpb == 0u, "row %u frame %llu not a block boundary", i, static_cast<unsigned long long>(f));
      SND_CHECK_EQ(b, f / fpb * h.block_align);
      SND_CHECK(f < frames);
      prev = f;
      // Decode a little from the row and compare with the continuous decode.
      codec->Init(st, h);
      codec->Seek(st, f);
      std::vector<std::vector<float>> tmp(ch, std::vector<float>(256));
      std::vector<float*> dst(ch);
      for (std::uint32_t c = 0; c < ch; ++c) dst[c] = tmp[c].data();
      const std::uint64_t n = std::min<std::uint64_t>(h.block_align, data.bytes - b);
      const std::uint32_t got = codec->Decode(st, data.data + b, static_cast<std::uint32_t>(n), dst.data(), 256);
      SND_REQUIRE(got != SND_DECODE_ERROR && got > 0u);
      for (std::uint32_t c = 0; c < ch; ++c) {
        for (std::uint32_t k = 0; k < got && f + k < frames; ++k) {
          if (tmp[c][k] != dec[c][static_cast<std::size_t>(f + k)]) {
            SND_CHECK_MSG(false, "seek row %u: frame %llu ch %u differs", i, static_cast<unsigned long long>(f + k), c);
            break;
          }
        }
      }
    }
  }
}

// §6.3: stream file and preload of an ADPCM stream asset.
SND_TEST(IndepFormat11, StreamFileAndPreloadFollowTheSpec) {
  mini::Project proj("indep-stream");
  proj.Add("banks/Main.json", "bank", "Main");
  proj.Add("buses/Master.json", "bus", "Master", "\"master\": true");
  const bt::PcmData src = Chirp(2, 48000, 48000);
  proj.Wav("audio/ADPC.wav", src, bt::WavSampleFormat::Float32);
  proj.Add("assets/S.json", "asset", "S", AssetJson("ADPC", "stream", ", \"preloadMs\": 50, \"seekIntervalMs\": 100"));
  proj.Add("events/E.json", "event", "E", "\"bank\": \"Main\", \"tracks\": [{\"clips\": [{\"asset\": \"S\"}]}]");
  bt::BuildOptions o = proj.Options();
  o.encoders = &bt::CodecEncoders();
  bt::Diagnostics d;
  bt::BuildResult res;
  SND_REQUIRE_MSG(proj.Build(o, &d, &res), "%s", mini::Dump(d).c_str());
  pipeline::ReadBank bank(mini::ReadBytes(proj.out() / "Main.sndb"));
  SND_REQUIRE_MSG(bank.ok, "%s", bank.error.message);
  const bk::BankView& v = bank.view;
  const std::uint32_t row = v.FindRow<bk::col::ASST_id>(HashId("S"));
  SND_REQUIRE(row != bf::kNone);
  SND_CHECK_EQ(v.Column<bk::col::ASST_loadForm>()[row], 3u);
  const snd::CodecHeader h = pipeline::AssetHeader(v, row);
  const std::uint32_t strm = v.Column<bk::col::ASST_stream>()[row];
  SND_REQUIRE(strm != bf::kNone);
  const char* path = v.StreamPath(strm);
  SND_REQUIRE(path != nullptr);
  const std::vector<std::uint8_t> file = mini::ReadBytes(proj.out() / path);
  SND_REQUIRE(file.size() >= 64u);
  const std::uint32_t sector = v.Column<bk::col::STRM_sectorAlign>()[strm];
  SND_CHECK_EQ(sector, 2048u);
  SND_CHECK_EQ(Le32(file.data() + 0), bf::kStreamMagic);
  SND_CHECK_EQ(Le32(file.data() + 4), 1u);
  SND_CHECK_EQ(Le32(file.data() + 8), HashId("S"));
  SND_CHECK_EQ(Le32(file.data() + 12), SND_CODEC_ADPCM);
  SND_CHECK_EQ(Le32(file.data() + 16), 2u);
  SND_CHECK_EQ(Le32(file.data() + 20), 48000u);
  SND_CHECK_EQ(Le64(file.data() + 24), h.total_frames);
  const std::uint64_t dataOffset = Le64(file.data() + 32);
  const std::uint64_t dataBytes = Le64(file.data() + 40);
  SND_CHECK_EQ(dataOffset % sector, 0u);
  SND_CHECK_EQ(dataOffset, v.Column<bk::col::STRM_dataOffset>()[strm]);
  SND_CHECK_EQ(Le32(file.data() + 48), sector);
  SND_CHECK_EQ(Le32(file.data() + 52), 0u);
  SND_CHECK_EQ(Le64(file.data() + 56), 0u);
  SND_CHECK_EQ(file.size() % sector, 0u);
  SND_CHECK_EQ(static_cast<std::uint64_t>(file.size()), v.Column<bk::col::STRM_fileBytes>()[strm]);
  const std::uint32_t fpb = 1u + (h.block_align - 4u * 2u) / 8u * 8u;
  SND_CHECK_EQ(dataBytes, (h.total_frames + fpb - 1) / fpb * h.block_align);
  bool zeros = true;
  for (std::size_t i = 64; i < dataOffset; ++i) zeros = zeros && file[i] == 0;
  for (std::size_t i = static_cast<std::size_t>(dataOffset + dataBytes); i < file.size(); ++i) zeros = zeros && file[i] == 0;
  SND_CHECK_MSG(zeros, "padding is not zero");
  // The data decodes to the source (the stream file holds the codec data as §6.5).
  std::vector<std::vector<float>> dec;
  SND_REQUIRE(pipeline::DecodeAll(h, file.data() + dataOffset, dataBytes, 4096, &dec));
  SND_CHECK(pipeline::SnrDb(Planar(src, 0), dec[0]) > 22.0);
  // PREL: a prefix of the data, whole blocks, at least preloadMs.
  const std::uint64_t pOff = v.Column<bk::col::ASST_preloadOffset>()[row];
  const std::uint32_t pBytes = v.Column<bk::col::ASST_preloadBytes>()[row];
  const std::uint32_t pFrames = v.Column<bk::col::ASST_preloadFrames>()[row];
  const bk::ByteSpan prel = v.Chunk(bf::chunk::PREL);
  SND_REQUIRE(pBytes > 0u && pOff + pBytes <= prel.bytes);
  SND_CHECK_EQ(pBytes % h.block_align, 0u);
  SND_CHECK_EQ(pFrames, pBytes / h.block_align * fpb);
  SND_CHECK(pFrames >= 48000u * 50u / 1000u);
  SND_CHECK(pFrames < 48000u * 50u / 1000u + fpb);
  SND_CHECK(std::memcmp(prel.data + pOff, file.data() + dataOffset, pBytes) == 0);
  // SEEK rows of a stream count from the data start (§6.3).
  const snd::RangeU32 seek = v.SeekRange(row);
  SND_REQUIRE(seek.count >= 2u);
  for (std::uint32_t i = 0; i < seek.count; ++i) {
    const std::uint64_t f = v.Column<bk::col::SEEK_frame>()[seek.first + i];
    SND_CHECK_EQ(v.Column<bk::col::SEEK_byteOffset>()[seek.first + i], f / fpb * h.block_align);
  }
}

namespace {
// Channels the effects of a bus run at (bank-format.md BUSS.channels: 0 = parent's;
// the root's 0 is taken as 2 here, the build tool's rule - see the report: the
// runtime may run the master at the device's channel count).
std::uint32_t Channels(const bk::BankView& v, std::uint32_t row) {
  for (int depth = 0; row != bf::kNone && depth < 64; ++depth) {
    const std::uint32_t ch = v.Column<bk::col::BUSS_channels>()[row];
    if (ch != 0) return ch;
    row = v.Column<bk::col::BUSS_parent>()[row];
  }
  return 2;
}

void FxProject(mini::Project& proj) {
  proj.Add("banks/Main.json", "bank", "Main");
  proj.Add("buses/Master.json", "bus", "Master",
           "\"master\": true, \"effects\": [{\"type\": \"snd.fx.limiter\", \"params\": [-1, 50, 10]}]");
  proj.Add("buses/Mono.json", "bus", "Mono",
           "\"bank\": \"Main\", \"parent\": \"Master\", \"channels\": 1, \"effects\": [{\"type\": \"snd.fx.delay\", \"params\": [100, 0.5, 0, -6, 20000, 0, 4000]}]");
  proj.Add("buses/Wide.json", "bus", "Wide",
           "\"parent\": \"Master\", \"channels\": 8, \"effects\": [{\"type\": \"snd.fx.reverb\"}, {\"type\": \"snd.fx.surround\"}]");
  // Inherits 8 from Wide through Mid (2 levels); echo MaxDelay [init] above its range (warning, clamped).
  proj.Add("buses/Mid.json", "bus", "Mid", "\"parent\": \"Wide\"");
  proj.Add("buses/Leaf.json", "bus", "Leaf",
           "\"parent\": \"Mid\", \"effects\": [{\"type\": \"snd.fx.echo\", \"params\": [300, 0.5, 1, 1, 9000]}, "
           "{\"type\": \"snd.fx.pitchshift\", \"params\": [1.5, 200, 1]}, {\"type\": \"snd.fx.chorus\"}]");
  proj.Wav("audio/a.wav", mini::Tone(1, 48000, 480));
  proj.Add("assets/a.json", "asset", "a", "\"file\": \"audio/a.wav\"");
  proj.Add("events/E.json", "event", "E", "\"bank\": \"Main\", \"bus\": \"Leaf\", \"tracks\": [{\"clips\": [{\"asset\": \"a\"}]}]");
}
}  // namespace

SND_TEST(IndepFormat11, DspNeedMatchesDspSizingForInheritedChannelsAndOddRates) {
  std::uint64_t bytesAt[3] = {};
  const std::uint32_t rates[3] = {22050u, 44100u, 96000u};
  const std::uint32_t blocks[3] = {64u, 1000u, 256u};
  for (int k = 0; k < 3; ++k) {
    mini::Project proj("indep-dspn-" + std::to_string(k));
    FxProject(proj);
    bt::BuildOptions o = proj.Options();
    o.dspSampleRate = rates[k];
    o.dspMaxFrames = blocks[k];
    bt::Diagnostics d;
    SND_REQUIRE_MSG(proj.Build(o, &d), "%s", mini::Dump(d).c_str());
    // The out-of-range [init] value is reported once (banks share buses).
    SND_CHECK(mini::HasDiag(d, bt::Severity::Warning, "parameter 4 (MaxDelay)"));
    pipeline::ReadBank bank(mini::ReadBytes(proj.out() / "Main.sndb"));
    SND_REQUIRE_MSG(bank.ok, "%s", bank.error.message);
    const bk::BankView& v = bank.view;
    std::vector<snd::EffectDesc> descs;
    std::vector<std::vector<float>> params;
    params.reserve(32);
    std::uint64_t sumColumn = 0;
    for (std::uint32_t b = 0; b < v.Rows(bf::chunk::BUSS); ++b) {
      const std::vector<snd::EffectDesc> fx =
          pipeline::BusEffectDescs(v, b, Channels(v, b), rates[k], blocks[k], &params);
      descs.insert(descs.end(), fx.begin(), fx.end());
    }
    for (std::uint32_t i = 0; i < v.Rows(bf::chunk::EFFX); ++i) {
      sumColumn += (v.Column<bk::col::EFFX_stateBytes>()[i] + 63u) / 64u * 64u;
    }
    SND_REQUIRE_EQ(descs.size(), 7u);
    std::uint32_t unknown = 99;
    const bf::DspPoolNeed rt =
        snd::dsp::ComputeDspPoolNeed(descs.data(), static_cast<std::uint32_t>(descs.size()), rates[k], nullptr, 0, &unknown);
    SND_CHECK_EQ(unknown, 0u);
    const bf::DspPoolNeed built = v.DspNeed();
    SND_CHECK_MSG(built.instanceBytes == rt.instanceBytes, "rate %u block %u: DSPN %llu, dsp %llu", rates[k], blocks[k],
                  static_cast<unsigned long long>(built.instanceBytes), static_cast<unsigned long long>(rt.instanceBytes));
    SND_CHECK_EQ(built.instanceCount, rt.instanceCount);
    SND_CHECK_EQ(built.instanceCount, 7u);
    SND_CHECK_EQ(built.instanceBytes, sumColumn);  // §6.2: DSPN = sum of roundup64(EFFX.stateBytes)
    SND_CHECK_EQ(built.instanceBytes % 64u, 0u);
    // The clamped [init] value sizes like the bound itself.
    for (std::size_t i = 0; i < descs.size(); ++i) {
      if (descs[i].effect_type != snd::dsp::kEffectEcho) continue;
      SND_CHECK_EQ(descs[i].channels, 8u);  // inherited through two parents
      std::vector<float> atBound(descs[i].initial_params, descs[i].initial_params + descs[i].param_count);
      atBound[4] = 5000.0f;
      snd::EffectDesc b2 = descs[i];
      b2.initial_params = atBound.data();
      SND_CHECK_EQ(snd::dsp::EffectInstanceBytes(b2, rates[k], nullptr, 0),
                   snd::dsp::EffectInstanceBytes(descs[i], rates[k], nullptr, 0));
    }
    // D-48: a desc carrying another rate cannot be sized.
    std::vector<snd::EffectDesc> other = descs;
    for (snd::EffectDesc& x : other) x.sample_rate = rates[k] + 1u;
    std::uint32_t bad = 0;
    const bf::DspPoolNeed none =
        snd::dsp::ComputeDspPoolNeed(other.data(), static_cast<std::uint32_t>(other.size()), rates[k], nullptr, 0, &bad);
    SND_CHECK_EQ(bad, 7u);
    SND_CHECK_EQ(none.instanceBytes, 0u);
    bytesAt[k] = built.instanceBytes;
  }
  // The build's rate matters (delay lines grow with the rate).
  SND_CHECK(bytesAt[0] < bytesAt[1] && bytesAt[1] < bytesAt[2]);
}

SND_TEST(IndepFormat11, EffectParameterCountLimitsComeFromDsp) {
  for (std::uint32_t kind = 0; kind < static_cast<std::uint32_t>(snd::dsp::EffectKind::Count); ++kind) {
    const snd::dsp::EffectInfo& info = snd::dsp::GetEffectInfo(static_cast<snd::dsp::EffectKind>(kind));
    for (std::uint32_t extra : {0u, 1u}) {
      mini::Project proj("indep-fxcount-" + std::to_string(kind) + "-" + std::to_string(extra));
      proj.Minimal();
      std::string params;
      for (std::uint32_t i = 0; i < info.paramCount + extra; ++i) {
        const float val = i < info.paramCount ? info.params[i].defaultValue : 0.0f;
        char b[32];
        std::snprintf(b, sizeof(b), "%s%.9g", i == 0 ? "" : ", ", static_cast<double>(val));
        params += b;
      }
      proj.Add("buses/B.json", "bus", "B",
               std::string("\"bank\": \"Main\", \"parent\": \"Master\", \"effects\": [{\"type\": \"") + info.name +
                   "\", \"params\": [" + params + "]}]");
      bt::Diagnostics d;
      const bool ok = proj.Build(&d);
      if (extra == 0) {
        SND_CHECK_MSG(ok && d.items().empty(), "%s with its %u defaults: %s", info.name, info.paramCount,
                      mini::Dump(d).c_str());
      } else {
        SND_CHECK_MSG(!ok && mini::HasDiag(d, bt::Severity::Error, "parameters"), "%s with %u parameters accepted",
                      info.name, info.paramCount + 1);
      }
    }
  }
}
