// Fuzz (design.md 14): random mutations, truncations and huge lengths fed to
// the frame reader, the validators, the record reader and the replayer. Must
// never crash or read out of bounds (run under ASan); every result must be a
// clean accept / reject. Deterministic seeds; SND_FUZZ_ITERATIONS scales it.
#include <cstdlib>
#include <cstring>
#include <random>
#include <vector>

#include "snd/base/allocator.hpp"
#include "snd/core/core.hpp"
#include "snd/format/bank_format.hpp"
#include "snd/livelink/frame_stream.hpp"
#include "snd/livelink/record_format.hpp"
#include "snd/livelink/replayer.hpp"
#include "snd/livelink/wire.hpp"
#include "snd/test/test.hpp"

namespace ll = snd::livelink;
namespace bf = snd::bank_format;

namespace {

std::uint32_t Iterations(std::uint32_t base) {
  const char* e = std::getenv("SND_FUZZ_ITERATIONS");
  return e != nullptr ? static_cast<std::uint32_t>(std::strtoul(e, nullptr, 10)) : base;
}

// Exercises every decoder on a frame the way the server / client would.
struct Decode final : ll::FrameReader::Handler {
  std::uint64_t frames = 0, oversized = 0, accepted = 0;
  void OnFrame(const ll::FrameHeader& h, const std::uint8_t* p) noexcept override {
    ++frames;
    const std::uint32_t n = h.payloadBytes;
    snd_result r = SND_OK;
    const std::uint8_t* data = nullptr;
    ll::ListView lv;
    switch (static_cast<ll::MessageType>(h.type)) {
      case ll::MessageType::Hello: r = ll::CheckHello(ll::ReadFixed<ll::Hello>(p, n)); break;
      case ll::MessageType::SyncDecision: r = ll::CheckSyncDecision(ll::ReadFixed<ll::SyncDecision>(p, n)); break;
      case ll::MessageType::SetValue: r = ll::CheckSetValue(ll::ReadFixed<ll::SetValue>(p, n)); break;
      case ll::MessageType::Audition: r = ll::CheckAudition(ll::ReadFixed<ll::Audition>(p, n)); break;
      case ll::MessageType::OverrideParam: r = ll::CheckOverrideParam(ll::ReadFixed<ll::OverrideParam>(p, n)); break;
      case ll::MessageType::OverrideState: r = ll::CheckOverrideState(ll::ReadFixed<ll::OverrideState>(p, n)); break;
      case ll::MessageType::SetProfilerMask: r = ll::CheckSetProfilerMask(ll::ReadFixed<ll::SetProfilerMask>(p, n)); break;
      case ll::MessageType::RecordControl: r = ll::CheckRecordControl(ll::ReadFixed<ll::RecordControl>(p, n)); break;
      case ll::MessageType::StructEdit: {
        const ll::StructEdit m = ll::ReadFixed<ll::StructEdit>(p, n);
        r = ll::CheckStructEdit(m);
        if (r == SND_OK && !ll::TrailingData(p, n, sizeof(m), std::uint64_t{m.argCount} * 8u, &data)) r = SND_ERR_INVALID_ARGUMENT;
        if (r == SND_OK && m.argCount != 0u) {
          volatile std::uint8_t sink = data[m.argCount * 8u - 1u];  // last argument byte is inside
          (void)sink;
        }
        break;
      }
      case ll::MessageType::ReplaceChunk: {
        const ll::ReplaceChunk m = ll::ReadFixed<ll::ReplaceChunk>(p, n);
        std::uint32_t rows = 0;
        r = ll::TrailingData(p, n, sizeof(m), m.bytes, &data) ? ll::CheckTableChunk(m.fourcc, data, m.bytes, &rows)
                                                              : SND_ERR_INVALID_ARGUMENT;
        break;
      }
      case ll::MessageType::VoiceList:
      case ll::MessageType::EventLog:
      case ll::MessageType::BusMeters:
        if (ll::ParseList(p, n, 1u << 20, &lv)) {
          for (std::uint32_t i = 0; i < lv.count; ++i) (void)lv.At<ll::MeterEntry>(i);
        }
        break;
      case ll::MessageType::RecordData: {
        const ll::RecordData m = ll::ReadFixed<ll::RecordData>(p, n);
        if (ll::TrailingData(p, n, sizeof(m), m.bytes, &data)) {
          ll::RecordReader rr(data, m.bytes);
          ll::RecordItem it;
          while (rr.Next(&it)) {
          }
        }
        break;
      }
      default:
        break;
    }
    if (r == SND_OK) ++accepted;
  }
  void OnOversized(const ll::FrameHeader&) noexcept override { ++oversized; }
};

std::vector<std::uint8_t> Frame(std::uint16_t type, const std::vector<std::uint8_t>& p) {
  std::vector<std::uint8_t> b(ll::kFrameHeaderBytes + p.size());
  ll::EncodeHeader(b.data(), type, 0, static_cast<std::uint32_t>(p.size()), 1);
  if (!p.empty()) std::memcpy(b.data() + ll::kFrameHeaderBytes, p.data(), p.size());
  return b;
}

template <class T>
std::vector<std::uint8_t> Bytes(const T& v) {
  std::vector<std::uint8_t> b(sizeof(T));
  std::memcpy(b.data(), &v, sizeof(T));
  return b;
}

std::vector<std::vector<std::uint8_t>> Seeds() {
  std::vector<std::vector<std::uint8_t>> s;
  ll::Hello h{};
  h.protocolMajor = 1;
  h.role = 1;
  s.push_back(Frame(0x0001, Bytes(h)));
  ll::SetValue v{};
  v.chunk = bf::chunk::BUSS;
  v.column = bf::col::BUSS_volumeDb;
  s.push_back(Frame(0x0010, Bytes(v)));
  std::vector<std::uint8_t> rc(16 + 128, 0);
  const ll::ReplaceChunk rch{1, bf::chunk::BUSS, 128};
  std::memcpy(rc.data(), &rch, 16);
  const bf::TableChunkHeader th{1, 1, 0, 0};
  const bf::ColumnEntry ce{static_cast<std::uint16_t>(bf::col::BUSS_volumeDb), 4, 0, 64};
  std::memcpy(rc.data() + 16, &th, 16);
  std::memcpy(rc.data() + 32, &ce, 16);
  s.push_back(Frame(0x0011, rc));
  std::vector<std::uint8_t> se(16 + 16, 0);
  const ll::StructEdit seh{1, 2, 2, 0};
  std::memcpy(se.data(), &seh, 16);
  s.push_back(Frame(0x0018, se));
  std::vector<std::uint8_t> vl(8 + 64, 0);
  const ll::ListHeader lh{2, 32};
  std::memcpy(vl.data(), &lh, 8);
  s.push_back(Frame(0x0100, vl));
  std::vector<std::uint8_t> rd(16 + 16, 0);
  const ll::RecordData rdh{0, 16};
  const ll::RecordLod lod{2, 0, 5};
  std::memcpy(rd.data(), &rdh, 16);
  std::memcpy(rd.data() + 16, &lod, 16);
  s.push_back(Frame(0x0107, rd));
  return s;
}

}  // namespace

SND_TEST(LivelinkFuzz, FrameStreamMutations) {
  std::mt19937 rng(20261117u);
  const auto seeds = Seeds();
  ll::FrameReader reader;
  SND_REQUIRE(reader.Init(snd::base::DefaultAllocator(), 1u << 16));
  Decode d;
  std::uint64_t badHeaders = 0;
  const std::uint32_t iters = Iterations(20000);
  for (std::uint32_t i = 0; i < iters; ++i) {
    std::vector<std::uint8_t> f = seeds[rng() % seeds.size()];
    switch (rng() % 6) {
      case 0:
        for (int k = 0; k < 1 + static_cast<int>(rng() % 8); ++k) f[rng() % f.size()] ^= static_cast<std::uint8_t>(rng());
        break;
      case 1:  // truncate (the reader then waits for more: reset afterwards)
        f.resize(rng() % f.size());
        break;
      case 2: {  // huge declared length (header only)
        const std::uint32_t len = rng() % 2 ? 0xFFFFFFFFu : ll::kMaxPayloadBytes - (rng() % 4);
        std::memcpy(f.data() + 8, &len, 4);
        f.resize(16 + (rng() % 64));
        break;
      }
      case 3: {  // payload length smaller / larger than the struct
        const std::uint32_t len = static_cast<std::uint32_t>(f.size() - 16);
        const std::uint32_t nl = rng() % (len + 8);
        f.resize(16 + nl, 0xAB);
        std::memcpy(f.data() + 8, &nl, 4);
        break;
      }
      case 4:  // random garbage
        f.resize(rng() % 128);
        for (auto& b : f) b = static_cast<std::uint8_t>(rng());
        break;
      default: {  // poke 32-bit fields inside the payload with extreme values
        static const std::uint32_t kv[] = {0u, 1u, 0x7FFFFFFFu, 0x80000000u, 0xFFFFFFFFu, 0x7FC00000u};
        for (std::size_t at = 16; at + 4 <= f.size(); at += 4) {
          if (rng() % 3 == 0) std::memcpy(f.data() + at, &kv[rng() % 6], 4);
        }
      }
    }
    // Feed in random splits.
    std::size_t at = 0;
    while (at < f.size()) {
      const std::size_t n = 1 + rng() % (f.size() - at);
      if (reader.Feed(f.data() + at, n, d) != ll::HeaderStatus::Ok) {
        ++badHeaders;
        reader.Reset();
        break;
      }
      at += n;
    }
    if (!reader.AtBoundary() && rng() % 2 == 0) reader.Reset();
  }
  SND_CHECK(d.frames > iters / 4);
  SND_CHECK(d.accepted > 0u);
  SND_CHECK(badHeaders > 0u);
}

SND_TEST(LivelinkFuzz, RecordReaderAndReplayerSurviveMutations) {
  snd_config cfg;
  snd_config_init(&cfg);
  cfg.max_playbacks = 64;
  cfg.max_emitters = 16;
  cfg.max_transports = 4;
  cfg.max_user_buffers = 4;
  cfg.max_banks = 8;
  // A valid stream: Lod, BankMap, Arena (two commands), Stall, Arena with a fallback.
  std::vector<std::uint8_t> s;
  auto put = [&](const void* p, std::size_t n) {
    const auto* b = static_cast<const std::uint8_t*>(p);
    s.insert(s.end(), b, b + n);
  };
  const ll::RecordLod lod{2, 0, 100};
  put(&lod, 16);
  const ll::RecordBankMap bm{4, 7, 99};
  put(&bm, 16);
  std::vector<std::uint8_t> cmds(snd::cmd::kWireSize<snd::cmd::SetParam> + snd::cmd::kWireSize<snd::cmd::Stop>, 0);
  const snd::cmd::CommandHeader h1{static_cast<std::uint16_t>(snd::cmd::CommandType::SetParam),
                                   static_cast<std::uint16_t>(snd::cmd::kWireSize<snd::cmd::SetParam>), 0};
  const snd::cmd::CommandHeader h2{static_cast<std::uint16_t>(snd::cmd::CommandType::Stop),
                                   static_cast<std::uint16_t>(snd::cmd::kWireSize<snd::cmd::Stop>), 1};
  std::memcpy(cmds.data(), &h1, 8);
  std::memcpy(cmds.data() + snd::cmd::kWireSize<snd::cmd::SetParam>, &h2, 8);
  const ll::RecordArena ra{1, 0, 101, static_cast<std::uint32_t>(16 + cmds.size()), 0};
  const ll::ArenaSections sec{static_cast<std::uint32_t>(cmds.size()), 0, 0, 0};
  put(&ra, 24);
  put(&sec, 16);
  put(cmds.data(), cmds.size());
  const ll::RecordStall st{3, 1, 102};
  put(&st, 16);
  const std::uint32_t words = (64 + 63) / 64 + 1 + 1 + 1 + 1;
  const std::uint32_t fbBytes = 32 + words * 8 + 24;
  const ll::RecordArena rb{1, 1, 103, 16 + fbBytes, 0};
  const ll::ArenaSections sb{0, 0, fbBytes, snd::cmd::kArenaHasFallback};
  put(&rb, 24);
  put(&sb, 16);
  const ll::FallbackHeader fh{64, 16, 4, 4, 8, 1, 0, 0};
  put(&fh, 32);
  std::vector<std::uint8_t> zero(words * 8 + 24, 0);
  zero[0] = 1;  // stop playback slot 0
  put(zero.data(), zero.size());
  {
    ll::RecordReader rr(s.data(), s.size());
    ll::RecordItem it;
    int n = 0;
    while (rr.Next(&it)) ++n;
    SND_REQUIRE(rr.Error() == ll::RecordError::None);
    SND_REQUIRE_EQ(n, 5);
    ll::ReplayDesc rd;
    rd.config = &cfg;
    rd.allocator = &snd::base::DefaultAllocator();
    rd.record = s.data();
    rd.bytes = s.size();
    ll::Replayer* rp = nullptr;
    SND_REQUIRE_EQ(ll::Replayer::Create(rd, &rp), SND_OK);
    SND_CHECK_EQ(rp->Info().arenas, 2u);
    SND_CHECK_EQ(rp->Info().firstBlock, 100u);
    SND_CHECK_EQ(rp->Blocks(), 4u);
    ll::Replayer::Destroy(rp);
  }
  std::mt19937 rng(77u);
  std::uint32_t accepted = 0, rejected = 0;
  const std::uint32_t iters = Iterations(20000);
  for (std::uint32_t i = 0; i < iters; ++i) {
    std::vector<std::uint8_t> m = s;
    switch (rng() % 4) {
      case 0:
        for (int k = 0; k < 1 + static_cast<int>(rng() % 6); ++k) m[rng() % m.size()] ^= static_cast<std::uint8_t>(rng());
        break;
      case 1:
        m.resize(rng() % m.size());
        break;
      case 2: {
        static const std::uint32_t kv[] = {0u, 0xFFFFFFFFu, 0x7FFFFFF8u, 8u, 0x10000000u};
        const std::size_t at = 4u * (rng() % (m.size() / 4));
        std::memcpy(m.data() + at, &kv[rng() % 5], 4);
        break;
      }
      default:
        m.insert(m.begin() + static_cast<std::ptrdiff_t>(rng() % m.size()), static_cast<std::uint8_t>(rng()));
    }
    ll::RecordReader rr(m.data(), m.size());
    ll::RecordItem it;
    while (rr.Next(&it)) {
    }
    ll::ReplayDesc rd;
    rd.config = &cfg;
    rd.allocator = &snd::base::DefaultAllocator();
    rd.record = m.data();
    rd.bytes = m.size();
    ll::Replayer* rp = nullptr;
    if (ll::Replayer::Create(rd, &rp) == SND_OK) {
      ++accepted;
      // Walk it as the audio thread would.
      rp->Start();
      snd::core::LiveArenaView v{};
      for (std::uint64_t b = 0; b < rp->Blocks() + 2u && b < 4096u; ++b) {
        while (rp->Next(1000 + b, &v)) {
          snd::cmd::ForEachCommand(v.normal, v.normal + v.normalBytes, [](const snd::cmd::CommandHeader&, const std::uint8_t*) {});
        }
      }
      ll::Replayer::Destroy(rp);
    } else {
      ++rejected;
    }
  }
  SND_CHECK(accepted > 0u);
  SND_CHECK(rejected > 0u);
}
