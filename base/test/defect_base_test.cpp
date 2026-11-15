// Independent tests that FAIL on the current base implementation (defects
// found by the independent review; kept failing on purpose, see the report).
// Each test names the property it checks. They are a separate executable so
// the passing suites stay green and ctest shows exactly which defect fails:
//   base_defect_handle_test   HandleAllocator accepts never-issued generations (bit 31)
//   base_defect_snapshot_test queries can see a frame newer than the latest Update, then an older one
//   base_defect_alloc_test    AccountingAllocator: spurious budget failures / bogus peak under concurrency
//   base_defect_size_test     BlockPool / ThreadArenaSet accept sizes whose total wraps size_t
//   base_defect_log_test      a conversion wider than 127 characters is silently cut
#include <atomic>
#include <bit>
#include <cstdint>
#include <cstring>
#include <thread>
#include <vector>

#include "adv_test_util.hpp"
#include "snd/base/allocator.hpp"
#include "snd/base/arena.hpp"
#include "snd/base/handle.hpp"
#include "snd/base/log.hpp"
#include "snd/base/notify.hpp"
#include "snd/base/pool.hpp"
#include "snd/test/test.hpp"

using namespace snd::base;
namespace nt = snd::base::notify;

#if defined(SND_DEFECT_HANDLE)
// 3.1 / D-05: an operation with a handle whose generation does not match must
// be ignored. HandleAllocator stores `generation | kLiveBit` and compares
// `stored == (h.generation | kLiveBit)`, so a handle whose generation has bit
// 31 set (never issued: issued generations stay below 2^31) matches the live
// row of generation g & 0x7FFFFFFF. Release() of such a handle frees the
// slot under its real owner.
SND_TEST(DefectHandle, GenerationWithBit31IsNotCurrentAndCannotRelease) {
  HandleAllocator h;
  SND_REQUIRE(h.Init(DefaultAllocator(), 2));
  const snd::HandleRef real = h.Acquire();
  SND_REQUIRE(real.generation == 1u);
  const snd::HandleRef forged{real.slot, real.generation | 0x80000000u};  // never issued
  SND_CHECK_MSG(!h.IsCurrent(forged), "IsCurrent accepted generation 0x%08x for live generation %u",
                forged.generation, real.generation);
  SND_CHECK_MSG(!h.Release(forged), "Release accepted generation 0x%08x and freed the slot of the live handle",
                forged.generation);
  SND_CHECK_MSG(h.IsCurrent(real), "the real owner's handle went stale");
}
#endif

#if defined(SND_DEFECT_SNAPSHOT)
// 2.1: query results are "consistent as of the latest Update()". ReadFront
// loads `frontPublished`, then reads that frame under its seqlock, but never
// re-checks that the frame is still the front. If the reader is delayed after
// loading the index, Update() swaps and the audio thread recycles that frame
// (front -> middle -> back) and publishes a newer block into it; the seqlock is
// even again, so the query returns a block that no Update() has published yet.
// The next query reads the real front and goes back in time.
SND_TEST(DefectSnapshot, QueriesFromOneThreadNeverGoBackInTime) {
  nt::SnapshotTripleBuffer tb;
  SND_REQUIRE(nt::InitTripleBuffer(tb, DefaultAllocator(), 4, 1, 1));
  std::atomic<bool> stop{false};
  std::atomic<std::uint64_t> backwards{0}, reads{0}, worst{0};
  std::thread writer([&] {
    snd_stats st{};
    for (std::uint64_t v = 1; !stop.load(std::memory_order_relaxed); ++v) {
      nt::SnapshotFrame& f = nt::BeginSnapshot(tb);
      nt::SetFrameHeader(f, v, v, st, 0);
      nt::EndSnapshot(tb);
    }
  });
  std::thread update([&] {
    while (!stop.load(std::memory_order_relaxed)) nt::SwapFront(tb);
  });
  std::vector<std::thread> qs;
  for (int q = 0; q < 3; ++q) {
    qs.emplace_back([&] {
      std::uint64_t last = 0;
      while (!stop.load(std::memory_order_relaxed)) {
        std::uint64_t b = 0;
        if (!nt::ReadFront(tb, [&](const nt::SnapshotFrame& f) { b = nt::GetFrameHeader(f).block; })) continue;
        reads.fetch_add(1, std::memory_order_relaxed);
        if (b < last) {
          backwards.fetch_add(1, std::memory_order_relaxed);
          AtomicFetchMax(worst, last - b);
        }
        if (b > last) last = b;
      }
    });
  }
  const std::uint64_t seconds = 2u * snd::advtest::StressScale();
  std::this_thread::sleep_for(std::chrono::seconds(seconds));
  stop = true;
  writer.join();
  update.join();
  for (auto& t : qs) t.join();
  snd::test::Note("reads=%llu backwards=%llu worst step back=%llu blocks", static_cast<unsigned long long>(reads.load()),
                  static_cast<unsigned long long>(backwards.load()), static_cast<unsigned long long>(worst.load()));
  SND_CHECK_MSG(backwards.load() == 0u, "%llu queries returned an older block than an earlier query on the same thread",
                static_cast<unsigned long long>(backwards.load()));
  nt::DestroyTripleBuffer(tb, DefaultAllocator());
}
#endif

#if defined(SND_DEFECT_ALLOC)
// 1.2 principle 2 / 3.3: a request is refused only when it would push `used`
// above the budget. AccountingAllocator reserves `bytes` with fetch_add before
// checking, so an oversized request that is about to fail makes concurrent,
// well-within-budget requests fail too (normal workers load banks in parallel).
SND_TEST(DefectAlloc, OversizedFailingRequestDoesNotFailConcurrentSmallOnes) {
  AccountingAllocator acc(DefaultAllocator());
  constexpr std::uint64_t kBudget = 1u << 20;
  acc.SetBudget(snd::MemTag::Bank, kBudget);
  std::atomic<bool> stop{false};
  std::thread big([&] {
    while (!stop.load(std::memory_order_relaxed)) {
      void* p = acc.Allocate(kBudget + 1u, 64, snd::MemTag::Bank);  // always refused (over budget)
      if (p != nullptr) acc.Free(p, snd::MemTag::Bank);
    }
  });
  std::uint64_t spurious = 0;
  const int kSmall = 200000;
  for (int i = 0; i < kSmall; ++i) {
    void* p = acc.Allocate(64, 64, snd::MemTag::Bank);  // 64 bytes of a 1 MiB budget, nothing else live
    if (p == nullptr) {
      ++spurious;
      continue;
    }
    acc.Free(p, snd::MemTag::Bank);
  }
  stop = true;
  big.join();
  snd::test::Note("%llu of %d small requests refused", static_cast<unsigned long long>(spurious), kSmall);
  SND_CHECK_MSG(spurious == 0u, "%llu requests of 64 bytes were refused although used <= 64 of %llu",
                static_cast<unsigned long long>(spurious), static_cast<unsigned long long>(kBudget));
}

// 3.3 "区分ごとに集計": peak is the high-water mark of the bytes actually
// allocated. A concurrent request that fails (upstream refuses a huge size, or
// the size overflows) is still added to `used` for a moment, and a successful
// request in that window records the inflated value as the peak.
namespace {
// Upstream that refuses anything above 1 MiB (models OOM of a huge bank).
class RefusingUpstream final : public snd::IAllocator {
 public:
  void* Allocate(std::size_t bytes, std::size_t align, snd::MemTag tag) override {
    if (bytes > (std::size_t{1} << 20)) return nullptr;
    return DefaultAllocator().Allocate(bytes, align, tag);
  }
  void Free(void* p, snd::MemTag tag) override { DefaultAllocator().Free(p, tag); }
};
}  // namespace

SND_TEST(DefectAlloc, PeakIgnoresConcurrentFailedRequests) {
  RefusingUpstream upstream;
  AccountingAllocator acc(upstream);  // no budget
  std::atomic<bool> stop{false};
  std::thread big([&] {
    while (!stop.load(std::memory_order_relaxed)) {
      void* p = acc.Allocate(std::size_t{1} << 40, 64, snd::MemTag::Misc);  // upstream refuses
      if (p != nullptr) acc.Free(p, snd::MemTag::Misc);
    }
  });
  for (int i = 0; i < 200000; ++i) {
    void* p = acc.Allocate(64, 64, snd::MemTag::Misc);
    if (p != nullptr) acc.Free(p, snd::MemTag::Misc);
  }
  stop = true;
  big.join();
  const MemTagUsage u = acc.Usage(snd::MemTag::Misc);
  snd::test::Note("peak=%llu used=%llu", static_cast<unsigned long long>(u.peak), static_cast<unsigned long long>(u.used));
  SND_CHECK_EQ(u.used, 0u);
  SND_CHECK_MSG(u.peak <= 64u, "peak %llu bytes although at most 64 bytes were ever allocated",
                static_cast<unsigned long long>(u.peak));
}
#endif

#if defined(SND_DEFECT_SIZE)
// Huge sizes must be refused, not wrapped. stride * blockCount wraps size_t
// (2 * (2^63 + 64) == 128), the pool allocates 128 bytes and claims two
// blocks of 2^63 + 64 bytes: Block(1) points 8 EiB past the allocation.
SND_TEST(DefectSize, BlockPoolRejectsTotalSizeOverflow) {
  AccountingAllocator acc(DefaultAllocator());
  BlockPool p;
  const std::size_t huge = (std::size_t{1} << 63) + 64u;
  const bool ok = p.Init(acc, huge, 2, snd::MemTag::Misc);
  SND_CHECK_MSG(!ok, "Init accepted 2 blocks of %zu bytes with an upstream allocation of %llu bytes", huge,
                static_cast<unsigned long long>(acc.Usage(snd::MemTag::Misc).used));
  p.Destroy();
}

// Same wrap in ThreadArenaSet::Init (per * threads). Run last: with UBSan the
// pointer arithmetic on the wrapped size may abort the process.
SND_TEST(DefectSize, ThreadArenaSetRejectsTotalSizeOverflow) {
  AccountingAllocator acc(DefaultAllocator());
  ThreadArenaSet s;
  const std::size_t huge = (std::size_t{1} << 63) + 64u;
  const bool ok = s.Init(acc, 2, huge);
  SND_CHECK_MSG(!ok, "Init accepted 2 arenas of %zu bytes from %llu bytes of memory", huge,
                static_cast<unsigned long long>(acc.Usage(snd::MemTag::ThreadArena).used));
  s.Destroy();
}
#endif

#if defined(SND_DEFECT_LOG)
// log.hpp: "Supported conversions ... with flags / width / precision"; the
// only documented truncation is the 512-byte Drain buffer. Emit() formats
// each conversion into a 128-byte buffer, so a width of 200 yields 127
// characters and the rest of the line shifts.
SND_TEST(DefectLog, WideConversionIsNotCutAt127Characters) {
  char out[512];
  LogEntry e{"[%200d]", {7u}, 0u, 0u, 1u};
  const std::size_t n = FormatLogEntry(e, out, sizeof(out));
  SND_CHECK_MSG(n == 202u, "\"[%%200d]\" produced %zu characters (expected 202)", n);
  LogEntry f{"[%.150f]", {std::bit_cast<std::uint64_t>(1.0)}, 0u, 0u, 1u};
  const std::size_t m = FormatLogEntry(f, out, sizeof(out));
  SND_CHECK_MSG(m == 154u, "\"[%%.150f]\" produced %zu characters (expected 154)", m);
}
#endif
