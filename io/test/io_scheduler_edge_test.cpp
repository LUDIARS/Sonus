// Independent edge-case tests of IoScheduler / StreamChannel / FileRequest
// (design.md 6.1, 6.3, 10.3, 14; CONTRACTS.md D-08, D-24), single thread
// (RunOnce) over ScriptFs with a manual clock.
//
// Tests named IoDefect.* reproduce defects found by this review and are
// expected to FAIL until the implementation is fixed.
#include <cstdint>
#include <string>
#include <vector>

#include "io_adversarial_util.hpp"
#include "snd/internal/rt_check.hpp"
#include "snd/io/io_scheduler.hpp"
#include "snd/platform/clock.hpp"
#include "snd/test/rt_guard.hpp"
#include "snd/test/test.hpp"

using namespace snd::iotest;
using snd::io::ChunkRecord;
using snd::io::ChunkStatus;
using snd::io::FileRequest;
using snd::io::FileResult;
using snd::io::FileState;
using snd::io::IoClass;
using snd::io::kInvalidChunk;

namespace {

constexpr std::uint32_t kFile = 64u * kChunk;

void AddStd(ScriptRig& rig) {
  rig.fs.AddFile("a.bin", MakePattern(1, kFile));
  rig.fs.AddFile("b.bin", MakePattern(2, kFile));
  rig.fs.AddFile("c.bin", MakePattern(3, kFile));
}

// Pops every ready record of `slot`, releases it, returns the tags in order.
std::vector<std::uint32_t> Drain(ScriptRig& rig, std::uint32_t slot, std::vector<ChunkRecord>* recs = nullptr,
                                 std::uint32_t seed = 0, bool* dataOk = nullptr) {
  std::vector<std::uint32_t> tags;
  ChunkRecord r{};
  while (rig.io.Channel(slot).Pop(r)) {
    tags.push_back(r.tag);
    if (recs != nullptr) recs->push_back(r);
    if (dataOk != nullptr && r.status == ChunkStatus::Ok && !MatchesPattern(seed, r.offset, rig.pool.Data(r.chunk), r.bytes)) {
      *dataOk = false;
    }
    rig.io.Channel(slot).Release(r);
  }
  return tags;
}

std::string LastIssuedPath(ScriptRig& rig) {
  const std::vector<ScriptFs::Issue> v = rig.fs.Issues();
  return v.empty() ? std::string() : v.back().path;
}

struct CallbackLog {
  std::atomic<std::uint32_t> calls{0};
  FileResult last{};
  static void Fn(void* user, const FileResult& r) noexcept {
    auto* l = static_cast<CallbackLog*>(user);
    l->last = r;
    l->calls.fetch_add(1);
  }
};

// IAllocator that fails the n-th allocation (0-based) and every later one when `failAll`.
class FailingAllocator final : public snd::IAllocator {
 public:
  explicit FailingAllocator(snd::IAllocator& up) : up_(up) {}
  void Arm(std::int64_t n) { failAt_ = n; count_ = 0; }
  std::int64_t Count() const { return count_; }
  void* Allocate(std::size_t bytes, std::size_t align, snd::MemTag tag) override {
    if (failAt_ >= 0 && count_++ == failAt_) return nullptr;
    return up_.Allocate(bytes, align, tag);
  }
  void Free(void* p, snd::MemTag tag) override { up_.Free(p, tag); }

 private:
  snd::IAllocator& up_;
  std::int64_t failAt_ = -1;
  std::int64_t count_ = 0;
};

}  // namespace

// ===========================================================================
// Defects (expected to fail)
// ===========================================================================

// Select() only looks at the head of each class list and, once the pool is
// empty, skips every class whose head has no chunk. A request that already
// owns a chunk (posted with dst = a caller chunk, or one that kept its chunk
// after a rejected ReadAsync) but sits BEHIND a chunkless request of the same
// class is then never issued. When those queued requests hold the last free
// chunks, nothing can ever complete: the stream stalls forever.
SND_TEST(IoDefect, QueuedCallerChunksBehindChunklessHeadDeadlock) {
  ScriptRig rig(2, kChunk);
  SND_REQUIRE(rig.Init());
  AddStd(rig);
  // Stream 0 asks the IO thread to borrow a chunk (dst = nullptr).
  SND_REQUIRE(rig.io.Post(StreamReq(0, "a.bin", 0, kChunk, IoClass::Music, 1)));
  // Stream 1 brings its own chunks: they are the whole pool.
  const std::uint32_t c0 = rig.pool.Acquire(1);
  const std::uint32_t c1 = rig.pool.Acquire(1);
  SND_REQUIRE(c0 != kInvalidChunk && c1 != kInvalidChunk);
  SND_REQUIRE(rig.io.Post(StreamReq(1, "b.bin", 0, kChunk, IoClass::Music, 2, rig.pool.Data(c0))));
  SND_REQUIRE(rig.io.Post(StreamReq(1, "b.bin", kChunk, kChunk, IoClass::Music, 3, rig.pool.Data(c1))));
  for (int i = 0; i < 100; ++i) {
    rig.io.RunOnce();
    rig.clock.AdvanceMs(10);
  }
  // Stream 1's reads own their memory; they must be served.
  const std::vector<std::uint32_t> got1 = Drain(rig, 1);
  SND_CHECK_EQ(got1.size(), 2u);
  // ... after which stream 0 gets a chunk too.
  for (int i = 0; i < 10; ++i) rig.io.RunOnce();
  SND_CHECK_EQ(Drain(rig, 0).size(), 1u);
  snd::test::Note("reads issued %llu, pool starved %llu", static_cast<unsigned long long>(rig.io.Stats().readsIssued),
                  static_cast<unsigned long long>(rig.io.Stats().poolStarved));
}

// The scheduler copies paths into a 256-byte table entry and fails longer ones
// (OpenFailed / Error) although the file exists and the file system accepts
// them (DefaultFileSystem allows 2047 bytes). The limit is not documented in
// io_scheduler.hpp, source_if.hpp or the contract.
SND_TEST(IoDefect, PathsOf256BytesOrMoreFailWithoutBeingTried) {
  ScriptRig rig;
  SND_REQUIRE(rig.Init());
  std::string path(300, 'p');
  path += ".bin";
  rig.fs.AddFile(path, MakePattern(4, 1000));
  SND_REQUIRE(rig.io.Post(StreamReq(0, path.c_str(), 0, 1000, IoClass::Music, 1)));
  std::vector<std::uint8_t> dst(1000);
  FileRequest req;
  req.path = path.c_str();
  req.bytes = 1000;
  req.dst = dst.data();
  SND_REQUIRE(rig.io.Submit(req));
  rig.Run();
  std::vector<ChunkRecord> recs;
  Drain(rig, 0, &recs);
  SND_REQUIRE_EQ(recs.size(), 1u);
  SND_CHECK(recs[0].status == ChunkStatus::Ok);
  SND_CHECK(req.State() == FileState::Done);
  SND_CHECK_EQ(rig.fs.OpenCalls(), 1u);  // the file system was never asked
}

// A size query (FileRequest with dst == nullptr) is documented as "no read,
// no bandwidth", but IssueLoop tests `windowUsed + cost > budget` also for
// cost 0: once a window is over budget (the allowed single oversize read), a
// size query waits for the next window although it uses no bandwidth.
// Low severity (at most one window of latency for bank loading).
SND_TEST(IoDefect, SizeQueryIsHeldBackByAnExhaustedBudget) {
  ScriptRig rig;
  rig.cfg.bytesPerWindow = kChunk / 4u;
  SND_REQUIRE(rig.Init());
  AddStd(rig);
  SND_REQUIRE(rig.io.Post(StreamReq(0, "a.bin", 0, kChunk, IoClass::Music, 0)));  // oversize: allowed alone
  rig.io.RunOnce();
  FileRequest q;
  q.path = "b.bin";
  SND_REQUIRE(rig.io.Submit(q));
  for (int i = 0; i < 5; ++i) rig.io.RunOnce();  // same window
  SND_CHECK(q.State() == FileState::Done);
}

// ===========================================================================
// Ownership of chunks on Post
// ===========================================================================

SND_TEST(IoEdge, RefusedPostLeavesTheChunkWithTheCaller) {
  ScriptRig rig(8);
  rig.cfg.ringChunks = 2;
  SND_REQUIRE(rig.Init());
  AddStd(rig);
  // Fill the channel.
  for (std::uint32_t i = 0; i < 2; ++i) SND_REQUIRE(rig.io.Post(StreamReq(0, "a.bin", i * kChunk, kChunk, IoClass::Music, i)));
  const std::uint32_t c = rig.pool.Acquire(0);
  SND_REQUIRE(c != kInvalidChunk);
  {
    SND_RT_SCOPE();
    SND_CHECK(!rig.io.Post(StreamReq(0, "a.bin", 0, kChunk, IoClass::Music, 9, rig.pool.Data(c))));  // channel full
  }
  SND_CHECK(rig.pool.IsBorrowed(c));
  SND_CHECK_EQ(rig.pool.OwnerOf(c), 0u);
  rig.Run();
  SND_CHECK_EQ(Drain(rig, 0).size(), 2u);
  SND_CHECK(rig.pool.IsBorrowed(c));  // the scheduler did not take it
  SND_CHECK_EQ(rig.pool.Used(), 1u);
  rig.pool.Release(c);
  // After Shutdown: refused, caller keeps it.
  const std::uint32_t d = rig.pool.Acquire(3);
  rig.io.Shutdown();
  SND_CHECK(!rig.io.Post(StreamReq(3, "a.bin", 0, kChunk, IoClass::Music, 9, rig.pool.Data(d))));
  SND_CHECK(rig.pool.IsBorrowed(d));
  SND_CHECK_EQ(rig.io.Channel(3).Outstanding(), 0u);
  rig.pool.Release(d);
  SND_CHECK_EQ(rig.pool.Stats().badReleases, 0u);
}

SND_TEST(IoEdge, CallerChunkOfAnotherOwnerAndStaleCallerChunkAreReturnedOnce) {
  ScriptRig rig(4);
  SND_REQUIRE(rig.Init());
  AddStd(rig);
  rig.fs.SetLatency(5);
  const std::uint32_t c = rig.pool.Acquire(snd::io::kNoOwner);
  SND_REQUIRE(rig.io.Post(StreamReq(2, "a.bin", 0, kChunk, IoClass::Voice, 1, rig.pool.Data(c))));
  const std::uint32_t d = rig.pool.Acquire(2);
  SND_REQUIRE(rig.io.Post(StreamReq(2, "a.bin", kChunk, kChunk, IoClass::Voice, 2, rig.pool.Data(d))));
  rig.io.RunOnce();  // both in flight (maxInFlight 4)
  rig.io.Channel(2).Cancel();
  rig.Run();
  SND_CHECK_EQ(rig.pool.Used(), 0u);
  SND_CHECK_EQ(rig.pool.Stats().badReleases, 0u);
  SND_CHECK(rig.io.Channel(2).Reclaim());
  SND_CHECK_EQ(rig.io.Stats().staleCompleted, 2u);
}

// ===========================================================================
// Cancel / epochs
// ===========================================================================

SND_TEST(IoEdge, RepostAfterCancelIsNotHeldBackByStaleInFlightReads) {
  ScriptRig rig;
  rig.cfg.maxInFlight = 4;
  SND_REQUIRE(rig.Init());
  AddStd(rig);
  rig.fs.SetLatency(1000000);  // the old reads never finish during the test
  for (std::uint32_t i = 0; i < 2; ++i) SND_REQUIRE(rig.io.Post(StreamReq(0, "a.bin", i * kChunk, kChunk, IoClass::Music, i)));
  rig.io.RunOnce();
  SND_CHECK_EQ(rig.io.Stats().inFlight, 2u);
  rig.io.Channel(0).Cancel();
  rig.fs.SetLatency(0);
  for (std::uint32_t i = 0; i < 2; ++i) {
    SND_REQUIRE(rig.io.Post(StreamReq(0, "a.bin", (10 + i) * kChunk, kChunk, IoClass::Music, 100 + i)));
  }
  for (int i = 0; i < 10; ++i) rig.io.RunOnce();
  std::vector<ChunkRecord> recs;
  bool ok = true;
  Drain(rig, 0, &recs, 1, &ok);
  SND_REQUIRE_EQ(recs.size(), 2u);
  SND_CHECK_EQ(recs[0].tag, 100u);
  SND_CHECK_EQ(recs[1].tag, 101u);
  SND_CHECK(ok);
  SND_CHECK_EQ(rig.io.Channel(0).ReadPos(), 12u * kChunk);
  SND_CHECK_EQ(rig.io.Channel(0).Outstanding(), 2u);  // the two stale in-flight reads
  SND_CHECK(!rig.io.Channel(0).Reclaim());
}

SND_TEST(IoEdge, CancelBetweenDeliveryAndPopNeverHandsOutStaleData) {
  ScriptRig rig;
  SND_REQUIRE(rig.Init());
  AddStd(rig);
  for (std::uint32_t i = 0; i < 4; ++i) SND_REQUIRE(rig.io.Post(StreamReq(0, "a.bin", i * kChunk, kChunk, IoClass::Music, i)));
  rig.Run();
  SND_CHECK_EQ(rig.io.Channel(0).Ready(), 4u);
  ChunkRecord first{};
  SND_REQUIRE(rig.io.Channel(0).Pop(first));  // consumer holds one current record
  rig.io.Channel(0).Cancel();
  SND_REQUIRE(rig.io.Post(StreamReq(0, "b.bin", 0, kChunk, IoClass::Music, 50)));
  {
    SND_RT_SCOPE();
    ChunkRecord r{};
    SND_CHECK(rig.io.Channel(0).Peek() == nullptr);  // stale ones skipped and released
    SND_CHECK(!rig.io.Channel(0).Pop(r));
  }
  SND_CHECK_EQ(rig.io.Channel(0).Outstanding(), 2u);  // held `first` + the new request
  rig.Run();
  std::vector<ChunkRecord> recs;
  bool ok = true;
  Drain(rig, 0, &recs, 2, &ok);
  SND_REQUIRE_EQ(recs.size(), 1u);
  SND_CHECK_EQ(recs[0].tag, 50u);
  SND_CHECK(ok);
  rig.io.Channel(0).Release(first);  // releasing a record of an old epoch is still fine
  SND_CHECK_EQ(rig.io.Channel(0).Outstanding(), 0u);
  SND_CHECK_EQ(rig.pool.Used(), 0u);
}

SND_TEST(IoEdge, CancelStormKeepsBooksAndWakesOnce) {
  ScriptRig rig;
  rig.cfg.ringChunks = 4;
  SND_REQUIRE(rig.Init());
  AddStd(rig);
  rig.fs.SetRandomLatency(3, 7);
  std::uint32_t lastTag = 0;
  for (std::uint32_t round = 0; round < 500; ++round) {
    {
      SND_RT_SCOPE();
      for (std::uint32_t k = 0; k < 1u + round % 4u; ++k) {
        if (rig.io.Post(StreamReq(1, "c.bin", (round % 60) * kChunk, kChunk, IoClass::Voice, round))) lastTag = round;
      }
      for (std::uint32_t k = 0; k < round % 5u; ++k) rig.io.Channel(1).Cancel();
    }
    if (round % 3u == 0u) rig.io.RunOnce();
    ChunkRecord r{};
    while (rig.io.Channel(1).Pop(r)) {
      SND_CHECK_EQ(r.epoch, rig.io.Channel(1).Epoch());
      rig.io.Channel(1).Release(r);
    }
  }
  (void)lastTag;
  rig.io.Channel(1).Cancel();
  for (int i = 0; i < 100 && !rig.io.Channel(1).Reclaim(); ++i) rig.io.RunOnce();
  SND_CHECK(rig.io.Channel(1).Reclaim());
  SND_CHECK_EQ(rig.pool.Used(), 0u);
  SND_CHECK(rig.fs.Violations().empty());
}

// ===========================================================================
// Order and priority
// ===========================================================================

SND_TEST(IoEdge, ChannelOrderSurvivesPriorityChangeMidStream) {
  ScriptRig rig;
  rig.cfg.maxInFlight = 1;
  SND_REQUIRE(rig.Init());
  AddStd(rig);
  // The stream changes class (e.g. a voice promoted to music): later requests
  // are issued first, but delivery must stay in request order.
  SND_REQUIRE(rig.io.Post(StreamReq(0, "a.bin", 0, kChunk, IoClass::Ambience, 1)));
  SND_REQUIRE(rig.io.Post(StreamReq(0, "a.bin", kChunk, kChunk, IoClass::Voice, 2)));
  SND_REQUIRE(rig.io.Post(StreamReq(0, "a.bin", 2u * kChunk, kChunk, IoClass::Music, 3)));
  rig.Run();
  const std::vector<ScriptFs::Issue> issues = rig.fs.Issues();
  SND_REQUIRE_EQ(issues.size(), 3u);
  SND_CHECK_EQ(issues[0].offset, 2u * kChunk);  // music first
  SND_CHECK_EQ(issues[2].offset, 0u);
  const std::vector<std::uint32_t> tags = Drain(rig, 0);
  SND_REQUIRE_EQ(tags.size(), 3u);
  SND_CHECK_EQ(tags[0], 1u);
  SND_CHECK_EQ(tags[1], 2u);
  SND_CHECK_EQ(tags[2], 3u);
}

SND_TEST(IoEdge, ClassOrderThenFifoInsideAClass) {
  ScriptRig rig;
  rig.cfg.maxInFlight = 1;
  SND_REQUIRE(rig.Init());
  AddStd(rig);
  // Posted: voice s1, amb s4, voice s2, music s5, voice s3, music s6 (one request each).
  const std::uint32_t slots[] = {1, 4, 2, 5, 3, 6};
  const IoClass cls[] = {IoClass::Voice, IoClass::Ambience, IoClass::Voice, IoClass::Music, IoClass::Voice, IoClass::Music};
  for (int i = 0; i < 6; ++i) SND_REQUIRE(rig.io.Post(StreamReq(slots[i], "a.bin", slots[i] * kChunk, kChunk, cls[i], 0)));
  rig.Run();
  const std::vector<ScriptFs::Issue> issues = rig.fs.Issues();
  SND_REQUIRE_EQ(issues.size(), 6u);
  const std::uint32_t expect[] = {5, 6, 1, 2, 3, 4};
  for (int i = 0; i < 6; ++i) SND_CHECK_EQ(issues[i].offset, expect[i] * kChunk);
}

SND_TEST(IoEdge, AgingThresholdBoundaryAndOldestFirst) {
  ScriptRig rig;
  rig.cfg.maxInFlight = 1;
  rig.cfg.agingNs = 300000000ull;
  SND_REQUIRE(rig.Init());
  rig.fs.AddFile("music.bin", MakePattern(1, kFile));
  rig.fs.AddFile("amb.bin", MakePattern(3, kFile));
  rig.fs.AddFile("voice.bin", MakePattern(2, kFile));
  rig.fs.SetLatency(0);
  // t = 0: ambience waits; music keeps the IO busy.
  SND_REQUIRE(rig.io.Post(StreamReq(2, "amb.bin", 0, kChunk, IoClass::Ambience, 0)));
  SND_REQUIRE(rig.io.Post(StreamReq(0, "music.bin", 0, kChunk, IoClass::Music, 0)));
  rig.io.RunOnce();  // music read issued (amb enqueued at t = 0)
  SND_CHECK_EQ(LastIssuedPath(rig), std::string("music.bin"));
  rig.clock.now.store(1);  // younger requests from here on
  SND_REQUIRE(rig.io.Post(StreamReq(1, "voice.bin", 0, kChunk, IoClass::Voice, 0)));
  SND_REQUIRE(rig.io.Post(StreamReq(0, "music.bin", kChunk, kChunk, IoClass::Music, 1)));
  rig.io.RunOnce();  // previous completes; voice/music enqueued at t = 1; music issued
  SND_CHECK_EQ(LastIssuedPath(rig), std::string("music.bin"));
  // One nanosecond before the ambience request ages: priority order holds.
  rig.clock.now.store(300000000ull - 1u);
  SND_REQUIRE(rig.io.Post(StreamReq(0, "music.bin", 2u * kChunk, kChunk, IoClass::Music, 2)));
  rig.io.RunOnce();
  SND_CHECK_EQ(LastIssuedPath(rig), std::string("music.bin"));
  SND_CHECK_EQ(rig.io.Stats().agedServed, 0u);
  // Exactly at the threshold: the ambience request (oldest) goes first.
  rig.clock.now.store(300000000ull);
  rig.io.RunOnce();
  SND_CHECK_EQ(LastIssuedPath(rig), std::string("amb.bin"));
  SND_CHECK_EQ(rig.io.Stats().agedServed, 1u);
  rig.io.RunOnce();
  SND_CHECK_EQ(LastIssuedPath(rig), std::string("voice.bin"));  // back to priority order
}

SND_TEST(IoEdge, AgingDoesNotLetAFloodOfOldLowRequestsBlockMusicForever) {
  // 40 ambience requests all aged; music posted later must still be served
  // after at most the requests that aged before it (header comment bound).
  ScriptRig rig(128);
  rig.cfg.maxStreams = 8;
  rig.cfg.ringChunks = 8;
  rig.cfg.maxInFlight = 1;
  rig.cfg.agingNs = 100000000ull;
  SND_REQUIRE(rig.Init());
  AddStd(rig);
  for (std::uint32_t s = 1; s < 6; ++s) {
    for (std::uint32_t i = 0; i < 8; ++i) SND_REQUIRE(rig.io.Post(StreamReq(s, "c.bin", i * kChunk, kChunk, IoClass::Ambience, i)));
  }
  rig.io.RunOnce();  // drained at t = 0, one issued
  rig.clock.now.store(200000000ull);
  SND_REQUIRE(rig.io.Post(StreamReq(0, "a.bin", 0, kChunk, IoClass::Music, 0)));
  std::uint32_t steps = 0;
  for (; steps < 200; ++steps) {
    rig.io.RunOnce();
    if (LastIssuedPath(rig) == "a.bin") break;
    for (std::uint32_t s = 1; s < 6; ++s) Drain(rig, s);
  }
  // Music waits for the 39 aged ambience reads, then it is served; and once
  // it has itself waited agingNs it is "aged" too, so the bound is finite.
  SND_CHECK(steps < 60u);
  snd::test::Note("music waited %u IO iterations behind aged ambience reads", steps);
}

// ===========================================================================
// Bandwidth budget
// ===========================================================================

namespace {
// Max bytes issued in any fixed window [t0 + k*w, t0 + (k+1)*w) and in any sliding window of length w.
void WindowMaxima(const std::vector<ScriptFs::Issue>& v, std::uint64_t t0, std::uint64_t w, std::uint64_t* fixedMax,
                  std::uint64_t* slidingMax, std::uint32_t* maxSingle) {
  *fixedMax = 0;
  *slidingMax = 0;
  *maxSingle = 0;
  for (std::size_t i = 0; i < v.size(); ++i) {
    if (v[i].bytes > *maxSingle) *maxSingle = v[i].bytes;
    std::uint64_t fixedSum = 0, slideSum = 0;
    const std::uint64_t k = (v[i].ns - t0) / w;
    for (std::size_t j = 0; j < v.size(); ++j) {
      if ((v[j].ns - t0) / w == k) fixedSum += v[j].bytes;
      if (v[j].ns >= v[i].ns && v[j].ns < v[i].ns + w) slideSum += v[j].bytes;
    }
    if (fixedSum > *fixedMax) *fixedMax = fixedSum;
    if (slideSum > *slidingMax) *slidingMax = slideSum;
  }
}
}  // namespace

SND_TEST(IoEdge, BudgetHoldsInEveryWindowUnderIrregularClockSteps) {
  ScriptRig rig(128);
  rig.cfg.maxInFlight = 8;
  rig.cfg.bytesPerWindow = 3u * kChunk;
  SND_REQUIRE(rig.Init());
  AddStd(rig);
  rig.fs.SetRandomLatency(2, 3);
  const std::uint64_t t0 = 5000000ull;
  rig.clock.now.store(t0);
  std::uint32_t posted = 0;
  const std::uint32_t steps[] = {7, 13, 61, 0, 19, 99, 1, 100, 33, 250, 3};
  for (std::uint32_t it = 0; it < 400; ++it) {
    for (std::uint32_t s = 0; s < 4; ++s) {
      while (rig.io.Channel(s).Outstanding() < rig.io.Channel(s).Capacity()) {
        const std::uint32_t bytes = 1u + (posted * 7919u) % kChunk;
        if (!rig.io.Post(StreamReq(s, "a.bin", (posted % 60u) * kChunk, bytes, static_cast<IoClass>(s % 3u), posted))) break;
        ++posted;
      }
      Drain(rig, s);
    }
    rig.io.RunOnce();
    rig.clock.AdvanceMs(steps[it % 11u]);
  }
  std::uint64_t fixedMax = 0, slidingMax = 0;
  std::uint32_t single = 0;
  WindowMaxima(rig.fs.Issues(), t0, 100000000ull, &fixedMax, &slidingMax, &single);
  SND_CHECK(fixedMax <= 3u * kChunk);
  SND_CHECK(rig.io.Stats().maxWindowBytes <= 3u * kChunk);
  SND_CHECK(rig.fs.Issues().size() > 50u);
  // Fixed windows are what the implementation promises; a sliding 100 ms
  // window can see up to twice the budget (see the report: spec gap).
  snd::test::Note("fixed-window max %llu, sliding-window max %llu, budget %u",
                  static_cast<unsigned long long>(fixedMax), static_cast<unsigned long long>(slidingMax), 3u * kChunk);
}

SND_TEST(IoEdge, BudgetDoesNotCarryOverIdleWindowsAndAllowsOneOversizeRead) {
  ScriptRig rig(64);
  rig.cfg.maxInFlight = 8;
  rig.cfg.bytesPerWindow = kChunk / 4u;  // every read is bigger than the budget
  SND_REQUIRE(rig.Init());
  AddStd(rig);
  rig.io.RunOnce();
  rig.clock.AdvanceMs(1000);  // 10 idle windows
  for (std::uint32_t i = 0; i < 6; ++i) SND_REQUIRE(rig.io.Post(StreamReq(0, "a.bin", i * kChunk, kChunk, IoClass::Music, i)));
  rig.io.RunOnce();
  rig.io.RunOnce();
  SND_CHECK_EQ(rig.fs.Issues().size(), 1u);  // one oversize read per window, no carry-over
  for (int w = 0; w < 5; ++w) {
    rig.clock.AdvanceMs(100);
    rig.io.RunOnce();
  }
  SND_CHECK_EQ(rig.fs.Issues().size(), 6u);
  rig.clock.AdvanceMs(100);
  FileRequest q;
  q.path = "b.bin";
  SND_REQUIRE(rig.io.Submit(q));
  rig.io.RunOnce();
  SND_CHECK(q.State() == FileState::Done);
  SND_CHECK_EQ(q.Result().fileSize, kFile);
}

SND_TEST(IoEdge, BudgetExtremesAndClockJumps) {
  for (std::uint32_t budget : {0u, 1u, 0xFFFFFFFFu}) {
    ScriptRig rig(64);
    rig.cfg.maxInFlight = 4;
    rig.cfg.bytesPerWindow = budget;
    SND_REQUIRE(rig.Init());
    AddStd(rig);
    for (std::uint32_t i = 0; i < 8; ++i) SND_REQUIRE(rig.io.Post(StreamReq(0, "a.bin", i * kChunk, kChunk, IoClass::Music, i)));
    rig.clock.now.store(1000);
    rig.io.RunOnce();
    const std::size_t first = rig.fs.Issues().size();
    if (budget == 1u) SND_CHECK_EQ(first, 1u);
    else SND_CHECK_EQ(first, 4u);
    // Huge forward jump.
    rig.clock.now.store(1000000000000000ull);
    for (int i = 0; i < 20; ++i) {
      rig.io.RunOnce();
      rig.clock.AdvanceMs(100);
    }
    SND_CHECK_EQ(Drain(rig, 0).size(), 8u);
    SND_CHECK_EQ(rig.pool.Used(), 0u);
  }
  // Clock going backwards (a contract violation of IoClock: must be monotonic):
  // nothing breaks, and IO resumes at the latest once the clock is past the
  // window it had reached.
  ScriptRig rig(64);
  rig.cfg.maxInFlight = 8;
  rig.cfg.bytesPerWindow = kChunk;
  SND_REQUIRE(rig.Init());
  AddStd(rig);
  for (std::uint32_t i = 0; i < 4; ++i) SND_REQUIRE(rig.io.Post(StreamReq(0, "a.bin", i * kChunk, kChunk, IoClass::Music, i)));
  rig.clock.now.store(10000000000ull);  // 10 s
  rig.io.RunOnce();
  SND_CHECK_EQ(rig.fs.Issues().size(), 1u);
  rig.clock.now.store(1000000000ull);  // back to 1 s
  std::uint32_t it = 0;
  for (; it < 200 && Drain(rig, 0).size() == 0u && rig.fs.Issues().size() < 4u; ++it) {
    rig.io.RunOnce();
    rig.clock.AdvanceMs(100);
  }
  for (int i = 0; i < 10; ++i) rig.io.RunOnce();
  snd::test::Note("after the clock went back 9 s, the next read waited %u windows", it);
  std::uint64_t fixedMax = 0, slidingMax = 0;
  std::uint32_t single = 0;
  WindowMaxima(rig.fs.Issues(), 0, 100000000ull, &fixedMax, &slidingMax, &single);
  SND_CHECK(rig.fs.Issues().size() >= 2u);
}

SND_TEST(IoEdge, RejectedReadsDoNotConsumeTheBudget) {
  ScriptRig rig;
  rig.cfg.bytesPerWindow = kChunk;
  SND_REQUIRE(rig.Init());
  AddStd(rig);
  rig.fs.RejectNextReads(3);
  SND_REQUIRE(rig.io.Post(StreamReq(0, "a.bin", 0, kChunk, IoClass::Music, 0)));
  for (int i = 0; i < 6; ++i) rig.io.RunOnce();  // same window
  SND_CHECK_EQ(rig.fs.RejectedReads(), 3u);
  SND_CHECK_EQ(Drain(rig, 0).size(), 1u);
  SND_CHECK_EQ(rig.io.Stats().fsRejects, 3u);
  SND_CHECK_EQ(rig.pool.Used(), 0u);
}

// ===========================================================================
// Stall flag
// ===========================================================================

SND_TEST(IoEdge, StallThresholdOncePerRequestAndClearsOnCancel) {
  ScriptRig rig;
  rig.cfg.stallNs = 200000000ull;
  SND_REQUIRE(rig.Init());
  AddStd(rig);
  rig.fs.Hold(true);
  SND_REQUIRE(rig.io.Post(StreamReq(0, "a.bin", 0, kChunk, IoClass::Music, 0)));
  SND_REQUIRE(rig.io.Post(StreamReq(0, "a.bin", kChunk, kChunk, IoClass::Music, 1)));
  rig.io.RunOnce();  // enqueued at t = 0
  rig.clock.now.store(200000000ull - 1u);
  rig.io.RunOnce();
  SND_CHECK(!rig.io.Channel(0).IoStalled());
  rig.clock.now.store(200000000ull);
  rig.io.RunOnce();
  SND_CHECK(rig.io.Channel(0).IoStalled());
  SND_CHECK_EQ(rig.io.Channel(0).IoStallEvents(), 2u);  // two requests, once each
  rig.clock.AdvanceMs(1000);
  rig.io.RunOnce();
  rig.io.RunOnce();
  SND_CHECK_EQ(rig.io.Channel(0).IoStallEvents(), 2u);
  // Cancel: the stale in-flight reads must not keep the flag up.
  rig.io.Channel(0).Cancel();
  rig.io.RunOnce();
  SND_CHECK(!rig.io.Channel(0).IoStalled());
  // A new request after the cancel: not stalled yet.
  SND_REQUIRE(rig.io.Post(StreamReq(0, "a.bin", 0, kChunk, IoClass::Music, 2)));
  rig.io.RunOnce();
  SND_CHECK(!rig.io.Channel(0).IoStalled());
  rig.fs.Hold(false);
  rig.Run();
  SND_CHECK(!rig.io.Channel(0).IoStalled());
  SND_CHECK_EQ(Drain(rig, 0).size(), 1u);
  SND_CHECK_EQ(rig.io.Stats().stallEvents, 2u);
  SND_CHECK_EQ(rig.pool.Used(), 0u);
}

SND_TEST(IoEdge, StallFlagDropsWhenTheLateDataArrives) {
  ScriptRig rig;
  rig.cfg.stallNs = 50000000ull;
  SND_REQUIRE(rig.Init());
  AddStd(rig);
  rig.fs.Hold(true);
  SND_REQUIRE(rig.io.Post(StreamReq(3, "a.bin", 0, kChunk, IoClass::Voice, 0)));
  rig.io.RunOnce();
  rig.clock.AdvanceMs(60);
  rig.io.RunOnce();
  SND_CHECK(rig.io.Channel(3).IoStalled());
  rig.fs.Hold(false);
  rig.io.RunOnce();  // completes and is delivered in this iteration
  SND_CHECK(!rig.io.Channel(3).IoStalled());
  SND_CHECK_EQ(Drain(rig, 3).size(), 1u);
}

// ===========================================================================
// Errors and misbehaving file systems
// ===========================================================================

SND_TEST(IoEdge, ErrorsOfAllKindsArriveInRequestOrderWithoutLeaks) {
  ScriptRig rig;
  rig.cfg.maxInFlight = 4;
  SND_REQUIRE(rig.Init());
  AddStd(rig);
  rig.fs.AddFile("empty.bin", {});
  rig.fs.AddFile("noopen.bin", MakePattern(5, 100));
  rig.fs.FailOpen("noopen.bin");
  const std::uint32_t c = rig.pool.Acquire(0);
  // tag: 1 ok, 2 missing, 3 read error (caller chunk), 4 empty file, 5 crosses EOF, 6 past EOF, 7 cannot open, 8 ok
  SND_REQUIRE(rig.io.Post(StreamReq(0, "a.bin", 0, kChunk, IoClass::Music, 1)));
  SND_REQUIRE(rig.io.Post(StreamReq(0, "nothere.bin", 0, kChunk, IoClass::Music, 2)));
  rig.io.RunOnce();  // 1 issued
  rig.fs.FailNextReads(1);
  SND_REQUIRE(rig.io.Post(StreamReq(0, "b.bin", 0, kChunk, IoClass::Music, 3, rig.pool.Data(c))));
  SND_REQUIRE(rig.io.Post(StreamReq(0, "empty.bin", 0, kChunk, IoClass::Music, 4)));
  SND_REQUIRE(rig.io.Post(StreamReq(0, "a.bin", kFile - 100u, kChunk, IoClass::Music, 5)));
  SND_REQUIRE(rig.io.Post(StreamReq(0, "a.bin", kFile + 5u, kChunk, IoClass::Music, 6)));
  SND_REQUIRE(rig.io.Post(StreamReq(0, "noopen.bin", 0, 50, IoClass::Music, 7)));
  SND_REQUIRE(rig.io.Post(StreamReq(0, "c.bin", 0, kChunk, IoClass::Music, 8)));
  rig.Run();
  std::vector<ChunkRecord> recs;
  Drain(rig, 0, &recs);
  SND_REQUIRE_EQ(recs.size(), 8u);
  for (std::uint32_t i = 0; i < 8; ++i) SND_CHECK_EQ(recs[i].tag, i + 1u);
  SND_CHECK(recs[0].status == ChunkStatus::Ok);
  SND_CHECK(recs[1].status == ChunkStatus::OpenFailed);
  SND_CHECK(recs[2].status == ChunkStatus::ReadError);
  SND_CHECK(recs[3].status == ChunkStatus::Ok && recs[3].bytes == 0u);
  SND_CHECK(recs[4].status == ChunkStatus::Ok && recs[4].bytes == 100u);
  SND_CHECK(recs[5].status == ChunkStatus::Ok && recs[5].bytes == 0u);
  SND_CHECK(recs[6].status == ChunkStatus::OpenFailed);
  SND_CHECK(recs[7].status == ChunkStatus::Ok);
  for (const ChunkRecord& r : recs) {
    if (r.status != ChunkStatus::Ok) SND_CHECK_EQ(r.chunk, kInvalidChunk);
  }
  SND_CHECK_EQ(rig.io.Channel(0).Errors(), 3u);
  SND_CHECK_EQ(rig.pool.Used(), 0u);
  SND_CHECK_EQ(rig.io.Channel(0).Outstanding(), 0u);
  SND_CHECK(rig.fs.Violations().empty());
}

SND_TEST(IoEdge, OverReportingFileSystemIsClamped) {
  ScriptRig rig;
  SND_REQUIRE(rig.Init());
  AddStd(rig);
  rig.fs.SetOverReport(1000000);
  SND_REQUIRE(rig.io.Post(StreamReq(0, "a.bin", 0, 100, IoClass::Music, 0)));
  std::vector<std::uint8_t> dst(5000 + 64, 0xAB);
  FileRequest req;
  req.path = "b.bin";
  req.bytes = 5000;
  req.dst = dst.data();
  SND_REQUIRE(rig.io.Submit(req));
  rig.Run();
  std::vector<ChunkRecord> recs;
  Drain(rig, 0, &recs);
  SND_REQUIRE_EQ(recs.size(), 1u);
  SND_CHECK_EQ(recs[0].bytes, 100u);
  SND_CHECK(req.State() == FileState::Done);
  SND_CHECK_EQ(req.Result().bytesRead, 5000u);
  SND_CHECK(MatchesPattern(2, 0, dst.data(), 5000));
  for (std::size_t i = 5000; i < dst.size(); ++i) SND_CHECK_EQ(dst[i], 0xABu);
}

SND_TEST(IoEdge, UnknownTokenFromPollEndsTheReadAsAnError) {
  ScriptRig rig;
  SND_REQUIRE(rig.Init());
  AddStd(rig);
  rig.fs.UnknownNextCompletions(2);
  SND_REQUIRE(rig.io.Post(StreamReq(0, "a.bin", 0, kChunk, IoClass::Music, 0)));
  std::vector<std::uint8_t> dst(kChunk);
  FileRequest req;
  req.path = "b.bin";
  req.bytes = kChunk;
  req.dst = dst.data();
  req.ioClass = IoClass::Music;
  SND_REQUIRE(rig.io.Submit(req));
  rig.Run();
  std::vector<ChunkRecord> recs;
  Drain(rig, 0, &recs);
  SND_REQUIRE_EQ(recs.size(), 1u);
  SND_CHECK(recs[0].status == ChunkStatus::ReadError);
  SND_CHECK(req.State() == FileState::Error);
  SND_CHECK_EQ(rig.pool.Used(), 0u);
  SND_CHECK_EQ(rig.fs.InFlight(), 0u);
  // The scheduler does not poll retired tokens again.
  SND_CHECK_EQ(rig.fs.UnknownPolls(), 0u);
}

SND_TEST(IoEdge, PersistentlyRejectingFileSystemDoesNotLoseRequests) {
  ScriptRig rig;
  SND_REQUIRE(rig.Init());
  AddStd(rig);
  rig.fs.SetRejectAll(true);
  for (std::uint32_t i = 0; i < 4; ++i) SND_REQUIRE(rig.io.Post(StreamReq(0, "a.bin", i * kChunk, kChunk, IoClass::Music, i)));
  for (int i = 0; i < 200; ++i) {
    SND_CHECK(rig.io.RunOnce());  // work remains
    rig.clock.AdvanceMs(10);
  }
  SND_CHECK_EQ(rig.io.Channel(0).Ready(), 0u);
  SND_CHECK(rig.pool.Used() <= 1u);  // at most the head keeps its borrowed chunk
  rig.fs.SetRejectAll(false);
  rig.Run();
  SND_CHECK_EQ(Drain(rig, 0).size(), 4u);
  SND_CHECK_EQ(rig.pool.Used(), 0u);
}

// ===========================================================================
// FileRequest
// ===========================================================================

SND_TEST(IoEdge, FileRequestSizeQueryRangeEofAndHugeSplit) {
  ScriptRig rig;
  rig.cfg.filePieceBytes = 4096;
  SND_REQUIRE(rig.Init());
  const std::uint32_t big = 3u * 1024u * 1024u + 77u;
  rig.fs.AddFile("big.bin", MakePattern(6, big));
  rig.fs.AddFile("zero.bin", {});
  CallbackLog log;
  // Size query: no read, no budget.
  FileRequest q;
  q.path = "big.bin";
  q.onComplete = &CallbackLog::Fn;
  q.user = &log;
  SND_REQUIRE(rig.io.Submit(q));
  rig.Run();
  SND_CHECK(q.State() == FileState::Done);
  SND_CHECK_EQ(q.Result().fileSize, static_cast<std::uint64_t>(big));
  SND_CHECK_EQ(q.Result().bytesRead, 0u);
  SND_CHECK_EQ(rig.fs.ReadCalls(), 0u);
  SND_CHECK_EQ(log.calls.load(), 1u);
  // Whole file in 4 KiB pieces.
  std::vector<std::uint8_t> dst(big + 10u);
  FileRequest r;
  r.path = "big.bin";
  r.bytes = big + 10u;  // past EOF: stops early
  r.dst = dst.data();
  r.onComplete = &CallbackLog::Fn;
  r.user = &log;
  SND_REQUIRE(rig.io.Submit(r));
  rig.Run(100000);
  SND_CHECK(r.State() == FileState::Done);
  SND_CHECK_EQ(r.Result().bytesRead, static_cast<std::uint64_t>(big));
  SND_CHECK(MatchesPattern(6, 0, dst.data(), big));
  SND_CHECK_EQ(log.calls.load(), 2u);
  SND_CHECK_EQ(rig.fs.ReadCalls(), (big + 4095u) / 4096u);
  // Range starting past EOF, zero-size file.
  FileRequest p;
  p.path = "big.bin";
  p.offset = big + 1u;
  p.bytes = 10;
  p.dst = dst.data();
  SND_REQUIRE(rig.io.Submit(p));
  FileRequest z;
  z.path = "zero.bin";
  z.bytes = 10;
  z.dst = dst.data() + 100;
  SND_REQUIRE(rig.io.Submit(z));
  rig.Run();
  SND_CHECK(p.State() == FileState::Done);
  SND_CHECK_EQ(p.Result().bytesRead, 0u);
  SND_CHECK(z.State() == FileState::Done);
  SND_CHECK_EQ(z.Result().bytesRead, 0u);
  SND_CHECK_EQ(z.Result().fileSize, 0u);
  // Reuse after completion.
  r.offset = 1000;
  r.bytes = 5000;
  SND_REQUIRE(rig.io.Submit(r));
  rig.Run();
  SND_CHECK_EQ(r.Result().bytesRead, 5000u);
  SND_CHECK(MatchesPattern(6, 1000, dst.data(), 5000));
  SND_CHECK(rig.fs.Violations().empty());
}

SND_TEST(IoEdge, FileRequestRejectsInvalidAndBusyAndFullQueue) {
  ScriptRig rig;
  rig.cfg.fileQueueCapacity = 2;
  SND_REQUIRE(rig.Init());
  AddStd(rig);
  std::uint8_t buf[16];
  FileRequest none;
  SND_CHECK(!rig.io.Submit(none));  // no path
  FileRequest zero;
  zero.path = "a.bin";
  zero.dst = buf;
  SND_CHECK(!rig.io.Submit(zero));  // dst with 0 bytes
  SND_CHECK(zero.State() == FileState::Idle);
  FileRequest a, b, c;
  a.path = b.path = c.path = "a.bin";
  SND_CHECK(rig.io.Submit(a));
  SND_CHECK(!rig.io.Submit(a));  // in use
  SND_CHECK(rig.io.Submit(b));
  SND_CHECK(!rig.io.Submit(c));  // queue full
  SND_CHECK(c.State() == FileState::Idle);
  rig.Run();
  SND_CHECK(a.Finished() && b.Finished());
  SND_CHECK(rig.io.Submit(c));
  rig.Run();
  SND_CHECK(c.State() == FileState::Done);
}

namespace {
struct Chain {
  snd::io::IoScheduler* io;
  FileRequest req;
  std::uint32_t remaining;
  std::uint32_t calls;
  std::uint32_t submitFailures;
  static void Fn(void* user, const FileResult& r) noexcept {
    auto* c = static_cast<Chain*>(user);
    ++c->calls;
    if (r.state != FileState::Done) return;
    if (c->remaining == 0u) return;
    --c->remaining;
    c->req.offset += 100u;
    if (!c->io->Submit(c->req)) ++c->submitFailures;  // resubmit the same request from its callback
  }
};
}  // namespace

SND_TEST(IoEdge, FileRequestCallbackMaySubmitAgain) {
  ScriptRig rig;
  rig.cfg.fileQueueCapacity = 1;
  SND_REQUIRE(rig.Init());
  AddStd(rig);
  std::vector<std::uint8_t> dst(100);
  Chain chain{&rig.io, {}, 5, 0, 0};
  chain.req.path = "b.bin";
  chain.req.bytes = 100;
  chain.req.dst = dst.data();
  chain.req.onComplete = &Chain::Fn;
  chain.req.user = &chain;
  SND_REQUIRE(rig.io.Submit(chain.req));
  rig.Run();
  SND_CHECK_EQ(chain.calls, 6u);
  SND_CHECK_EQ(chain.submitFailures, 0u);
  SND_CHECK(chain.req.State() == FileState::Done);
  SND_CHECK(MatchesPattern(2, 500, dst.data(), 100));
}

SND_TEST(IoEdge, FileRequestCancelAtEveryStageCallsBackOnce) {
  for (int stage = 0; stage < 4; ++stage) {
    ScriptRig rig;
    rig.cfg.filePieceBytes = 1024;
    SND_REQUIRE(rig.Init());
    AddStd(rig);
    rig.fs.SetLatency(2);
    std::vector<std::uint8_t> dst(8192);
    CallbackLog log;
    FileRequest r;
    r.path = "a.bin";
    r.bytes = 8192;
    r.dst = dst.data();
    r.onComplete = &CallbackLog::Fn;
    r.user = &log;
    SND_REQUIRE(rig.io.Submit(r));
    for (int i = 0; i < stage * 2; ++i) rig.io.RunOnce();  // 0: queued, 1+: pieces in flight
    rig.io.Cancel(r);
    rig.Run();
    SND_CHECK(r.Finished());
    SND_CHECK(r.State() == FileState::Cancelled);
    SND_CHECK_EQ(log.calls.load(), 1u);
    SND_CHECK(log.last.state == FileState::Cancelled);
    SND_CHECK(r.Result().bytesRead < 8192u);
    SND_CHECK(MatchesPattern(1, 0, dst.data(), static_cast<std::uint32_t>(r.Result().bytesRead)));
    SND_CHECK_EQ(rig.fs.InFlight(), 0u);
  }
}

// ===========================================================================
// Lifecycle
// ===========================================================================

SND_TEST(IoEdge, ShutdownFromAMixedStateEndsEverythingAndDestroyReturnsAll) {
  ScriptRig rig(16);
  rig.cfg.maxInFlight = 2;
  SND_REQUIRE(rig.Init());
  AddStd(rig);
  // Ring-arrived records on channel 0.
  for (std::uint32_t i = 0; i < 3; ++i) SND_REQUIRE(rig.io.Post(StreamReq(0, "a.bin", i * kChunk, kChunk, IoClass::Music, i)));
  rig.Run();
  ChunkRecord held{};
  SND_REQUIRE(rig.io.Channel(0).Pop(held));  // one held by the consumer
  // In flight (slow) on channel 1, queued on channel 2, queued file request.
  rig.fs.SetLatency(3);
  for (std::uint32_t i = 0; i < 2; ++i) SND_REQUIRE(rig.io.Post(StreamReq(1, "b.bin", i * kChunk, kChunk, IoClass::Music, i)));
  rig.io.RunOnce();
  const std::uint32_t mine = rig.pool.Acquire(2);
  for (std::uint32_t i = 0; i < 3; ++i) SND_REQUIRE(rig.io.Post(StreamReq(2, "c.bin", i * kChunk, kChunk, IoClass::Voice, i)));
  SND_REQUIRE(rig.io.Post(StreamReq(2, "c.bin", 9u * kChunk, kChunk, IoClass::Voice, 9, rig.pool.Data(mine))));
  std::vector<std::uint8_t> dst(1000);
  CallbackLog log;
  FileRequest fr;
  fr.path = "a.bin";
  fr.bytes = 1000;
  fr.dst = dst.data();
  fr.onComplete = &CallbackLog::Fn;
  fr.user = &log;
  SND_REQUIRE(rig.io.Submit(fr));
  rig.io.Shutdown();
  SND_CHECK_EQ(rig.fs.InFlight(), 0u);
  SND_CHECK_EQ(rig.fs.OpenFiles(), 0u);
  SND_CHECK(fr.State() == FileState::Cancelled);
  SND_CHECK_EQ(log.calls.load(), 1u);
  SND_CHECK_EQ(rig.io.Channel(1).Outstanding(), 0u);
  SND_CHECK_EQ(rig.io.Channel(2).Outstanding(), 0u);
  SND_CHECK_EQ(rig.io.Channel(0).Outstanding(), 3u);  // held + 2 in the ring
  SND_CHECK(!rig.io.Channel(1).IoStalled());
  SND_CHECK(!rig.io.RunOnce());
  SND_CHECK(!rig.io.Start());
  SND_CHECK(!rig.io.Running());
  rig.io.Shutdown();  // twice
  SND_CHECK(!rig.io.Submit(fr));
  SND_CHECK_EQ(rig.pool.Used(), 3u);
  rig.io.Channel(0).Release(held);
  rig.io.Destroy();  // returns the 2 ring records
  SND_CHECK_EQ(rig.pool.Used(), 0u);
  SND_CHECK_EQ(rig.pool.Stats().badReleases, 0u);
  SND_CHECK(rig.fs.Violations().empty());
  rig.io.Destroy();  // twice
  rig.io.Shutdown();  // after Destroy
}

SND_TEST(IoEdge, InitFailureAtEveryAllocationLeaksNothing) {
  snd::base::AccountingAllocator acc(snd::base::DefaultAllocator());
  FailingAllocator fail(acc);
  ScriptFs fs;
  snd::io::StreamPool pool;
  SND_REQUIRE(pool.Init(acc, 8u * kChunk, kChunk, 4));
  snd::io::IoSchedulerConfig cfg;
  cfg.maxStreams = 4;
  // Count the allocations of a successful Init.
  fail.Arm(-1);
  {
    snd::io::IoScheduler io;
    SND_REQUIRE(io.Init(fail, fs, pool, cfg));
  }
  const std::uint64_t base = acc.TotalUsed();
  std::int64_t n = 0;
  for (;; ++n) {
    fail.Arm(n);
    snd::io::IoScheduler io;
    const bool ok = io.Init(fail, fs, pool, cfg);
    if (!ok) SND_CHECK_EQ(acc.TotalUsed(), base);
    if (!ok) SND_CHECK(!io.Post(StreamReq(0, "a", 0, 10, IoClass::Music)));
    io.Destroy();
    SND_CHECK_EQ(acc.TotalUsed(), base);
    if (ok) break;
    if (n > 1000) break;
  }
  snd::test::Note("Init made %lld allocations", static_cast<long long>(n));
  // Same for the pool.
  pool.Destroy();
  for (std::int64_t k = 0; k < 10; ++k) {
    fail.Arm(k);
    snd::io::StreamPool p;
    const bool ok = p.Init(fail, 8u * kChunk, kChunk, 4);
    if (!ok) SND_CHECK_EQ(acc.TotalUsed(), 0u);
    p.Destroy();
    if (ok) break;
  }
  SND_CHECK_EQ(acc.TotalUsed(), 0u);
}

SND_TEST(IoEdge, ReinitAfterDestroyStartsClean) {
  ScriptRig rig;
  SND_REQUIRE(rig.Init());
  AddStd(rig);
  rig.io.Channel(0).Cancel();
  rig.io.Channel(0).Cancel();
  SND_REQUIRE(rig.io.Post(StreamReq(0, "a.bin", 0, kChunk, IoClass::Music, 0)));
  rig.Run();
  rig.io.Destroy();  // the record in the ring goes back
  SND_CHECK_EQ(rig.pool.Used(), 0u);
  SND_REQUIRE(rig.io.Init(rig.alloc, rig.fs, rig.pool, rig.cfg));
  SND_CHECK_EQ(rig.io.Channel(0).Outstanding(), 0u);
  SND_CHECK_EQ(rig.io.Channel(0).Delivered(), 0u);
  SND_REQUIRE(rig.io.Post(StreamReq(0, "a.bin", kChunk, kChunk, IoClass::Music, 1)));
  rig.Run();
  std::vector<ChunkRecord> recs;
  Drain(rig, 0, &recs);
  SND_REQUIRE_EQ(recs.size(), 1u);
  SND_CHECK_EQ(recs[0].tag, 1u);
  SND_CHECK_EQ(rig.io.Stats().streamPosted, 1u);
}

// ===========================================================================
// RT safety of the RT-side API with the IO thread running
// ===========================================================================

SND_TEST(IoEdge, RtSideApiIsRtSafeWithTheThreadRunning) {
  snd::base::AccountingAllocator alloc(snd::base::DefaultAllocator());
  snd::mock::MemoryFileSystem fs;
  fs.AddFile("a.bin", MakePattern(1, kFile));
  snd::io::StreamPool pool;
  SND_REQUIRE(pool.Init(alloc, 16u * kChunk, kChunk, 4));
  snd::io::IoScheduler io;
  snd::io::IoSchedulerConfig cfg;
  cfg.maxStreams = 4;
  cfg.pollIntervalNs = 200000;
  SND_REQUIRE(io.Init(alloc, fs, pool, cfg));
  SND_REQUIRE(io.Start());
  snd::test::rt::Reset();
  std::uint32_t got = 0;
  std::uint32_t counts[8];
  for (int round = 0; round < 200 && got < 100u; ++round) {
    {
      SND_RT_SCOPE();
      snd::io::StreamChannel& ch = io.Channel(1);
      const std::uint32_t c = pool.Acquire(1);
      void* dst = c != kInvalidChunk ? pool.Data(c) : nullptr;
      if (!snd::io::IoScheduler::PostIo(&io, StreamReq(1, "a.bin", 0, kChunk, IoClass::Music, 0, dst)) && c != kInvalidChunk) {
        pool.Release(c);
      }
      (void)io.Post(StreamReq(1, "a.bin", kChunk, kChunk, IoClass::Music, 1));
      io.Wake();
      (void)ch.Epoch();
      (void)ch.Outstanding();
      (void)ch.ReadPos();
      (void)ch.IoStalled();
      (void)ch.IoStallEvents();
      (void)ch.Errors();
      (void)ch.Delivered();
      (void)ch.Ready();
      (void)pool.HeldBy(1);
      (void)pool.Holdings(counts, 8);
      (void)pool.Stats();
      (void)pool.IsBorrowed(0);
      (void)pool.IndexOf(pool.Data(0));
      (void)io.Stats();
      (void)io.DrainEpoch();
      ChunkRecord r{};
      (void)ch.Peek();
      while (ch.Pop(r)) {
        ch.Release(r);
        ++got;
      }
      if (round % 17 == 0) {
        ch.Cancel();
        (void)ch.Reclaim();
      }
    }
    snd::platform::SleepFor(500000);
  }
  SND_CHECK(got >= 50u);
  SND_CHECK_EQ(snd::test::rt::UnexpectedTotal(), 0u);
  io.Channel(1).Cancel();
  io.Shutdown();
  SND_CHECK(io.Channel(1).Reclaim());
  io.Destroy();
  SND_CHECK_EQ(pool.Used(), 0u);
}
