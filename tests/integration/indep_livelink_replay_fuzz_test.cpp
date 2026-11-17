// Independent tests: replay files are external input (bug reports, design.md 11.3).
// "Create parses and validates the whole recording (malformed input is rejected,
// never crashes)" (replayer.hpp). Records are built here from record_format.hpp:
// structurally valid arenas whose commands carry boundary / random field values
// (slots out of range, NaN, unknown enums ...), fallback sections with random
// bitsets and parameters, and truncated / reordered / mismatched variants. Run
// under ASan + UBSan: replaying must never touch memory out of bounds.
// Kept in its own executable: a crash here must not hide the other live link tests.
#include <algorithm>
#include <cstdlib>
#include <limits>
#include <random>
#include <set>

#include "indep_livelink_common.hpp"
#include "snd/livelink/record_format.hpp"

using namespace indep;
using lltest::Fixture;
using lltest::Hooks;

namespace {

std::uint32_t FuzzIterations(std::uint32_t def) {
  const char* e = std::getenv("SND_FUZZ_ITERATIONS");
  if (e == nullptr) return def;
  const long v = std::strtol(e, nullptr, 10);
  return v > 0 ? static_cast<std::uint32_t>(v) : def;
}

template <class T>
void Put(std::vector<std::uint8_t>& b, const T& v) {
  const auto* p = reinterpret_cast<const std::uint8_t*>(&v);
  b.insert(b.end(), p, p + sizeof(T));
}

struct Gen {
  std::mt19937 rng;
  std::set<std::uint16_t> excluded;  // command types not generated
  explicit Gen(std::uint32_t seed) : rng(seed) {}
  std::uint32_t R() { return static_cast<std::uint32_t>(rng()); }
  std::uint32_t Word() {
    static const std::uint32_t kWords[] = {0u,
                                           1u,
                                           2u,
                                           3u,
                                           63u,
                                           64u,
                                           65u,
                                           255u,
                                           0xFFFFu,
                                           0x7FFFFFFFu,
                                           0x80000000u,
                                           0xFFFFFFFFu,
                                           0x7FC00000u /* NaN */,
                                           0x7F800000u /* +Inf */,
                                           0xFF800000u /* -Inf */,
                                           0x80000000u /* -0 */,
                                           0x3F800000u /* 1.0f */,
                                           0xC2C80000u /* -100.0f */,
                                           0x7F7FFFFFu /* FLT_MAX */,
                                           HashId("Tone"),
                                           HashId("Blip"),
                                           HashId("Sfx"),
                                           HashId("Master"),
                                           HashId("Knob"),
                                           lltest::kBankId};
    return R() % 3u == 0u ? R() : kWords[R() % (sizeof(kWords) / sizeof(kWords[0]))];
  }
  void Fill(std::uint8_t* p, std::size_t n) {
    for (std::size_t k = 0; k < n; k += 4) {
      const std::uint32_t w = Word();
      std::memcpy(p + k, &w, n - k < 4 ? n - k : 4);
    }
  }
  // A well-formed command stream of random commands with random contents.
  std::vector<std::uint8_t> Commands(std::uint32_t count) {
    std::vector<std::uint8_t> out;
    for (std::uint32_t i = 0; i < count; ++i) {
      std::uint16_t type = 0;
      do {
        type = static_cast<std::uint16_t>(1u + R() % (static_cast<std::uint32_t>(cmd::CommandType::Count) - 1u));
      } while (excluded.find(type) != excluded.end());
      const std::uint32_t size = cmd::kCommandInfo[type].wireSize;
      std::vector<std::uint8_t> c(size, 0);
      const cmd::CommandHeader h{type, static_cast<std::uint16_t>(size), i};
      std::memcpy(c.data(), &h, sizeof(h));
      Fill(c.data() + sizeof(h), size - sizeof(h));
      out.insert(out.end(), c.begin(), c.end());
    }
    return out;
  }
  // Fallback section for the given capacities (FallbackHeader + bitsets + params).
  std::vector<std::uint8_t> Fallback(const ll::FallbackCaps& k, bool mismatch) {
    const std::uint32_t params = R() % 4u;
    ll::FallbackHeader fh{k.playbacks + (mismatch ? 1u : 0u), k.emitters, k.transports, k.userBuffers, k.banks,
                          params, R() % 3u, 0u};
    std::vector<std::uint8_t> out;
    Put(out, fh);
    const std::uint32_t words = ll::BitWordsOf(fh.playbackBits) + ll::BitWordsOf(k.emitters) +
                                ll::BitWordsOf(k.transports) + ll::BitWordsOf(k.userBuffers) + ll::BitWordsOf(k.banks);
    for (std::uint32_t w = 0; w < words; ++w) {
      const std::uint64_t v = (static_cast<std::uint64_t>(R()) << 32) | R();
      Put(out, R() % 2u == 0u ? v : std::uint64_t{0});
    }
    for (std::uint32_t p = 0; p < params; ++p) {
      cmd::OverflowParam op{};
      op.scope = static_cast<std::uint8_t>(R());
      op.isSwitch = static_cast<std::uint8_t>(R() % 3u);
      op.id = Word();
      op.target = snd::HandleRef{Word(), Word()};
      op.value = Word();
      op.used = 1u;
      Put(out, op);
    }
    return out;
  }
};

void Arena(std::vector<std::uint8_t>& rec, std::uint64_t block, const std::vector<std::uint8_t>& normal,
           const std::vector<std::uint8_t>& reserve, const std::vector<std::uint8_t>& fallback) {
  const ll::RecordArena ra{static_cast<std::uint32_t>(ll::RecordKind::Arena), 0u, block,
                           static_cast<std::uint32_t>(sizeof(ll::ArenaSections) + normal.size() + reserve.size() +
                                                      fallback.size()),
                           0u};
  Put(rec, ra);
  const ll::ArenaSections s{static_cast<std::uint32_t>(normal.size()), static_cast<std::uint32_t>(reserve.size()),
                            static_cast<std::uint32_t>(fallback.size()),
                            fallback.empty() ? 0u : static_cast<std::uint32_t>(cmd::kArenaHasFallback)};
  Put(rec, s);
  rec.insert(rec.end(), normal.begin(), normal.end());
  rec.insert(rec.end(), reserve.begin(), reserve.end());
  rec.insert(rec.end(), fallback.begin(), fallback.end());
}

std::vector<std::uint8_t> RandomRecord(Gen& g, const snd_config& cfg, std::uint64_t first) {
  std::vector<std::uint8_t> rec;
  Put(rec, ll::RecordLod{static_cast<std::uint32_t>(ll::RecordKind::Lod), 0u, first});
  Put(rec, ll::RecordBankMap{static_cast<std::uint32_t>(ll::RecordKind::BankMap), lltest::kBankId, g.R()});
  const ll::FallbackCaps caps = ll::CapsOf(cfg);
  std::uint64_t block = first;
  const std::uint32_t arenas = 1u + g.R() % 12u;
  for (std::uint32_t a = 0; a < arenas; ++a) {
    block += g.R() % 3u;
    const std::vector<std::uint8_t> normal = g.Commands(g.R() % 24u);
    const std::vector<std::uint8_t> reserve = g.Commands(g.R() % 4u);
    const std::vector<std::uint8_t> fb = g.R() % 4u == 0u ? g.Fallback(caps, false) : std::vector<std::uint8_t>{};
    Arena(rec, block, normal, reserve, fb);
    if (g.R() % 5u == 0u) Put(rec, ll::RecordStall{static_cast<std::uint32_t>(ll::RecordKind::Stall), g.R(), block});
    if (g.R() % 7u == 0u) Put(rec, ll::RecordLod{static_cast<std::uint32_t>(ll::RecordKind::Lod), g.R() % 4u, block});
  }
  return rec;
}

// Replays `rec` into a fresh System with the bank loaded and a voice playing.
int ReplayInto(const std::vector<std::uint8_t>& rec, ll::ReplayInfo* info) {
  const snd_config cfg = lltest::Config();
  ll::ReplayDesc rd;
  rd.config = &cfg;
  rd.allocator = &snd::base::DefaultAllocator();
  rd.record = rec.data();
  rd.bytes = rec.size();
  ll::Replayer* rp = nullptr;
  const int c = ll::Replayer::Create(rd, &rp);
  if (c != SND_OK) return c;
  {
    Fixture f(Hooks::Replay, cfg, rp);
    BankOpts o;
    o.withLimiter = true;
    o.withCategory = true;
    o.withGroup = true;
    f.Load(IndepBank(o), "a.sndb");
    f.sys->Play(HashId("Tone"), snd_emitter_handle{0}, nullptr);
    f.Steps(2);
    rp->Start();
    for (std::uint64_t i = 0; i < rp->Blocks() + 4u; ++i) f.Step();
    if (info != nullptr) *info = rp->Info();
    for (float x : f.out) {
      if (!std::isfinite(x)) {
        SND_CHECK_MSG(false, "non-finite output after a replay");
        break;
      }
    }
  }
  ll::Replayer::Destroy(rp);
  return SND_OK;
}

}  // namespace

// F5 / F7: malformed variants are refused by Create with the documented codes, at
// every truncation point, never crash.
SND_TEST(IndepLivelinkReplayFuzz, MalformedRecordingsAreRejected) {
  const snd_config cfg = lltest::Config();
  Gen g(0xBADu);
  std::vector<std::uint8_t> rec = RandomRecord(g, cfg, 30u);
  {
    ll::ReplayInfo info{};
    SND_REQUIRE_EQ(ReplayInto(rec, &info), SND_OK);
  }
  auto create = [&](const std::vector<std::uint8_t>& bytes) {
    ll::ReplayDesc rd;
    rd.config = &cfg;
    rd.allocator = &snd::base::DefaultAllocator();
    rd.record = bytes.data();
    rd.bytes = bytes.size();
    ll::Replayer* rp = nullptr;
    const int r = ll::Replayer::Create(rd, &rp);
    ll::Replayer::Destroy(rp);
    return r;
  };
  // Every truncation that does not end on a record boundary is CORRUPT.
  std::vector<std::uint64_t> bounds;
  {
    ll::RecordReader rr(rec.data(), rec.size());
    ll::RecordItem it;
    while (rr.Next(&it)) bounds.push_back(it.offset);
    bounds.push_back(rec.size());
  }
  std::size_t checked = 0;
  for (std::size_t cut = 1; cut < rec.size(); ++cut) {
    std::vector<std::uint8_t> t(rec.begin(), rec.begin() + static_cast<std::ptrdiff_t>(cut));
    const bool boundary = std::find(bounds.begin(), bounds.end(), cut) != bounds.end();
    const int r = create(t);
    if (!boundary) {
      SND_CHECK_MSG(r == SND_ERR_CORRUPT, "cut at %zu: %d", cut, r);
      ++checked;
    }
  }
  SND_CHECK(checked > 0u);
  // Arenas going back in time.
  std::vector<std::uint8_t> back;
  Put(back, ll::RecordLod{static_cast<std::uint32_t>(ll::RecordKind::Lod), 0u, 50u});
  Arena(back, 52u, g.Commands(2), {}, {});
  Arena(back, 51u, g.Commands(2), {}, {});
  SND_CHECK_EQ(create(back), SND_ERR_CORRUPT);
  // An arena before the starting Lod block.
  std::vector<std::uint8_t> before;
  Put(before, ll::RecordLod{static_cast<std::uint32_t>(ll::RecordKind::Lod), 0u, 50u});
  Arena(before, 49u, g.Commands(2), {}, {});
  SND_CHECK_EQ(create(before), SND_ERR_CORRUPT);
  // Fallback capacities that differ from the Config.
  std::vector<std::uint8_t> caps;
  Put(caps, ll::RecordLod{static_cast<std::uint32_t>(ll::RecordKind::Lod), 0u, 50u});
  Arena(caps, 50u, g.Commands(1), {}, g.Fallback(ll::CapsOf(cfg), true));
  SND_CHECK_EQ(create(caps), SND_ERR_VERSION_MISMATCH);
  // Unknown record kind, misaligned arena size, a bad command size inside a region.
  std::vector<std::uint8_t> unk;
  Put(unk, ll::RecordLod{9u, 0u, 50u});
  SND_CHECK_EQ(create(unk), SND_ERR_CORRUPT);
  std::vector<std::uint8_t> bad;
  Put(bad, ll::RecordLod{static_cast<std::uint32_t>(ll::RecordKind::Lod), 0u, 50u});
  std::vector<std::uint8_t> cmds = g.Commands(3);
  cmds[2] ^= 0x08u;  // first command's sizeBytes
  Arena(bad, 50u, cmds, {}, {});
  SND_CHECK_EQ(create(bad), SND_ERR_CORRUPT);
  // Empty recording: nothing to replay, not an error.
  SND_CHECK_EQ(create({}), SND_OK);
}

// F5 (defect): replaying random, structurally valid recordings must never corrupt
// memory, divide by zero or produce non-finite output. The Replayer validates the
// record structure only and hands command fields (transport time signatures, bus
// volumes ...) to core unchecked; the game API validates them on the way in, a replay
// file does not go through it. Runs last in this executable: it may abort under UBSan.
// SND_INDEP_EXCLUDE=26,... leaves out command types (exploration).
SND_TEST(IndepLivelinkReplayFuzz, Defect_livelink_replay_feeds_unvalidated_commands_to_core) {
  const snd_config cfg = lltest::Config();
  Gen g(0xC0FFEEu);
  if (const char* ex = std::getenv("SND_INDEP_EXCLUDE")) {
    for (const char* q = ex; *q != '\0';) {
      g.excluded.insert(static_cast<std::uint16_t>(std::strtol(q, const_cast<char**>(&q), 10)));
      if (*q == ',') ++q;
    }
  }
  const std::uint32_t n = FuzzIterations(25);
  std::uint32_t replayed = 0;
  for (std::uint32_t i = 0; i < n; ++i) {
    const std::vector<std::uint8_t> rec = RandomRecord(g, cfg, 20u + g.R() % 50u);
    ll::ReplayInfo info{};
    const int r = ReplayInto(rec, &info);
    SND_CHECK_MSG(r == SND_OK, "iteration %u: Create returned %d for a well-formed recording", i, r);
    replayed += r == SND_OK ? 1u : 0u;
  }
  SND_CHECK_EQ(replayed, n);
}
