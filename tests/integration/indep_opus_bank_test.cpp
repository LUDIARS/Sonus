// Independent tests: Opus assets in the banks and stream files written by the
// build tool, read with a reader written from bank-format.md alone (§2-§5
// file / chunk / table layout, §6 ASST / SEEK / STRM / WAVE / PREL / SPTH,
// §6.3 stream files, §6.5 Opus) - not with the bank module - and the Opus
// settings of project-format.md (layering, ranges, cache keys, warnings).
// Properties: scratchpad indep-opus-properties.md A6, B4, E5, G1, H1, H2, I1, I2.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "indep_opus_ref.hpp"
#include "mini_project.hpp"
#include "snd/buildtool/build.hpp"
#include "snd/buildtool/plugins.hpp"
#include "snd/codec/codec.hpp"
#include "snd/codec/encode.hpp"
#include "snd/id.hpp"
#include "snd/test/test.hpp"

namespace bt = snd::buildtool;
namespace ce = snd::codec::encode;

namespace {

std::uint32_t U32(const std::uint8_t* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | (static_cast<std::uint32_t>(p[3]) << 24); }
std::uint64_t U64(const std::uint8_t* p) { return U32(p) | (static_cast<std::uint64_t>(U32(p + 4)) << 32); }
std::uint16_t U16(const std::uint8_t* p) { return static_cast<std::uint16_t>(p[0] | (p[1] << 8)); }

// --- bank reader from bank-format.md §2-§5 ---------------------------------------------
struct Chunk {
  std::uint64_t offset = 0, bytes = 0;
  bool present = false;
};
struct Table {
  const std::uint8_t* base = nullptr;
  std::uint32_t rows = 0;
  std::map<std::uint32_t, std::pair<std::uint32_t, std::uint64_t>> cols;  // id -> {elemBytes, offset}
  std::uint64_t Get(std::uint32_t col, std::uint32_t row, std::uint64_t def) const {
    auto it = cols.find(col);
    if (it == cols.end()) return def;
    const std::uint8_t* p = base + it->second.second + static_cast<std::uint64_t>(row) * it->second.first;
    return it->second.first == 8 ? U64(p) : U32(p);
  }
};
struct RawBank {
  std::vector<std::uint8_t> b;
  std::map<std::string, Chunk> chunks;
  std::string error;
  explicit RawBank(std::vector<std::uint8_t> bytes) : b(std::move(bytes)) {
    if (b.size() < 128 || std::memcmp(b.data(), "SNDB", 4) != 0) {
      error = "magic";
      return;
    }
    if (U16(b.data() + 4) != 2) error = "formatMajor";
    if (U64(b.data() + 24) != b.size()) error = "fileBytes";
    const std::uint32_t count = U32(b.data() + 60);
    const std::uint64_t table = U64(b.data() + 72);
    for (std::uint32_t i = 0; i < count; ++i) {
      const std::uint8_t* e = b.data() + table + 32u * i;
      Chunk c;
      c.offset = U64(e + 16);
      c.bytes = U64(e + 24);
      c.present = true;
      if (c.offset % 64 != 0) error = "chunk offset";
      chunks[std::string(reinterpret_cast<const char*>(e), 4)] = c;
    }
  }
  const std::uint8_t* Data(const std::string& f) const {
    auto it = chunks.find(f);
    return it == chunks.end() ? nullptr : b.data() + it->second.offset;
  }
  std::uint64_t Bytes(const std::string& f) const {
    auto it = chunks.find(f);
    return it == chunks.end() ? 0 : it->second.bytes;
  }
  Table Tab(const std::string& f) const {
    Table t;
    const std::uint8_t* c = Data(f);
    if (c == nullptr) return t;
    t.base = c;
    t.rows = U32(c);
    const std::uint32_t n = U32(c + 4);
    for (std::uint32_t i = 0; i < n; ++i) {
      const std::uint8_t* ce = c + 16 + 16u * i;
      t.cols[U16(ce)] = {U16(ce + 2), U64(ce + 8)};
    }
    return t;
  }
};

// ASST column ids (bank-format.md §6 ASST).
enum : std::uint32_t {
  A_id = 0, A_loadForm = 1, A_codec = 2, A_channels = 3, A_sampleRate = 4, A_totalFrames = 5, A_preSkip = 6,
  A_preRoll = 7, A_fpp = 8, A_blockAlign = 9, A_dataOffset = 10, A_dataBytes = 11, A_preloadOffset = 14,
  A_preloadBytes = 15, A_preloadFrames = 16, A_seekFirst = 17, A_seekCount = 18, A_stream = 19, A_extraOffset = 20,
  A_extraBytes = 21
};

struct Asset {
  snd::CodecHeader h{};
  std::uint32_t loadForm = 0;
  std::vector<std::uint8_t> setup;
  std::vector<std::uint8_t> data;  // resident data or the stream file's data
  std::vector<std::uint64_t> seekFrames, seekBytes;
  std::vector<std::uint8_t> preload;
  std::uint32_t preloadFrames = 0;
  std::string error;  // first violation of the spec found while reading
};

// Reads asset `name` from bank + out dir with the spec's rules, checking them on the way.
Asset ReadAsset(const RawBank& bank, const mini::fs::path& outDir, const char* name) {
  Asset a;
  const Table t = bank.Tab("ASST");
  std::uint32_t row = 0xFFFFFFFFu;
  for (std::uint32_t r = 0; r < t.rows; ++r) {
    if (t.Get(A_id, r, 0) == snd::HashId(name)) row = r;
  }
  if (row == 0xFFFFFFFFu) {
    a.error = "no ASST row";
    return a;
  }
  a.loadForm = static_cast<std::uint32_t>(t.Get(A_loadForm, row, 0));
  a.h.codec = static_cast<std::uint32_t>(t.Get(A_codec, row, 0));
  a.h.channels = static_cast<std::uint32_t>(t.Get(A_channels, row, 0));
  a.h.sample_rate = static_cast<std::uint32_t>(t.Get(A_sampleRate, row, 0));
  a.h.total_frames = t.Get(A_totalFrames, row, 0);
  a.h.pre_skip = static_cast<std::uint32_t>(t.Get(A_preSkip, row, 0));
  a.h.pre_roll = static_cast<std::uint32_t>(t.Get(A_preRoll, row, 0));
  a.h.frames_per_packet = static_cast<std::uint32_t>(t.Get(A_fpp, row, 0));
  a.h.block_align = static_cast<std::uint32_t>(t.Get(A_blockAlign, row, 0));
  const std::uint8_t* wave = bank.Data("WAVE");
  const std::uint64_t waveBytes = bank.Bytes("WAVE");
  const std::uint64_t xo = t.Get(A_extraOffset, row, 0), xb = t.Get(A_extraBytes, row, 0);
  if (xb == 0 || wave == nullptr || xo + xb > waveBytes) {
    a.error = "extra missing or outside WAVE";
    return a;
  }
  if (xo % 64 != 0) a.error = "extraOffset not 64-byte aligned";
  a.setup.assign(wave + xo, wave + xo + xb);
  a.h.extra = a.setup.data();
  a.h.extra_bytes = static_cast<std::uint32_t>(xb);
  const std::uint32_t strm = static_cast<std::uint32_t>(t.Get(A_stream, row, 0xFFFFFFFFu));
  if (a.loadForm == 3) {  // Stream: the stream file (6.3)
    const Table st = bank.Tab("STRM");
    const std::uint8_t* spth = bank.Data("SPTH");
    if (strm >= st.rows || spth == nullptr) {
      a.error = "stream row";
      return a;
    }
    const std::string rel(reinterpret_cast<const char*>(spth + st.Get(0, strm, 0)));
    const std::vector<std::uint8_t> f = mini::ReadBytes(outDir / rel);
    const std::uint64_t dataOffset = st.Get(2, strm, 0);
    const std::uint32_t sector = static_cast<std::uint32_t>(st.Get(3, strm, 2048));
    if (f.size() < 64 || std::memcmp(f.data(), "SNDS", 4) != 0) {
      a.error = "stream magic";
      return a;
    }
    if (U32(f.data() + 4) != 1 || U32(f.data() + 8) != snd::HashId(name) || U32(f.data() + 12) != a.h.codec ||
        U32(f.data() + 16) != a.h.channels || U32(f.data() + 20) != a.h.sample_rate ||
        U64(f.data() + 24) != a.h.total_frames || U64(f.data() + 32) != dataOffset || U32(f.data() + 48) != sector) {
      a.error = "stream file header disagrees with ASST / STRM";
    }
    const std::uint64_t db = U64(f.data() + 40);
    if (f.size() != st.Get(1, strm, 0) || f.size() % sector != 0 || dataOffset % sector != 0 ||
        dataOffset + db > f.size()) {
      a.error = "stream file size / alignment";
      return a;
    }
    for (std::uint64_t i = dataOffset + db; i < f.size(); ++i) {
      if (f[i] != 0) a.error = "stream padding not zero";
    }
    a.data.assign(f.begin() + static_cast<std::ptrdiff_t>(dataOffset),
                  f.begin() + static_cast<std::ptrdiff_t>(dataOffset + db));
    const std::uint64_t po = t.Get(A_preloadOffset, row, 0), pb = t.Get(A_preloadBytes, row, 0);
    const std::uint8_t* prel = bank.Data("PREL");
    if (pb > 0) {
      if (prel == nullptr || po + pb > bank.Bytes("PREL") || po % 64 != 0) {
        a.error = "preload range";
        return a;
      }
      a.preload.assign(prel + po, prel + po + pb);
      a.preloadFrames = static_cast<std::uint32_t>(t.Get(A_preloadFrames, row, 0));
    }
  } else {
    const std::uint64_t o = t.Get(A_dataOffset, row, 0), b = t.Get(A_dataBytes, row, 0);
    if (wave == nullptr || o + b > waveBytes) {
      a.error = "data outside WAVE";
      return a;
    }
    if (o % 64 != 0) a.error = "dataOffset not 64-byte aligned";
    a.data.assign(wave + o, wave + o + b);
  }
  const std::uint32_t sf = static_cast<std::uint32_t>(t.Get(A_seekFirst, row, 0xFFFFFFFFu));
  const std::uint32_t sc = static_cast<std::uint32_t>(t.Get(A_seekCount, row, 0));
  if (sc > 0) {
    const Table s = bank.Tab("SEEK");
    if (sf + static_cast<std::uint64_t>(sc) > s.rows) {
      a.error = "seek rows";
      return a;
    }
    for (std::uint32_t i = 0; i < sc; ++i) {
      a.seekFrames.push_back(s.Get(0, sf + i, 0));
      a.seekBytes.push_back(s.Get(1, sf + i, 0));
    }
  }
  return a;
}

// §6.5 checks of one asset read by ReadAsset.
void CheckAsset(const Asset& a, const char* name, std::uint32_t ch, std::uint64_t total, std::uint64_t seekInterval) {
  SND_REQUIRE_MSG(a.error.empty(), "%s: %s", name, a.error.c_str());
  SND_CHECK_MSG(a.h.codec == SND_CODEC_OPUS && a.h.channels == ch && a.h.sample_rate == 48000u, "%s: ASST", name);
  SND_CHECK_MSG(a.h.total_frames == total, "%s: totalFrames %llu want %llu", name,
                static_cast<unsigned long long>(a.h.total_frames), static_cast<unsigned long long>(total));
  SND_CHECK_MSG(a.h.pre_skip == 312u && a.h.pre_roll == 3840u && a.h.block_align == 0u, "%s: ASST", name);
  SND_CHECK_MSG(a.h.frames_per_packet == 960u || a.h.frames_per_packet == 0u, "%s: fpp", name);
  opusref::Setup s;
  const std::string se = opusref::ParseSetup(a.setup.data(), static_cast<std::uint32_t>(a.setup.size()), ch, &s);
  SND_REQUIRE_MSG(se.empty(), "%s: setup %s", name, se.c_str());
  std::vector<opusref::Packet> pk;
  const std::string we = opusref::WalkPackets(a.data.data(), a.data.size(), s.streams, &pk);
  SND_REQUIRE_MSG(we.empty(), "%s: %s", name, we.c_str());
  SND_CHECK_MSG(pk.size() == (total + 312u + 959u) / 960u, "%s: %zu packets", name, pk.size());
  if (seekInterval == 0) return;
  SND_REQUIRE_MSG(!a.seekFrames.empty(), "%s: no SEEK rows", name);
  SND_CHECK_MSG(a.seekFrames[0] == 0u && a.seekBytes[0] == 0u, "%s: first SEEK row", name);
  for (std::size_t i = 1; i < a.seekFrames.size(); ++i) {
    const std::uint64_t f = a.seekFrames[i];
    SND_CHECK_MSG((f + 312u) % 960u == 0u, "%s: SEEK %zu frame %llu", name, i, static_cast<unsigned long long>(f));
    SND_CHECK_MSG(f < total && f > a.seekFrames[i - 1] && f - a.seekFrames[i - 1] >= seekInterval, "%s: SEEK %zu", name, i);
    const std::uint64_t k = (f + 312u) / 960u;
    SND_CHECK_MSG(k < pk.size() && pk[k].offset == a.seekBytes[i], "%s: SEEK %zu byteOffset", name, i);
  }
}

bt::PcmData ToPcm(const opusref::Planar& p, std::uint32_t rate) {
  bt::PcmData d;
  d.channels = static_cast<std::uint32_t>(p.size());
  d.sampleRate = rate;
  d.frames = p[0].size();
  d.samples.resize(static_cast<std::size_t>(d.frames * d.channels));
  for (std::uint64_t f = 0; f < d.frames; ++f) {
    for (std::uint32_t c = 0; c < d.channels; ++c) d.samples[static_cast<std::size_t>(f * d.channels + c)] = p[c][f];
  }
  return d;
}

void Base(mini::Project& p) {
  p.Add("banks/Main.json", "bank", "Main");
  p.Add("buses/Master.json", "bus", "Master", "\"master\": true");
}
void Event(mini::Project& p, const std::string& asset) {
  p.Add("events/E_" + asset + ".json", "event", "E_" + asset,
        "\"bank\": \"Main\", \"tracks\": [{\"clips\": [{\"asset\": \"" + asset + "\"}]}]");
}

opusref::Planar DecodeAsset(const Asset& a, const std::vector<std::uint8_t>& data, bool* ok) {
  opusref::Encoded e;
  e.h = a.h;
  e.data = data;
  return opusref::DecodeAll(e, ok);
}

}  // namespace

// A6 + B3 + B4: the build's banks and stream files, read from the spec alone,
// decode to the input (levels per WAV channel, 44.1 kHz material at 48 kHz),
// SEEK rows walk to the packets, the preload is a prefix cut at a SEEK row.
SND_TEST(IndepOpusBank, BankBytesFollowTheSpec) {
  if (!snd::codec::OpusAvailable()) return;
  mini::Project proj("indep-opus-bank");
  Base(proj);
  const double hz6[6] = {330, 550, 770, 60, 1210, 1430};
  proj.Wav("audio/m.wav", ToPcm(opusref::Tones(1, 48000, {1000}, 0.5), 48000), bt::WavSampleFormat::Float32);
  proj.Wav("audio/m441.wav", ToPcm(opusref::Tones(1, 44100 + 7, {1000}, 0.5, 44100), 44100), bt::WavSampleFormat::Float32);
  proj.Wav("audio/s.wav", ToPcm(opusref::Tones(2, 96000 + 5, {440, 1250}, 0.4), 48000), bt::WavSampleFormat::Float32);
  proj.Wav("audio/six.wav", ToPcm(opusref::Tones(6, 48000, std::vector<double>(hz6, hz6 + 6), 0.3), 48000),
           bt::WavSampleFormat::Float32);
  const char* conv = "\"convert\": {\"default\": {\"codec\": \"OPUS\"}}";
  proj.Add("assets/m.json", "asset", "m", std::string("\"file\": \"audio/m.wav\", \"loadForm\": \"compressed\", ") + conv);
  proj.Add("assets/m441.json", "asset", "m441",
           std::string("\"file\": \"audio/m441.wav\", \"loadForm\": \"compressed\", ") + conv);
  proj.Add("assets/s.json", "asset", "s",
           std::string("\"file\": \"audio/s.wav\", \"loadForm\": \"stream\", \"preloadMs\": 100, \"seekIntervalMs\": 100, ") +
               conv);
  proj.Add("assets/six.json", "asset", "six",
           std::string("\"file\": \"audio/six.wav\", \"loadForm\": \"stream\", \"seekIntervalMs\": 50, ") + conv);
  for (const char* n : {"m", "m441", "s", "six"}) Event(proj, n);
  bt::BuildOptions o = proj.Options();
  o.encoders = &bt::CodecEncoders();
  bt::Diagnostics d;
  SND_REQUIRE_MSG(proj.Build(o, &d), "%s", mini::Dump(d).c_str());
  RawBank bank(mini::ReadBytes(proj.out() / "Main.sndb"));
  SND_REQUIRE_MSG(bank.error.empty(), "%s", bank.error.c_str());

  const Asset m = ReadAsset(bank, proj.out(), "m");
  CheckAsset(m, "m", 1, 48000, 4800);
  const Asset m441 = ReadAsset(bank, proj.out(), "m441");
  // 44107 frames at 44.1 kHz are 48007.6 frames at 48 kHz; the spec does not fix
  // the rounding ("48 kHz 換算"), so accept either neighbour.
  SND_REQUIRE_MSG(m441.error.empty(), "%s", m441.error.c_str());
  SND_CHECK_MSG(m441.h.total_frames == 48007u || m441.h.total_frames == 48008u, "m441: totalFrames %llu",
                static_cast<unsigned long long>(m441.h.total_frames));
  CheckAsset(m441, "m441", 1, m441.h.total_frames, 4800);
  const Asset s = ReadAsset(bank, proj.out(), "s");
  CheckAsset(s, "s", 2, 96005, 4800);
  const Asset six = ReadAsset(bank, proj.out(), "six");
  CheckAsset(six, "six", 6, 48000, 2400);

  // Tone levels (per WAV channel) after decoding the bytes read above.
  struct Want {
    const Asset* a;
    std::vector<double> hz;
    double amp;
  };
  const Want wants[] = {{&m, {1000}, 0.5}, {&m441, {1000}, 0.5}, {&s, {440, 1250}, 0.4},
                        {&six, std::vector<double>(hz6, hz6 + 6), 0.3}};
  for (const Want& w : wants) {
    bool ok = false;
    const opusref::Planar out = DecodeAsset(*w.a, w.a->data, &ok);
    SND_REQUIRE(ok);
    SND_CHECK_EQ(out[0].size(), w.a->h.total_frames);
    for (std::size_t c = 0; c < w.hz.size(); ++c) {
      const double own = opusref::ToneAmp(out[c], 9600, 19200, w.hz[c]);
      SND_CHECK_MSG(std::fabs(opusref::Db(own / w.amp)) < 1.5, "channel %zu of %u ch: %.2f dB", c, w.a->h.channels,
                    opusref::Db(own / w.amp));
      for (std::size_t x = 0; x < w.hz.size(); ++x) {
        if (x != c) SND_CHECK_MSG(opusref::ToneAmp(out[c], 9600, 19200, w.hz[x]) < 0.1 * own, "leak %zu -> %zu", x, c);
      }
    }
  }

  // Stream preload (6.1, §6.5 "PREL"): a prefix of the data cut at a SEEK row of
  // at least preloadMs, and its frame count is that row's frame.
  SND_REQUIRE(!s.preload.empty());
  SND_CHECK(std::equal(s.preload.begin(), s.preload.end(), s.data.begin()));
  bool atRow = false;
  for (std::size_t i = 1; i < s.seekFrames.size(); ++i) {
    if (s.seekBytes[i] == s.preload.size() && s.seekFrames[i] == s.preloadFrames) atRow = true;
  }
  SND_CHECK_MSG(atRow, "preload %zu bytes / %u frames is not a SEEK row", s.preload.size(), s.preloadFrames);
  SND_CHECK(s.preloadFrames >= 4800u);
  std::printf("  m441 totalFrames %llu; s: %zu SEEK rows, preload %zu bytes / %u frames; six: %zu SEEK rows, setup N %u M %u\n",
              static_cast<unsigned long long>(m441.h.total_frames), s.seekFrames.size(), s.preload.size(),
              s.preloadFrames, six.seekFrames.size(), six.setup[2], six.setup[3]);
  {
    bool ok = false;
    const opusref::Planar pre = DecodeAsset(s, s.preload, &ok);
    SND_REQUIRE(ok);
    SND_CHECK_EQ(pre[0].size(), s.preloadFrames);  // exactly the preload frames decode from it
  }

  // Seeks through the SEEK rows of the stream file are sample exact.
  bool ok = false;
  const opusref::Planar full = DecodeAsset(s, s.data, &ok);
  SND_REQUIRE(ok);
  for (std::uint64_t target : {0u, 5000u, 33333u, 47688u, 96004u}) {
    const std::uint32_t idx = snd::codec::FindSeekPoint(s.seekFrames.data(), static_cast<std::uint32_t>(s.seekFrames.size()),
                                                        target, s.h.pre_roll, s.h.total_frames);
    SND_REQUIRE(idx != snd::codec::kNoSeekPoint);
    opusref::Dec dec(s.h, 1500);
    dec.c->Seek(dec.mem.get(), s.seekFrames[idx]);
    SND_REQUIRE(dec.FeedAll(s.data.data() + s.seekBytes[idx], s.data.size() - s.seekBytes[idx], 2048));
    const std::uint64_t drop = target - s.seekFrames[idx];
    SND_REQUIRE(dec.Frames() == s.h.total_frames - s.seekFrames[idx]);
    bool same = true;
    for (std::uint32_t c = 0; c < 2; ++c) {
      for (std::uint64_t i = target; i < s.h.total_frames; ++i) same = same && dec.got[c][i - target + drop] == full[c][i];
    }
    SND_CHECK_MSG(same, "seek to %llu", static_cast<unsigned long long>(target));
  }
}

// E5 + B4: 3+ channel Opus assets warn with the slot they need; 1-2 channel
// ones do not; a sampleRate other than 48000 warns (and fails with --werror).
SND_TEST(IndepOpusBank, WarningsForSlotsAndRates) {
  if (!snd::codec::OpusAvailable()) return;
  for (std::uint32_t ch = 1; ch <= 8; ++ch) {
    mini::Project proj("indep-opus-warn-" + std::to_string(ch));
    Base(proj);
    proj.Wav("audio/a.wav", ToPcm(opusref::Tones(ch, 4800, {500, 700, 900}, 0.2), 48000));
    proj.Add("assets/a.json", "asset", "a",
             "\"file\": \"audio/a.wav\", \"loadForm\": \"compressed\", \"convert\": {\"default\": {\"codec\": \"OPUS\"}}");
    Event(proj, "a");
    bt::BuildOptions o = proj.Options();
    o.encoders = &bt::CodecEncoders();
    bt::Diagnostics d;
    SND_REQUIRE_MSG(proj.Build(o, &d), "%s", mini::Dump(d).c_str());
    // The number in the warning is what the decoder asks for (StateBytes of the built header).
    RawBank bank(mini::ReadBytes(proj.out() / "Main.sndb"));
    const Asset a = ReadAsset(bank, proj.out(), "a");
    SND_REQUIRE(a.error.empty());
    const std::uint32_t need = snd::codec::BuiltinCodec(SND_CODEC_OPUS)->StateBytes(a.h);
    const bool warned = mini::HasDiag(d, bt::Severity::Warning, std::to_string(need));
    SND_CHECK_MSG(warned == (ch >= 3), "%u ch: warned %d\n%s", ch, warned ? 1 : 0, mini::Dump(d).c_str());
    if (ch >= 3) {
      o.warningsAsErrors = true;
      bt::Diagnostics d2;
      SND_CHECK(!proj.Build(o, &d2));
    }
  }
  mini::Project proj("indep-opus-rate");
  Base(proj);
  proj.Wav("audio/a.wav", ToPcm(opusref::Tones(1, 4800, {500}, 0.2), 48000));
  proj.Add("assets/a.json", "asset", "a",
           "\"file\": \"audio/a.wav\", \"loadForm\": \"compressed\", \"convert\": {\"default\": {\"codec\": \"OPUS\", "
           "\"sampleRate\": 32000}}");
  Event(proj, "a");
  bt::BuildOptions o = proj.Options();
  o.encoders = &bt::CodecEncoders();
  bt::Diagnostics d;
  SND_REQUIRE_MSG(proj.Build(o, &d), "%s", mini::Dump(d).c_str());
  SND_CHECK_MSG(mini::HasDiag(d, bt::Severity::Warning, "32000"), "%s", mini::Dump(d).c_str());
  RawBank bank(mini::ReadBytes(proj.out() / "Main.sndb"));
  const Asset a = ReadAsset(bank, proj.out(), "a");
  SND_CHECK(a.error.empty() && a.h.sample_rate == 48000u && a.h.total_frames == 4800u);  // ignored, not resampled to 32 kHz
  o.warningsAsErrors = true;
  bt::Diagnostics d2;
  SND_CHECK(!proj.Build(o, &d2));
}

// I2: Opus settings are layered member by member (platforms.default <
// platforms[platform] < convert.default < convert[platform]); the bank holds
// exactly EncodeOpus(those settings, the WAV samples). Defaults when nothing is
// given: 64000 x ch, complexity 10, VBR, auto, independent frames.
SND_TEST(IndepOpusBank, SettingsAreLayeredPerMember) {
  if (!snd::codec::OpusAvailable()) return;
  mini::Project proj("indep-opus-layers");
  proj.Add("project.json", "project", "P",
           "\"platforms\": {\"default\": {\"opus\": {\"bitrate\": 96000, \"complexity\": 3}}, "
           "\"LNX_\": {\"opus\": {\"complexity\": 4, \"vbr\": \"cbr\"}}}");
  Base(proj);
  const opusref::Planar pcm = opusref::Tones(2, 24000, {300, 800}, 0.3);
  proj.Wav("audio/a.wav", ToPcm(pcm, 48000), bt::WavSampleFormat::Float32);
  proj.Add("assets/a.json", "asset", "a",
           "\"file\": \"audio/a.wav\", \"loadForm\": \"compressed\", \"convert\": {"
           "\"default\": {\"codec\": \"OPUS\", \"opus\": {\"signal\": \"music\", \"vbr\": \"constrained\"}}, "
           "\"LNX_\": {\"opus\": {\"independentFrames\": false}}}");
  proj.Add("assets/b.json", "asset", "b", "\"file\": \"audio/a.wav\", \"loadForm\": \"compressed\", \"convert\": {\"default\": {\"codec\": \"OPUS\"}}");
  Event(proj, "a");
  Event(proj, "b");
  auto expect = [&](const ce::OpusSettings& st) {
    opusref::Encoded e;
    SND_CHECK(opusref::Encode(pcm, st, &e));
    return e.data;
  };
  for (const char* plat : {"LNX_", "WIN_"}) {
    bt::BuildOptions o = proj.Options();
    o.encoders = &bt::CodecEncoders();
    SND_REQUIRE(bt::ParsePlatform(plat, &o.platform));
    bt::Diagnostics d;
    SND_REQUIRE_MSG(proj.Build(o, &d), "%s", mini::Dump(d).c_str());
    RawBank bank(mini::ReadBytes(proj.out() / "Main.sndb"));
    const Asset a = ReadAsset(bank, proj.out(), "a");
    const Asset b = ReadAsset(bank, proj.out(), "b");
    SND_REQUIRE(a.error.empty() && b.error.empty());
    ce::OpusSettings st;
    st.bitrate = 96000;
    st.complexity = std::string(plat) == "LNX_" ? 4u : 3u;
    st.vbr = ce::OpusVbr::ConstrainedVbr;
    st.signal = ce::OpusSignal::Music;
    st.independentFrames = std::string(plat) != "LNX_";
    SND_CHECK_MSG(a.data == expect(st), "%s: asset a is not EncodeOpus(96000, %u, constrained, music, %d)", plat,
                  st.complexity, st.independentFrames ? 1 : 0);
    ce::OpusSettings sb;  // b: only the project layers
    sb.bitrate = 96000;
    sb.complexity = st.complexity;
    sb.vbr = std::string(plat) == "LNX_" ? ce::OpusVbr::Cbr : ce::OpusVbr::Vbr;
    SND_CHECK_MSG(b.data == expect(sb), "%s: asset b", plat);
  }
  // No settings anywhere: the documented defaults.
  mini::Project p2("indep-opus-defaults");
  Base(p2);
  p2.Wav("audio/a.wav", ToPcm(pcm, 48000), bt::WavSampleFormat::Float32);
  p2.Add("assets/a.json", "asset", "a", "\"file\": \"audio/a.wav\", \"loadForm\": \"compressed\", \"convert\": {\"default\": {\"codec\": \"OPUS\"}}");
  Event(p2, "a");
  bt::BuildOptions o = p2.Options();
  o.encoders = &bt::CodecEncoders();
  bt::Diagnostics d;
  SND_REQUIRE_MSG(p2.Build(o, &d), "%s", mini::Dump(d).c_str());
  RawBank bank(mini::ReadBytes(p2.out() / "Main.sndb"));
  const Asset a = ReadAsset(bank, p2.out(), "a");
  ce::OpusSettings def;
  def.bitrate = 128000;
  def.complexity = 10;
  SND_CHECK(a.error.empty() && a.data == expect(def));
}

// I1: ranges of project-format.md "Opus の設定".
SND_TEST(IndepOpusBank, SettingRangesAreBuildErrors) {
  if (!snd::codec::OpusAvailable()) return;
  struct Case {
    const char* opus;
    std::uint32_t ch;
    bool ok;
  };
  const Case cases[] = {
      {"{\"bitrate\": 0}", 1, true},          {"{\"bitrate\": 6000}", 1, true},
      {"{\"bitrate\": 5999}", 1, false},      {"{\"bitrate\": 4080001}", 8, false},
      {"{\"bitrate\": 4080000}", 8, true},    {"{\"bitrate\": 510001}", 1, false},  // > 510000 per channel: encode-time error
      {"{\"complexity\": 0}", 1, true},       {"{\"complexity\": 11}", 1, false},
      {"{\"vbr\": \"cbr\"}", 1, true},        {"{\"vbr\": \"abr\"}", 1, false},
      {"{\"signal\": \"voice\"}", 1, true},   {"{\"signal\": \"speech\"}", 1, false},
      {"{\"independentFrames\": false}", 1, true},
  };
  int i = 0;
  for (const Case& c : cases) {
    mini::Project proj("indep-opus-range-" + std::to_string(i++));
    Base(proj);
    proj.Wav("audio/a.wav", ToPcm(opusref::Tones(c.ch, 1920, {500}, 0.2), 48000));
    proj.Add("assets/a.json", "asset", "a",
             std::string("\"file\": \"audio/a.wav\", \"loadForm\": \"compressed\", \"convert\": {\"default\": {\"codec\": "
                         "\"OPUS\", \"opus\": ") + c.opus + "}}");
    Event(proj, "a");
    bt::BuildOptions o = proj.Options();
    o.encoders = &bt::CodecEncoders();
    bt::Diagnostics d;
    const bool built = proj.Build(o, &d);
    SND_CHECK_MSG(built == c.ok, "%s (%u ch): built %d\n%s", c.opus, c.ch, built ? 1 : 0, mini::Dump(d).c_str());
  }
}

// H1 / H2: every Opus setting is part of the cache key of an Opus asset; other
// codecs' keys ignore the Opus settings (they stay cached).
SND_TEST(IndepOpusBank, CacheKeys) {
  if (!snd::codec::OpusAvailable()) return;
  mini::Project proj("indep-opus-cache");
  Base(proj);
  proj.Wav("audio/a.wav", ToPcm(opusref::Tones(1, 9600, {500}, 0.2), 48000));
  auto project = [&](const std::string& opus) {
    proj.Add("project.json", "project", "P", "\"platforms\": {\"default\": {\"opus\": " + opus + "}}");
  };
  auto assets = [&](const std::string& pcmOpus) {
    proj.Add("assets/o.json", "asset", "o",
             "\"file\": \"audio/a.wav\", \"loadForm\": \"compressed\", \"convert\": {\"default\": {\"codec\": \"OPUS\"}}");
    proj.Add("assets/p.json", "asset", "p",
             "\"file\": \"audio/a.wav\", \"loadForm\": \"compressed\", \"convert\": {\"default\": {\"codec\": \"PC16\", "
             "\"opus\": " + pcmOpus + "}}");
    proj.Add("assets/q.json", "asset", "q",
             "\"file\": \"audio/a.wav\", \"loadForm\": \"compressed\", \"convert\": {\"default\": {\"codec\": \"ADPC\"}}");
  };
  project("{}");
  assets("{}");
  for (const char* n : {"o", "p", "q"}) Event(proj, n);
  bt::BuildOptions o = proj.Options();
  o.encoders = &bt::CodecEncoders();
  o.cacheDir = (proj.root() / "cache").string();
  auto build = [&](bt::BuildResult* r) {
    bt::Diagnostics d;
    const bool ok = proj.Build(o, &d, r);
    SND_CHECK_MSG(ok, "%s", mini::Dump(d).c_str());
    return ok;
  };
  bt::BuildResult r0;
  SND_REQUIRE(build(&r0));
  SND_CHECK_EQ(r0.assetsEncoded, 3u);
  const char* changes[] = {"{\"bitrate\": 48000}",
                           "{\"bitrate\": 48000, \"complexity\": 9}",
                           "{\"bitrate\": 48000, \"complexity\": 9, \"vbr\": \"cbr\"}",
                           "{\"bitrate\": 48000, \"complexity\": 9, \"vbr\": \"constrained\"}",
                           "{\"bitrate\": 48000, \"complexity\": 9, \"vbr\": \"constrained\", \"signal\": \"voice\"}",
                           "{\"bitrate\": 48000, \"complexity\": 9, \"vbr\": \"constrained\", \"signal\": \"music\"}",
                           "{\"bitrate\": 48000, \"complexity\": 9, \"vbr\": \"constrained\", \"signal\": \"music\", "
                           "\"independentFrames\": false}"};
  for (const char* c : changes) {
    project(c);
    bt::BuildResult r;
    SND_REQUIRE(build(&r));
    SND_CHECK_MSG(r.assetsEncoded == 1u && r.assetsFromCache == 2u, "%s: encoded %u cached %u", c, r.assetsEncoded,
                  r.assetsFromCache);
  }
  // Unchanged settings: everything from the cache.
  bt::BuildResult same;
  SND_REQUIRE(build(&same));
  SND_CHECK_EQ(same.assetsEncoded, 0u);
  // Opus settings on a PC16 asset are not part of its key.
  assets("{\"bitrate\": 24000, \"complexity\": 1}");
  bt::BuildResult r2;
  SND_REQUIRE(build(&r2));
  SND_CHECK_MSG(r2.assetsEncoded == 0u, "PC16 asset re-encoded for Opus settings (%u)", r2.assetsEncoded);
}

// G1: a build tool without Opus rejects OPUS assets.
SND_TEST(IndepOpusBank, OffBuildRejectsOpusAssets) {
  if (snd::codec::OpusAvailable()) return;
  SND_CHECK(bt::CodecEncoders().Find(SND_CODEC_OPUS) == nullptr);
  mini::Project proj("indep-opus-off");
  Base(proj);
  proj.Wav("audio/a.wav", ToPcm(opusref::Tones(1, 960, {500}, 0.2), 48000));
  proj.Add("assets/a.json", "asset", "a", "\"file\": \"audio/a.wav\", \"convert\": {\"default\": {\"codec\": \"OPUS\"}}");
  Event(proj, "a");
  bt::BuildOptions o = proj.Options();
  o.encoders = &bt::CodecEncoders();
  bt::Diagnostics d;
  SND_CHECK(!proj.Build(o, &d));
  SND_CHECK(d.items().size() > 0);
  SND_CHECK(!mini::fs::exists(proj.out() / "Main.sndb"));
}
