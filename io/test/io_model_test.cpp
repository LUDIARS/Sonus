// Model-based randomized test of the IO scheduler, single thread (RunOnce).
//
// Random operation sequences (fixed seeds) over Post (pool-borrowed and
// caller-borrowed chunks, invalid requests), Cancel (seek), Pop, Peek,
// Release, Cancel+Reclaim, RunOnce, clock advance, fault injection (read
// errors, rejected reads, failing opens, missing files, reads past EOF),
// pool starvation by the test holding chunks, and Shutdown in the middle.
// After every operation the books are checked against a model:
//   - pool: Used == sum of the per-owner books; chunks held by the test and by
//     the consumer are borrowed and distinct; the scheduler holds at most one
//     chunk per outstanding request;
//   - every delivered record is the next request of the current epoch of its
//     channel (request order, no stale data, no gaps), with the right size and data;
//   - the file system is used as the contract says (ScriptFs checks).
// At the end everything must be delivered, then every chunk must be back.
#include <cstdint>
#include <cstdio>
#include <deque>
#include <string>
#include <vector>

#include "io_adversarial_util.hpp"
#include "snd/io/io_scheduler.hpp"
#include "snd/test/test.hpp"

using namespace snd::iotest;
using snd::io::ChunkRecord;
using snd::io::ChunkStatus;
using snd::io::IoClass;
using snd::io::kInvalidChunk;

namespace {

constexpr std::uint32_t kMChunk = 1024;
constexpr std::uint32_t kSlots = 4;
const char* const kMPaths[] = {"m0.bin", "m1.bin", "m2.bin", "missing.bin", "badopen.bin"};
const std::uint64_t kMSizes[] = {5u * kMChunk + 123u, 3u * kMChunk, 1u, 0u, 100u};

struct Rng {
  std::uint64_t s;
  std::uint32_t Next() noexcept {
    s ^= s << 13;
    s ^= s >> 7;
    s ^= s << 17;
    return static_cast<std::uint32_t>(s >> 16);
  }
  std::uint32_t Below(std::uint32_t n) noexcept { return n == 0u ? 0u : Next() % n; }
};

struct Expected {
  std::uint32_t tag;
  std::uint64_t offset;
  std::uint32_t bytes;
  std::uint32_t epoch;
  std::uint32_t file;  // index into kMPaths
};

std::uint64_t gRecordsChecked = 0;
std::uint64_t gOkRecords = 0;
std::uint64_t gErrorRecords = 0;

struct Held {
  std::uint32_t slot;
  ChunkRecord rec;
};

struct TestChunk {
  std::uint32_t chunk;
  std::uint32_t owner;
};

struct ModelRun {
  std::uint32_t seed;
  std::string failure;  // first failure (empty = ok)
  bool chunkDeadlock = false;  // pool exhausted by queued caller chunks (see IoDefect test)
  void Fail(const char* what, std::uint32_t step) {
    if (!failure.empty()) return;
    char buf[256];
    std::snprintf(buf, sizeof buf, "seed %u step %u: %s", seed, step, what);
    failure = buf;
  }
};

std::uint32_t ExpectedBytes(const Expected& e) noexcept {
  const std::uint64_t size = kMSizes[e.file];
  if (e.offset >= size) return 0u;
  const std::uint64_t avail = size - e.offset;
  return avail < e.bytes ? static_cast<std::uint32_t>(avail) : e.bytes;
}

void RunModel(ModelRun& run, std::uint32_t steps, bool allowShutdown) {
  Rng rng{0x9E3779B97F4A7C15ull ^ (static_cast<std::uint64_t>(run.seed) * 0x100000001B3ull)};
  for (int i = 0; i < 4; ++i) rng.Next();
  const std::uint32_t poolChunks = 3u + rng.Below(14);
  const std::uint32_t dstMode = run.seed % 3u;
  ScriptRig rig(poolChunks, kMChunk);
  rig.cfg.maxStreams = kSlots;
  rig.cfg.ringChunks = 1u + rng.Below(8);
  rig.cfg.maxInFlight = 1u + rng.Below(4);
  const std::uint32_t budgets[] = {0u, kMChunk / 2u, 3u * kMChunk, 1u};
  rig.cfg.bytesPerWindow = budgets[rng.Below(4)];
  rig.cfg.agingNs = (1u + rng.Below(4)) * 50000000ull;
  rig.cfg.stallNs = (1u + rng.Below(4)) * 50000000ull;
  // Small file table (eviction of idle entries). With fewer entries than paths
  // a full table is reported as OpenFailed (documented on ChunkStatus).
  rig.cfg.maxOpenFiles = 2u + rng.Below(4);
  const bool tableCanFill = rig.cfg.maxOpenFiles < 5u;
  rig.cfg.requestQueueCapacity = 4u + rng.Below(16);
  for (std::uint32_t f = 0; f < 3; ++f) {
    rig.fs.AddFile(kMPaths[f], MakePattern(f + 1u, static_cast<std::size_t>(kMSizes[f])));
  }
  rig.fs.AddFile(kMPaths[4], MakePattern(5, static_cast<std::size_t>(kMSizes[4])));
  rig.fs.FailOpen(kMPaths[4]);
  rig.fs.SetRandomLatency(rng.Below(4), run.seed);
  if (!rig.Init()) {
    run.Fail("Init failed", 0);
    return;
  }
  snd::io::StreamPool& pool = rig.pool;
  snd::io::IoScheduler& io = rig.io;

  std::deque<Expected> expected[kSlots];
  std::uint32_t epoch[kSlots] = {};
  std::vector<Held> held;
  std::vector<TestChunk> mine;
  std::uint32_t nextTag = 1;
  bool shut = false;
  const std::uint32_t shutdownAt = allowShutdown ? steps / 4u + rng.Below(steps / 2u) : 0xFFFFFFFFu;

  auto checkRecord = [&](std::uint32_t slot, const ChunkRecord& rec, std::uint32_t step, bool remove) {
    if (expected[slot].empty()) {
      run.Fail("record delivered that was never requested in the current epoch (stale or duplicate)", step);
      return;
    }
    const Expected e = expected[slot].front();
    ++gRecordsChecked;
    if (rec.status == ChunkStatus::Ok) ++gOkRecords; else ++gErrorRecords;
    if (rec.tag != e.tag || rec.offset != e.offset || rec.requested != e.bytes) {
      run.Fail("record out of request order / wrong request", step);
      return;
    }
    if (rec.epoch != e.epoch || rec.epoch != io.Channel(slot).Epoch()) run.Fail("record of a stale epoch", step);
    if (e.file >= 3u) {
      if (rec.status != ChunkStatus::OpenFailed) run.Fail("missing / unopenable file not reported OpenFailed", step);
    } else if (rec.status == ChunkStatus::Ok) {
      if (rec.chunk == kInvalidChunk || !pool.IsBorrowed(rec.chunk)) run.Fail("Ok record without a borrowed chunk", step);
      if (rec.bytes != ExpectedBytes(e)) run.Fail("wrong byte count (EOF handling)", step);
      if (rec.chunk != kInvalidChunk && !MatchesPattern(e.file + 1u, e.offset, pool.Data(rec.chunk), rec.bytes)) {
        run.Fail("wrong data", step);
      }
    } else if (rec.status == ChunkStatus::ReadError) {
      if (rec.chunk != kInvalidChunk) run.Fail("ReadError record carries a chunk", step);
    } else if (!tableCanFill) {
      run.Fail("OpenFailed for a file that exists", step);
    }
    if (remove) expected[slot].pop_front();
  };

  auto checkBooks = [&](std::uint32_t step) {
    const std::uint32_t used = pool.Used();
    if (used != rig.HeldSum()) run.Fail("pool Used != sum of HeldBy books", step);
    std::uint32_t consumerChunks = 0;
    std::uint32_t heldPer[kSlots] = {};
    std::vector<std::uint32_t> seen;
    for (const Held& h : held) {
      ++heldPer[h.slot];
      if (h.rec.chunk == kInvalidChunk) continue;
      ++consumerChunks;
      if (!pool.IsBorrowed(h.rec.chunk)) run.Fail("consumer-held chunk is free in the pool", step);
      seen.push_back(h.rec.chunk);
    }
    for (const TestChunk& t : mine) {
      if (!pool.IsBorrowed(t.chunk) || pool.OwnerOf(t.chunk) != t.owner) run.Fail("caller-held chunk lost / re-owned", step);
      seen.push_back(t.chunk);
    }
    for (std::size_t a = 0; a < seen.size(); ++a) {
      for (std::size_t b = a + 1u; b < seen.size(); ++b) {
        if (seen[a] == seen[b]) run.Fail("one chunk owned twice", step);
      }
    }
    const std::uint32_t outside = consumerChunks + static_cast<std::uint32_t>(mine.size());
    if (used < outside) {
      run.Fail("pool Used below the chunks held outside the scheduler", step);
      return;
    }
    std::uint32_t outstandingFree = 0;
    for (std::uint32_t s = 0; s < kSlots; ++s) {
      const snd::io::StreamChannel& ch = io.Channel(s);
      if (ch.Outstanding() > ch.Capacity()) run.Fail("Outstanding above Capacity", step);
      if (ch.Outstanding() < heldPer[s] + ch.Ready()) run.Fail("Outstanding below held + ready records", step);
      outstandingFree += ch.Outstanding() - heldPer[s];
    }
    if (used - outside > outstandingFree) run.Fail("scheduler holds more chunks than outstanding requests (leak)", step);
  };

  for (std::uint32_t step = 0; step < steps && run.failure.empty(); ++step) {
    if (step == shutdownAt && !shut) {
      io.Shutdown();
      shut = true;
      // Nothing is left inside the scheduler: what is outstanding is in the rings or held.
      for (std::uint32_t s = 0; s < kSlots; ++s) {
        std::uint32_t h = 0;
        for (const Held& x : held) h += x.slot == s ? 1u : 0u;
        if (io.Channel(s).Outstanding() != h + io.Channel(s).Ready()) run.Fail("Shutdown left requests inside the scheduler", step);
      }
      if (rig.fs.InFlight() != 0u) run.Fail("Shutdown returned with reads in flight", step);
      if (rig.fs.OpenFiles() != 0u) run.Fail("Shutdown left files open", step);
    }
    const std::uint32_t op = rng.Below(20);
    const std::uint32_t slot = rng.Below(kSlots);
    snd::io::StreamChannel& ch = io.Channel(slot);
    switch (op) {
      case 0: case 1: case 2: case 3: case 4: {  // valid Post
        std::uint32_t file = rng.Below(10) == 0u ? 3u + rng.Below(2) : rng.Below(3);
        const std::uint64_t size = kMSizes[file];
        std::uint64_t offset = rng.Below(4) == 0u ? rng.Below(static_cast<std::uint32_t>(size) + 2u * kMChunk)
                                                  : (rng.Below(6) * kMChunk);
        const std::uint32_t bytes = 1u + rng.Below(kMChunk);
        void* dst = nullptr;
        std::uint32_t myChunk = kInvalidChunk;
        std::uint32_t myOwner = slot;
        // dstMode 0: always dst = nullptr; 1: always a caller chunk (when one is free); 2: mixed.
        if (dstMode == 1u || (dstMode == 2u && rng.Below(2) == 0u)) {
          myOwner = rng.Below(3) == 0u ? snd::io::kNoOwner : slot;
          myChunk = pool.Acquire(myOwner);
          if (myChunk != kInvalidChunk) {
            dst = pool.Data(myChunk);
            mine.push_back(TestChunk{myChunk, myOwner});
          } else if (dstMode == 1u) {
            break;  // caller-chunk-only run: no chunk, no post
          }
        }
        const std::uint32_t tag = nextTag++;
        const bool ok = io.Post(StreamReq(slot, kMPaths[file], offset, bytes, static_cast<IoClass>(rng.Below(3)), tag, dst));
        if (shut && ok) run.Fail("Post accepted after Shutdown", step);
        if (ok) {
          expected[slot].push_back(Expected{tag, offset, bytes, epoch[slot], file});
          if (myChunk != kInvalidChunk) mine.pop_back();  // ownership passed to the scheduler
        }
        // Rejected: the caller keeps the chunk (checked in checkBooks).
        break;
      }
      case 5: {  // invalid Post: must be refused and change nothing
        const std::uint32_t before = ch.Outstanding();
        const std::uint32_t kind = rng.Below(5);
        snd::source::IoReadRequest r = StreamReq(slot, kMPaths[0], 0, 16, IoClass::Music, nextTag);
        std::uint32_t c = kInvalidChunk;
        if (kind == 0) r.bytes = 0;
        if (kind == 1) r.bytes = kMChunk + 1u;
        if (kind == 2) r.streamSlot = kSlots + rng.Below(3);
        if (kind == 3 || kind == 4) {
          c = pool.Acquire(slot);
          if (c == kInvalidChunk) break;
          r.dst = kind == 3 ? static_cast<void*>(pool.Data(c) + 1) : static_cast<void*>(pool.Data(c));
          if (kind == 4) pool.Release(c);  // a free chunk
        }
        if (io.Post(r)) {
          run.Fail("invalid Post accepted", step);
          expected[slot].push_back(Expected{nextTag, 0, 16, epoch[slot], 0});
        }
        ++nextTag;
        if (kind == 3) {
          if (!pool.IsBorrowed(c)) run.Fail("refused Post released the caller's chunk", step);
          pool.Release(c);
        }
        if (ch.Outstanding() != before) run.Fail("refused Post changed Outstanding", step);
        break;
      }
      case 6: {  // Cancel (seek / stop)
        ch.Cancel();
        ++epoch[slot];
        expected[slot].clear();
        if (ch.Epoch() != epoch[slot]) run.Fail("Epoch did not advance by one", step);
        if (rng.Below(3) == 0u) ch.Cancel(), ++epoch[slot];  // repeated cancel
        break;
      }
      case 7: case 8: case 9: {  // Pop
        ChunkRecord rec{};
        while (ch.Pop(rec)) {
          checkRecord(slot, rec, step, true);
          held.push_back(Held{slot, rec});
          if (rng.Below(2) == 0u) break;
        }
        break;
      }
      case 10: {  // Peek
        const ChunkRecord* p = ch.Peek();
        if (p != nullptr) {
          const ChunkRecord copy = *p;
          checkRecord(slot, copy, step, false);
          if (ch.Peek() == nullptr || ch.Peek()->tag != copy.tag) run.Fail("Peek removed or changed the record", step);
        }
        break;
      }
      case 11: case 12: {  // Release a held record
        if (held.empty()) break;
        const std::uint32_t k = rng.Below(static_cast<std::uint32_t>(held.size()));
        io.Channel(held[k].slot).Release(held[k].rec);
        held.erase(held.begin() + k);
        break;
      }
      case 13: {  // Cancel then Reclaim (stream released)
        ch.Cancel();
        ++epoch[slot];
        expected[slot].clear();
        const bool clean = ch.Reclaim();
        std::uint32_t h = 0;
        for (const Held& x : held) h += x.slot == slot ? 1u : 0u;
        if (clean && h != 0u) run.Fail("Reclaim reported clean while records are held", step);
        if (ch.Ready() != 0u) run.Fail("Reclaim left records in the ring", step);
        break;
      }
      case 14: case 15: {  // IO thread iterations
        const std::uint32_t n = 1u + rng.Below(3);
        for (std::uint32_t k = 0; k < n; ++k) io.RunOnce();
        break;
      }
      case 16: {  // time
        rig.clock.AdvanceMs(rng.Below(160));
        break;
      }
      case 17: {  // faults
        const std::uint32_t f = rng.Below(3);
        if (f == 0) rig.fs.FailNextReads(1u + rng.Below(2));
        if (f == 1) rig.fs.RejectNextReads(1u + rng.Below(3));
        if (f == 2) rig.fs.SetMaxInFlight(rng.Below(3));
        break;
      }
      case 18: {  // the test borrows a chunk (starves the pool) or gives one back
        if (rng.Below(2) == 0u) {
          const std::uint32_t c = pool.Acquire(slot);
          if (c != kInvalidChunk) mine.push_back(TestChunk{c, slot});
        } else if (!mine.empty()) {
          pool.Release(mine.back().chunk);
          mine.pop_back();
        }
        break;
      }
      default: {  // status reads must be consistent (no checks beyond books)
        (void)ch.ReadPos();
        (void)ch.IoStalled();
        (void)io.Stats();
        break;
      }
    }
    checkBooks(step);
  }
  if (!run.failure.empty()) return;

  // Drain: give everything back and run until every request is delivered.
  for (const TestChunk& t : mine) pool.Release(t.chunk);
  mine.clear();
  for (const Held& h : held) io.Channel(h.slot).Release(h.rec);
  held.clear();
  rig.fs.SetMaxInFlight(0);
  for (std::uint32_t it = 0; it < 4000 && run.failure.empty(); ++it) {
    const bool more = io.RunOnce();
    std::uint32_t left = 0;
    for (std::uint32_t s = 0; s < kSlots; ++s) {
      ChunkRecord rec{};
      while (io.Channel(s).Pop(rec)) {
        checkRecord(s, rec, 1000000u + it, true);
        io.Channel(s).Release(rec);
      }
      left += static_cast<std::uint32_t>(expected[s].size());
    }
    if (!more && left == 0u) break;
    if (!more && !shut && left != 0u) {
      run.Fail("scheduler idle but requests were never delivered", 1000000u + it);
      break;
    }
    if (shut) break;
    {
      const snd::io::IoStats st = io.Stats();
      if (more && dstMode == 2u && st.inFlight == 0u && st.pending != 0u && pool.Used() == pool.ChunkCount() &&
          it > 200u) {
        run.chunkDeadlock = true;  // nothing can be issued, nothing will come back
        return;
      }
    }
    rig.clock.AdvanceMs(100);
  }
  if (!run.failure.empty()) return;
  if (!shut) {
    for (std::uint32_t s = 0; s < kSlots; ++s) {
      if (!expected[s].empty()) {
        const snd::io::IoStats st = io.Stats();
        char buf[320];
        std::snprintf(buf, sizeof buf,
                      "requests never delivered: slot %u left %zu (first tag %u off %llu) outstanding %u ready %u; "
                      "pending %u inFlight %u fsInFlight %u poolUsed %u/%u starved %llu rejects %llu maxInFlight %u",
                      s, expected[s].size(), expected[s].front().tag,
                      static_cast<unsigned long long>(expected[s].front().offset), io.Channel(s).Outstanding(),
                      io.Channel(s).Ready(), st.pending, st.inFlight, rig.fs.InFlight(), pool.Used(), pool.ChunkCount(),
                      static_cast<unsigned long long>(st.poolStarved), static_cast<unsigned long long>(st.fsRejects),
                      rig.cfg.maxInFlight);
        run.Fail(buf, 2000000u);
      }
    }
  }
  for (std::uint32_t s = 0; s < kSlots; ++s) {
    io.Channel(s).Cancel();
    bool clean = false;
    for (int k = 0; k < 200 && !clean; ++k) {
      clean = io.Channel(s).Reclaim();
      if (!clean) {
        io.RunOnce();
        rig.clock.AdvanceMs(100);
      }
    }
    if (!clean) run.Fail("channel never became clean after Cancel + Reclaim", 3000000u + s);
  }
  if (pool.Used() != 0u) run.Fail("chunks leaked after every request ended", 4000000u);
  const std::vector<std::string> v = rig.fs.Violations();
  if (!v.empty()) run.Fail(("file system contract: " + JoinViolations(v)).c_str(), 5000000u);
  io.Destroy();
  if (pool.Used() != 0u) run.Fail("chunks leaked after Destroy", 6000000u);
  if (pool.Stats().badReleases != 0u) run.Fail("bad releases counted", 6000001u);
  pool.Destroy();
  if (rig.alloc.TotalUsed() != 0u) run.Fail("memory leaked", 7000000u);
}

}  // namespace

SND_TEST(IoModel, RandomOperationSequencesKeepChunksAndOrder) {
  std::uint32_t failures = 0;
  std::uint32_t deadlocks = 0;
  for (std::uint32_t seed = 1; seed <= 300; ++seed) {
    ModelRun run{seed, {}};
    RunModel(run, 1500, false);
    if (run.chunkDeadlock) ++deadlocks;
    if (!run.failure.empty()) {
      if (++failures <= 5) SND_CHECK_MSG(run.failure.empty(), "%s", run.failure.c_str());
    }
  }
  SND_CHECK_EQ(failures, 0u);
  // Mixed dst modes can wedge the scheduler (defect reproduced minimally in
  // IoDefect.QueuedCallerChunksBehindChunklessHeadDeadlock); counted, not failed here.
  snd::test::Note("runs ending in the queued-caller-chunk deadlock: %u; records checked %llu (ok %llu, error %llu)",
                  deadlocks, static_cast<unsigned long long>(gRecordsChecked),
                  static_cast<unsigned long long>(gOkRecords), static_cast<unsigned long long>(gErrorRecords));
}

SND_TEST(IoModel, RandomSequencesWithShutdownInTheMiddle) {
  std::uint32_t failures = 0;
  std::uint32_t deadlocks = 0;
  for (std::uint32_t seed = 1001; seed <= 1200; ++seed) {
    ModelRun run{seed, {}};
    RunModel(run, 1200, true);
    if (run.chunkDeadlock) ++deadlocks;
    if (!run.failure.empty()) {
      if (++failures <= 5) SND_CHECK_MSG(run.failure.empty(), "%s", run.failure.c_str());
    }
  }
  SND_CHECK_EQ(failures, 0u);
  // Mixed dst modes can wedge the scheduler (defect reproduced minimally in
  // IoDefect.QueuedCallerChunksBehindChunklessHeadDeadlock); counted, not failed here.
  snd::test::Note("runs ending in the queued-caller-chunk deadlock: %u; records checked %llu (ok %llu, error %llu)",
                  deadlocks, static_cast<unsigned long long>(gRecordsChecked),
                  static_cast<unsigned long long>(gOkRecords), static_cast<unsigned long long>(gErrorRecords));
}
