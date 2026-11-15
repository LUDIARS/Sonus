// StreamReadPlanner against a model, exhaustively over small geometries
// (design.md 6.3 "ループとつなぎ"): every combination of data range, loop range
// (valid and invalid: empty, reversed, outside the data, loopBegin == 0,
// loopEnd == data end, loop shorter / equal / longer than a chunk, non-integer
// multiples), chunk size and start position (Seek anywhere, also past the loop
// end). The concatenated reads must equal the "unrolled" ideal byte sequence.
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "snd/io/stream_read_planner.hpp"
#include "snd/test/test.hpp"

using snd::io::StreamRead;
using snd::io::StreamReadPlanner;

namespace {

struct Geometry {
  std::uint64_t db, de, lb, le;
  std::uint32_t chunk;
  std::int64_t seek;  // -1: no Seek after Reset
};

// Ideal byte offsets from the start position for `count` bytes.
std::vector<std::uint64_t> Ideal(const Geometry& g, std::uint64_t count, bool* looping) {
  const std::uint64_t de = g.de > g.db ? g.de : g.db;
  const bool loop = g.le != 0u && g.lb < g.le && g.lb >= g.db && g.le <= de;
  *looping = loop;
  std::uint64_t cur = g.db;
  if (g.seek >= 0) {
    cur = static_cast<std::uint64_t>(g.seek);
    if (cur < g.db) cur = g.db;
    if (cur > de) cur = de;
    if (loop && cur >= g.le) cur = g.lb;
  }
  std::vector<std::uint64_t> v;
  const std::uint64_t limit = loop ? g.le : de;
  while (v.size() < count) {
    if (cur >= limit) {
      if (!loop) break;
      cur = g.lb;
    }
    v.push_back(cur++);
  }
  return v;
}

std::string Describe(const Geometry& g) {
  char b[160];
  std::snprintf(b, sizeof b, "data [%llu,%llu) loop [%llu,%llu) chunk %u seek %lld", static_cast<unsigned long long>(g.db),
                static_cast<unsigned long long>(g.de), static_cast<unsigned long long>(g.lb),
                static_cast<unsigned long long>(g.le), g.chunk, static_cast<long long>(g.seek));
  return b;
}

// Returns an empty string when the planner matches the model.
std::string Check(const Geometry& g) {
  StreamReadPlanner p;
  p.Reset(g.db, g.de, g.lb, g.le, g.chunk);
  if (g.seek >= 0) p.Seek(static_cast<std::uint64_t>(g.seek));
  bool looping = false;
  const std::uint64_t want = 3u * (g.de + 2u) + 7u;
  const std::vector<std::uint64_t> ideal = Ideal(g, want, &looping);
  if (p.Looping() != looping) return "Looping() differs";
  std::vector<std::uint64_t> got;
  bool expectWrapped = false;
  bool first = true;
  for (int reads = 0; reads < 10000 && got.size() < want; ++reads) {
    const std::uint64_t btb = p.BytesToBoundary();
    StreamRead r{};
    if (!p.Next(r)) {
      if (looping) return "Next() returned false on a looping stream";
      if (btb != 0u) return "BytesToBoundary not 0 at the end";
      break;
    }
    if (r.bytes == 0u) return "0-byte read";
    if (r.bytes > (g.chunk == 0u ? 1u : g.chunk)) return "read longer than a chunk";
    if (looping && r.offset < g.le && r.offset + r.bytes > g.le) return "read crosses the loop end";
    if (looping && !first && r.wrapped != expectWrapped) return "wrapped flag wrong";
    if (r.loopEnd != (looping && r.offset + r.bytes == g.le)) return "loopEnd flag wrong";
    if (r.wrapped && r.offset != g.lb) return "wrapped read not at loop start";
    if (btb != 0u && r.bytes > btb) return "read longer than BytesToBoundary";
    expectWrapped = r.loopEnd;
    first = false;
    for (std::uint32_t i = 0; i < r.bytes; ++i) got.push_back(r.offset + i);
  }
  if (got.size() > ideal.size()) got.resize(ideal.size());
  if (got != ideal) {
    std::size_t k = 0;
    while (k < got.size() && k < ideal.size() && got[k] == ideal[k]) ++k;
    char b[96];
    std::snprintf(b, sizeof b, "byte sequence differs at %zu (got %zu bytes, want %zu)", k, got.size(), ideal.size());
    return b;
  }
  // A non-looping stream must end (Next false) after the ideal bytes.
  if (!looping) {
    StreamRead r{};
    if (p.Next(r)) return "non-looping stream did not end";
  }
  return {};
}

}  // namespace

SND_TEST(StreamPlannerModel, ExhaustiveSmallGeometriesMatchTheUnrolledLoop) {
  std::uint32_t cases = 0, failures = 0;
  for (std::uint64_t db : {0ull, 3ull}) {
    for (std::uint64_t de = 0; de <= 14; ++de) {
      for (std::uint64_t lb = 0; lb <= 15; ++lb) {
        for (std::uint64_t le = 0; le <= 15; ++le) {
          for (std::uint32_t chunk = 0; chunk <= 7; ++chunk) {
            for (std::int64_t seek = -1; seek <= 16; ++seek) {
              const Geometry g{db, de, lb, le, chunk, seek};
              const std::string e = Check(g);
              ++cases;
              if (!e.empty() && ++failures <= 10) {
                SND_CHECK_MSG(e.empty(), "%s: %s", Describe(g).c_str(), e.c_str());
              }
            }
          }
        }
      }
    }
  }
  SND_CHECK_EQ(failures, 0u);
  snd::test::Note("%u geometries checked", cases);
}

SND_TEST(StreamPlannerModel, LargeOffsetsAndChunkMultiples) {
  const std::uint64_t base = 5ull << 32;  // beyond 4 GiB
  const Geometry gs[] = {
      {base, base + 1000000, base + 4096, base + 4096 * 3, 4096, -1},       // loop = 2 chunks exactly
      {base, base + 1000000, base + 100, base + 4096 + 101, 4096, -1},      // loop = chunk + 1
      {base, base + 1000000, base + 100, base + 100 + 4095, 4096, -1},      // loop = chunk - 1
      {base, base + 1000000, base, base + 1000000, 65536, -1},              // whole file loops
      {base, base + 1000000, base + 999999, base + 1000000, 32768, -1},     // 1-byte loop at the end
      {base, base + 1000000, base + 50, base + 60, 32768, base + 999000},   // seek past loop end
      {base, base + 1000000, 0, 0, 32768, base + 999999},                    // no loop, seek to the last byte
  };
  for (const Geometry& g : gs) {
    StreamReadPlanner p;
    p.Reset(g.db, g.de, g.lb, g.le, g.chunk);
    if (g.seek >= 0) p.Seek(static_cast<std::uint64_t>(g.seek));
    bool looping = false;
    const std::vector<std::uint64_t> ideal = Ideal(g, 300000, &looping);
    std::uint64_t k = 0;
    bool ok = true;
    for (int reads = 0; reads < 400000 && k < ideal.size(); ++reads) {
      StreamRead r{};
      if (!p.Next(r)) break;
      if (r.bytes == 0u || r.bytes > g.chunk) ok = false;
      for (std::uint32_t i = 0; i < r.bytes && k < ideal.size(); ++i, ++k) {
        if (r.offset + i != ideal[k]) ok = false;
      }
      if (!ok) break;
    }
    SND_CHECK_MSG(ok && k == ideal.size(), "%s", Describe(g).c_str());
  }
}
