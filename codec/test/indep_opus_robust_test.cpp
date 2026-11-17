// Independent tests: RT safety, stack and broken input of the Opus decoder
// (CONTRACTS.md D-47, D-51, D-58, D-59, bank-format.md §6.5).
// Properties: scratchpad indep-opus-properties.md E1, E2, F1, F4.
#include <pthread.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#include "indep_opus_ref.hpp"
#include "snd/codec/codec.hpp"
#include "snd/codec/encode.hpp"
#include "snd/codec/registry.hpp"
#include "snd/internal/rt_check.hpp"
#include "snd/test/rt_guard.hpp"
#include "snd/test/test.hpp"

namespace ce = snd::codec::encode;
using opusref::Encoded;
using opusref::Planar;

namespace {

std::uint64_t Lcg(std::uint64_t& s) {
  s = s * 6364136223846793005ull + 1442695040888963407ull;
  return s >> 33;
}

Planar Music(std::uint32_t ch, std::uint64_t frames, std::uint64_t seed) {
  Planar p(ch, std::vector<float>(static_cast<std::size_t>(frames)));
  std::uint64_t s = seed;
  for (std::uint32_t c = 0; c < ch; ++c) {
    for (std::uint64_t i = 0; i < frames; ++i) {
      const double t = static_cast<double>(i) / 48000.0;
      double v = 0.3 * std::sin(opusref::kTwoPi * (180.0 + 90.0 * c) * t) * (0.5 + 0.5 * std::sin(opusref::kTwoPi * 3 * t));
      v += 0.05 * (static_cast<double>(Lcg(s) % 2001u) / 1000.0 - 1.0);
      p[c][static_cast<std::size_t>(i)] = static_cast<float>(v);
    }
  }
  return p;
}

// Decodes `d` with the decode protocol; returns false on SND_DECODE_ERROR. All
// output must be finite (broken data must not reach feedback DSP).
struct Outcome {
  bool error = false;
  bool finite = true;
  bool overMax = false;
  std::uint64_t frames = 0;
};
Outcome Run(const snd::CodecHeader& h, const std::uint8_t* d, std::size_t bytes, std::size_t chunk,
            std::uint32_t max, opusref::State& mem) {
  snd::ICodec* c = snd::codec::BuiltinCodec(SND_CODEC_OPUS);
  c->Init(mem.get(), h);
  std::vector<std::vector<float>> buf(h.channels == 0 || h.channels > 8 ? 8 : h.channels,
                                      std::vector<float>(max + 1u));
  std::vector<float*> ptr;
  for (auto& b : buf) ptr.push_back(b.data());
  Outcome o;
  auto call = [&](const std::uint8_t* src, std::uint32_t n) {
    const std::uint32_t r = c->Decode(mem.get(), src, n, ptr.data(), max);
    if (r == snd::kDecodeError) {
      o.error = true;
      return r;
    }
    if (r > max) {
      o.overMax = true;
      return snd::kDecodeError;
    }
    for (auto& b : buf) {
      for (std::uint32_t i = 0; i < r; ++i) o.finite = o.finite && std::isfinite(b[i]);
    }
    o.frames += r;
    return r;
  };
  for (std::size_t off = 0; off < bytes; off += chunk) {
    const std::uint32_t n = static_cast<std::uint32_t>(std::min(chunk, bytes - off));
    std::uint32_t r = call(d + off, n);
    while (r == max) r = call(nullptr, 0);
    if (r == snd::kDecodeError) return o;
  }
  std::uint32_t r = call(nullptr, 0);
  while (r == max) r = call(nullptr, 0);
  return o;
}

struct ThreadJob {
  const Encoded* e;
  std::uint64_t frames = 0;
  bool done = false;
};

void* DecodeJob(void* arg) {
  auto* j = static_cast<ThreadJob*>(arg);
  opusref::Dec d(j->e->h, 960);
  if (d.FeedAll(j->e->data.data(), j->e->data.size(), 4096)) j->frames = d.Frames();
  j->done = true;
  return nullptr;
}

}  // namespace

// E1 (D-59): Init / Decode / Seek / Find / ValidateHeader inside SND_RT_SCOPE do
// not allocate, lock or do IO - for 1..8 channels, small pieces, seeks, and the
// error paths (corrupt data).
SND_TEST(IndepOpusRobust, RtScopeHasNoViolations) {
  if (!snd::codec::OpusAvailable()) return;
  std::vector<Encoded> enc(8);
  for (std::uint32_t ch = 1; ch <= 8; ++ch) SND_REQUIRE(opusref::Encode(Music(ch, 9600, ch), ce::OpusSettings{}, &enc[ch - 1]));
  Encoded voice;
  ce::OpusSettings vs;
  vs.bitrate = 12000;
  vs.signal = ce::OpusSignal::Voice;
  SND_REQUIRE(opusref::Encode(Music(1, 9600, 99), vs, &voice));
  enc.push_back(voice);
  snd::ICodec* c = snd::codec::BuiltinCodec(SND_CODEC_OPUS);
  snd::codec::CodecRegistry reg;
  std::uint32_t biggest = 0;
  for (const auto& e : enc) biggest = std::max(biggest, c->StateBytes(e.h));
  opusref::State mem(biggest);
  std::vector<std::vector<float>> buf(8, std::vector<float>(300));
  std::vector<float*> ptr;
  for (auto& b : buf) ptr.push_back(b.data());
  std::vector<std::vector<std::uint8_t>> broken;
  for (const auto& e : enc) {
    std::vector<std::uint8_t> b = e.data;
    for (std::size_t i = 40; i < b.size(); i += 97) b[i] ^= 0x5A;
    broken.push_back(b);
  }
  std::uint64_t frames = 0, errors = 0;
  snd::test::rt::Reset();
  {
    SND_RT_SCOPE();
    for (std::size_t k = 0; k < enc.size(); ++k) {
      const Encoded& e = enc[k];
      SND_CHECK(reg.Find(SND_CODEC_OPUS) == c);
      (void)snd::codec::ValidateHeader(e.h);
      (void)c->StateBytes(e.h);
      for (int pass = 0; pass < 3; ++pass) {
        c->Init(mem.get(), e.h);
        const std::vector<std::uint8_t>& data = pass == 2 ? broken[k] : e.data;
        std::size_t off = 0;
        if (pass == 1) {  // seek to the third packet
          const std::uint32_t l0 = data[0] | (data[1] << 8);
          const std::uint32_t l1 = data[2 + l0] | (data[3 + l0] << 8);
          off = 4u + l0 + l1;
          c->Seek(mem.get(), 2u * 960u - 312u);
        }
        while (off < data.size()) {
          const std::uint32_t n = static_cast<std::uint32_t>(std::min<std::size_t>(333, data.size() - off));
          std::uint32_t r = c->Decode(mem.get(), data.data() + off, n, ptr.data(), 300);
          off += n;
          while (r == 300u) {
            frames += r;
            r = c->Decode(mem.get(), nullptr, 0, ptr.data(), 300);
          }
          if (r == snd::kDecodeError) {
            ++errors;
            break;
          }
          frames += r;
        }
        c->Seek(mem.get(), 0);
      }
    }
  }
  SND_CHECK_EQ(snd::test::rt::UnexpectedTotal(), 0u);
  SND_CHECK(frames > 9u * 9600u);
  std::printf("  rt scope: %llu frames, %llu error paths, libc hooks %s\n", static_cast<unsigned long long>(frames),
              static_cast<unsigned long long>(errors), snd::test::rt::LibcHooksActive() ? "on" : "off");
}

// E2 (D-47): decoding 8 channels at the top bitrate works on a thread whose
// whole stack is the RT minimum of 256 KiB (all configurations, sanitizers too).
SND_TEST(IndepOpusRobust, DecodesOn256KiBStack) {
  if (!snd::codec::OpusAvailable()) return;
  for (std::uint32_t ch : {1u, 2u, 8u}) {
    ce::OpusSettings st;
    st.bitrate = 510000u * ch;
    Encoded e;
    SND_REQUIRE(opusref::Encode(Music(ch, 19200, ch + 10), st, &e));
    ThreadJob job{&e};
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    SND_REQUIRE(pthread_attr_setstacksize(&attr, 256u * 1024u) == 0);
    pthread_t th;
    SND_REQUIRE(pthread_create(&th, &attr, &DecodeJob, &job) == 0);
    pthread_join(th, nullptr);
    pthread_attr_destroy(&attr);
    SND_CHECK(job.done);
    SND_CHECK_MSG(job.frames == 19200u, "%u ch: %llu frames", ch, static_cast<unsigned long long>(job.frames));
  }
}

// F1: broken length prefixes. Zero and too long are errors; a length that runs
// past the data is an incomplete packet (held, no crash, nothing invented).
SND_TEST(IndepOpusRobust, BrokenLengthPrefixes) {
  if (!snd::codec::OpusAvailable()) return;
  for (std::uint32_t ch : {1u, 2u, 6u}) {
    Encoded e;
    SND_REQUIRE(opusref::Encode(Music(ch, 9600, 3), ce::OpusSettings{}, &e));
    const std::uint32_t streams = e.setup[2];
    opusref::State mem(snd::codec::BuiltinCodec(SND_CODEC_OPUS)->StateBytes(e.h));
    // Offset of the third packet's prefix.
    const std::uint32_t l0 = e.data[0] | (e.data[1] << 8);
    const std::uint32_t l1 = e.data[2 + l0] | (e.data[3 + l0] << 8);
    const std::size_t p2 = 4u + l0 + l1;
    for (std::uint32_t bad : {0u, streams * 1277u + 1u, 0xFFFFu}) {
      std::vector<std::uint8_t> d = e.data;
      d[p2] = static_cast<std::uint8_t>(bad & 0xFF);
      d[p2 + 1] = static_cast<std::uint8_t>(bad >> 8);
      for (std::size_t chunk : {std::size_t{1}, std::size_t{100}, d.size()}) {
        const Outcome o = Run(e.h, d.data(), d.size(), chunk, 4096, mem);
        SND_CHECK_MSG(o.error, "%u ch: length %u (chunk %zu) accepted", ch, bad, chunk);
        SND_CHECK(!o.overMax && o.finite);
        SND_CHECK_MSG(o.frames <= 2u * 960u - 312u, "%u ch: %llu frames before the bad packet", ch,
                      static_cast<unsigned long long>(o.frames));
        SND_CHECK(mem.Intact());
      }
    }
    // The largest legal length (streams x 1277) is accepted by the framing: the
    // packet runs past the data, so it is held and nothing more comes out.
    {
      std::vector<std::uint8_t> d(e.data.begin(), e.data.begin() + static_cast<std::ptrdiff_t>(p2 + 2 + 10));
      const std::uint32_t big = streams * 1277u;
      d[p2] = static_cast<std::uint8_t>(big & 0xFF);
      d[p2 + 1] = static_cast<std::uint8_t>(big >> 8);
      const Outcome o = Run(e.h, d.data(), d.size(), 64, 4096, mem);
      SND_CHECK(!o.error && o.finite);
      SND_CHECK_EQ(o.frames, 2u * 960u - 312u);
    }
    // Truncations at every byte of the first 3 packets: never an error, never more
    // than the complete packets give.
    for (std::size_t cut = 0; cut <= p2 + 2; ++cut) {
      const Outcome o = Run(e.h, e.data.data(), cut, 7, 512, mem);
      SND_CHECK(!o.error && o.finite);
      std::uint64_t complete = 0;
      if (cut >= 2u + l0) complete = 960u - 312u;
      if (cut >= 4u + l0 + l1) complete = 2u * 960u - 312u;
      SND_CHECK_MSG(o.frames == complete, "%u ch: cut %zu gave %llu frames (want %llu)", ch, cut,
                    static_cast<unsigned long long>(o.frames), static_cast<unsigned long long>(complete));
    }
  }
}

// F4: random bit flips, byte garbage and packets of the wrong duration. The
// decoder never crashes (ASan / UBSan in the sanitizer build), never writes past
// its state or max_frames, and never outputs non-finite samples.
SND_TEST(IndepOpusRobust, RandomCorruptionFuzz) {
  if (!snd::codec::OpusAvailable()) return;
  std::vector<Encoded> enc(4);
  SND_REQUIRE(opusref::Encode(Music(1, 9600, 1), ce::OpusSettings{}, &enc[0]));
  SND_REQUIRE(opusref::Encode(Music(2, 9600, 2), ce::OpusSettings{}, &enc[1]));
  SND_REQUIRE(opusref::Encode(Music(6, 4800, 3), ce::OpusSettings{}, &enc[2]));
  ce::OpusSettings vs;
  vs.bitrate = 16000;
  vs.signal = ce::OpusSignal::Voice;
  SND_REQUIRE(opusref::Encode(Music(1, 9600, 4), vs, &enc[3]));
  std::uint64_t s = 12345, errors = 0, runs = 0;
  for (int iter = 0; iter < 600; ++iter) {
    const Encoded& e = enc[iter % 4];
    std::vector<std::uint8_t> d = e.data;
    const std::uint32_t kind = static_cast<std::uint32_t>(Lcg(s) % 5u);
    if (kind == 0) {  // a few bit flips anywhere (prefixes included)
      for (int k = 0; k < 1 + static_cast<int>(Lcg(s) % 8u); ++k) d[Lcg(s) % d.size()] ^= static_cast<std::uint8_t>(1u << (Lcg(s) % 8u));
    } else if (kind == 1) {  // payload-only garbage: keep the prefixes valid
      std::size_t off = 0;
      while (off + 2 <= d.size()) {
        const std::uint32_t len = d[off] | (d[off + 1] << 8);
        if (Lcg(s) % 3u == 0u) {
          for (std::uint32_t i = 0; i < len && off + 2 + i < d.size(); ++i) d[off + 2 + i] = static_cast<std::uint8_t>(Lcg(s));
        }
        off += 2u + len;
      }
    } else if (kind == 2) {  // TOC rewritten to other durations / codes
      std::size_t off = 0;
      while (off + 2 < d.size()) {
        const std::uint32_t len = d[off] | (d[off + 1] << 8);
        if (Lcg(s) % 2u == 0u) d[off + 2] = static_cast<std::uint8_t>(Lcg(s));
        off += 2u + len;
      }
    } else if (kind == 3) {  // truncated anywhere
      d.resize(Lcg(s) % d.size());
    } else {  // all random bytes
      for (auto& b : d) b = static_cast<std::uint8_t>(Lcg(s));
    }
    opusref::State mem(snd::codec::BuiltinCodec(SND_CODEC_OPUS)->StateBytes(e.h));
    const std::size_t chunk = 1u + Lcg(s) % 5000u;
    const std::uint32_t max = 1u + static_cast<std::uint32_t>(Lcg(s) % 2000u);
    const Outcome o = Run(e.h, d.data(), d.size(), chunk, max, mem);
    ++runs;
    errors += o.error ? 1u : 0u;
    SND_CHECK_MSG(!o.overMax, "iter %d: returned more than max_frames", iter);
    SND_CHECK_MSG(o.finite, "iter %d kind %u: non-finite output", iter, kind);
    SND_CHECK_MSG(o.frames <= e.h.total_frames, "iter %d: %llu frames > total", iter,
                  static_cast<unsigned long long>(o.frames));
    SND_CHECK_MSG(mem.Intact(), "iter %d: state guard overwritten", iter);
  }
  std::printf("  fuzz: %llu runs, %llu ended in SND_DECODE_ERROR\n", static_cast<unsigned long long>(runs),
              static_cast<unsigned long long>(errors));
  SND_CHECK(errors > 0u);
}

// F2 at run time: a header whose setup disagrees with the data (claims more
// streams than the packets carry, or a different coupling) fails cleanly.
SND_TEST(IndepOpusRobust, SetupThatDoesNotMatchTheData) {
  if (!snd::codec::OpusAvailable()) return;
  Encoded e;
  SND_REQUIRE(opusref::Encode(Music(2, 4800, 8), ce::OpusSettings{}, &e));  // N 1, M 1
  struct Alt {
    std::vector<std::uint8_t> setup;
    bool mustDecode;
  };
  const Alt alts[] = {
      // Two streams over one-stream data: usually malformed; either outcome is fine,
      // but it must not crash or produce non-finite output.
      {{1, 2, 2, 0, 0, 1}, false},
      {{1, 2, 2, 2, 0, 3}, false},
      {{1, 2, 1, 0, 0, 0}, true},  // one mono stream: a stereo packet decodes as mono (valid Opus)
      {{1, 2, 1, 1, 1, 0}, true},  // swapped channels
      {{1, 2, 1, 1, 255, 1}, true},
  };
  for (const Alt& a : alts) {
    snd::CodecHeader h = e.h;
    h.extra = a.setup.data();
    h.extra_bytes = static_cast<std::uint32_t>(a.setup.size());
    SND_REQUIRE(snd::codec::ValidateHeader(h) == snd::codec::HeaderError::None);
    opusref::State mem(snd::codec::BuiltinCodec(SND_CODEC_OPUS)->StateBytes(h));
    const Outcome o = Run(h, e.data.data(), e.data.size(), 1000, 960, mem);
    if (a.mustDecode) SND_CHECK_MSG(!o.error && o.frames == 4800u, "setup N %u M %u: error %d frames %llu", a.setup[2],
                                   a.setup[3], o.error ? 1 : 0, static_cast<unsigned long long>(o.frames));
    SND_CHECK(o.finite && mem.Intact());
  }
  // 255 = silence: that output channel is exactly zero, the other carries channel 1.
  {
    const std::vector<std::uint8_t> st = {1, 2, 1, 1, 255, 1};
    snd::CodecHeader h = e.h;
    h.extra = st.data();
    h.extra_bytes = 6;
    opusref::Dec d(h, 4096);
    SND_REQUIRE(d.FeedAll(e.data.data(), e.data.size(), 4096));
    bool ok = false;
    const Planar normal = opusref::DecodeAll(e, &ok);
    SND_REQUIRE(ok);
    bool zero = true;
    for (float v : d.got[0]) zero = zero && v == 0.0f;
    SND_CHECK(zero);
    SND_CHECK(d.got[1] == normal[1]);
  }
}
