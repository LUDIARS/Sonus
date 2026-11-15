#include "snd/io/io_scheduler.hpp"

#include <cstring>

#include "snd/base/mpmc.hpp"
#include "snd/base/pool.hpp"
#include "snd/platform/clock.hpp"
#include "snd/platform/semaphore.hpp"
#include "snd/platform/thread.hpp"

namespace snd::io {

namespace {

constexpr std::uint32_t kNil = 0xFFFFFFFFu;
constexpr std::uint32_t kMaxPathBytes = kMaxIoPathBytes;  // including the terminating NUL

enum class NodeKind : std::uint8_t { Stream = 0, File = 1 };
enum class NodeState : std::uint8_t { Pending = 0, InFlight = 1, Completed = 2 };

// One queued / in-flight unit of work. IO thread only.
struct Node {
  NodeKind kind = NodeKind::Stream;
  NodeState state = NodeState::Pending;
  IoClass cls = IoClass::Ambience;
  bool stallReported = false;
  ChunkStatus result = ChunkStatus::Ok;
  std::uint32_t prev = kNil, next = kNil;    // class pending list
  std::uint32_t cprev = kNil, cnext = kNil;  // channel order list (stream nodes)
  std::uint32_t file = kNil;                 // file table entry, kNil = unresolved (fails)
  std::uint64_t enqueueNs = 0;
  std::uint64_t seq = 0;  // acceptance order (aging tie-break)
  std::uint64_t token = 0;
  // stream
  std::uint32_t slot = 0, epoch = 0, tag = 0, chunk = kInvalidChunk, bytes = 0, resultBytes = 0;
  std::uint64_t offset = 0;
  // file
  FileRequest* req = nullptr;
  std::uint64_t done = 0;
  std::uint32_t piece = 0;
};

struct FileEntry {
  char path[kMaxPathBytes] = {};
  std::uint32_t hash = 0;
  FileId id = kInvalidFile;
  std::uint32_t refs = 0;
  std::uint64_t size = UINT64_MAX;
  std::uint64_t lastUse = 0;
  bool used = false;
  bool failed = false;
};

struct QueuedRead {
  source::IoReadRequest req;
  std::uint32_t epoch;
  std::uint32_t pad;
};

std::uint32_t HashPath(const char* p, std::size_t* len) noexcept {
  std::uint32_t h = 2166136261u;
  std::size_t n = 0;
  for (; p[n] != '\0'; ++n) {
    h ^= static_cast<std::uint8_t>(p[n]);
    h *= 16777619u;
  }
  *len = n;
  return h;
}

std::uint64_t MonotonicThunk(void*) noexcept { return platform::MonotonicNanoseconds(); }

IoClass ClassOfPriority(std::uint8_t p) noexcept {
  return p < static_cast<std::uint8_t>(IoClass::Count) ? static_cast<IoClass>(p) : IoClass::Ambience;
}

struct Counter {
  std::atomic<std::uint64_t> v{0};
  void Add(std::uint64_t n = 1) noexcept { v.fetch_add(n, std::memory_order_relaxed); }
  std::uint64_t Get() const noexcept { return v.load(std::memory_order_relaxed); }
};

}  // namespace

IoSchedulerConfig MakeIoSchedulerConfig(const snd_config& config) noexcept {
  IoSchedulerConfig c;
  c.maxStreams = config.max_streams;
  c.bytesPerWindow = config.tuning.io_bytes_per_100ms;
  c.windowNs = 100000000ull;
  if (config.tuning.stream_target_ahead_ms != 0u) {
    c.stallNs = static_cast<std::uint64_t>(config.tuning.stream_target_ahead_ms) * 1000000ull;
  }
  return c;
}

// ---------------------------------------------------------------------------
// Impl
// ---------------------------------------------------------------------------
struct IoScheduler::Impl {
  IoScheduler* self = nullptr;
  IAllocator* alloc = nullptr;
  IFileSystem* fs = nullptr;
  StreamPool* pool = nullptr;
  IoSchedulerConfig cfg{};

  base::MpmcRing<QueuedRead> streamQueue;
  base::MpmcRing<FileRequest*> fileQueue;
  std::atomic<std::uint32_t> fileOutstanding{0};
  std::atomic<std::uint32_t> accepting{0};
  std::atomic<std::uint32_t> stop{0};
  base::CachePadded<std::atomic<std::uint32_t>> wakePending;
  platform::Semaphore wake{0};
  platform::Thread thread;
  bool ready = false;  // Init completed
  bool started = false;
  bool shutDown = false;

  // --- IO thread private --------------------------------------------------------
  Node* nodes = nullptr;
  std::uint32_t nodeCount = 0;
  base::IndexFreeList freeNodes;
  std::uint32_t head[kIoClassCount] = {kNil, kNil, kNil, kNil};
  std::uint32_t tail[kIoClassCount] = {kNil, kNil, kNil, kNil};
  std::uint32_t* inflight = nullptr;
  std::uint32_t inflightCount = 0;
  std::uint32_t* chHead = nullptr;
  std::uint32_t* chTail = nullptr;
  FileEntry* files = nullptr;
  std::uint32_t fileCount = 0;
  std::uint64_t windowStart = 0;
  std::uint64_t windowUsed = 0;
  bool windowInit = false;
  bool windowWaited = false;
  std::uint64_t nextToken = 0;
  std::uint64_t nextSeq = 0;

  // --- statistics -----------------------------------------------------------------
  Counter readsIssued, bytesIssued, readsCompleted, bytesCompleted, readErrors, openFailures;
  Counter streamPosted, streamRejected, staleDropped, staleCompleted, fsRejects, budgetWaits;
  Counter poolStarved, agedServed, stallEvents, filesCompleted, maxWindowBytes;
  std::atomic<std::uint32_t> inflightStat{0};
  std::atomic<std::uint32_t> pendingStat{0};

  std::uint64_t Now() const noexcept { return cfg.clock.nowNs(cfg.clock.user); }

  // Per pool chunk: 1 while a caller chunk accepted by Post is in the
  // scheduler's hands (decision d: a second Post of the same dst is refused).
  // Cleared BEFORE the chunk leaves the scheduler (delivered to the ring or
  // released to the pool), so the next owner can post it again.
  std::atomic<std::uint8_t>* held = nullptr;
  std::uint32_t heldCount = 0;
  void Unhold(std::uint32_t chunk) noexcept {
    if (chunk < heldCount) held[chunk].store(0u, std::memory_order_release);
  }
  void ReleaseChunk(std::uint32_t chunk) noexcept {
    Unhold(chunk);
    pool->Release(chunk);
  }

  // Post / Submit calls between their `accepting` check and their queue push.
  std::atomic<std::uint32_t> inPost{0};
  StreamChannel& Ch(std::uint32_t slot) noexcept { return self->channels_[slot]; }

  // --- lists ----------------------------------------------------------------------------
  void PushBack(std::uint32_t i) noexcept {
    Node& n = nodes[i];
    const auto c = static_cast<std::uint32_t>(n.cls);
    n.prev = tail[c];
    n.next = kNil;
    if (tail[c] != kNil) nodes[tail[c]].next = i; else head[c] = i;
    tail[c] = i;
    pendingStat.fetch_add(1u, std::memory_order_relaxed);
  }
  void PushFront(std::uint32_t i) noexcept {
    Node& n = nodes[i];
    const auto c = static_cast<std::uint32_t>(n.cls);
    n.prev = kNil;
    n.next = head[c];
    if (head[c] != kNil) nodes[head[c]].prev = i; else tail[c] = i;
    head[c] = i;
    pendingStat.fetch_add(1u, std::memory_order_relaxed);
  }
  void Unlink(std::uint32_t i) noexcept {
    Node& n = nodes[i];
    const auto c = static_cast<std::uint32_t>(n.cls);
    if (n.prev != kNil) nodes[n.prev].next = n.next; else head[c] = n.next;
    if (n.next != kNil) nodes[n.next].prev = n.prev; else tail[c] = n.prev;
    n.prev = n.next = kNil;
    pendingStat.fetch_sub(1u, std::memory_order_relaxed);
  }
  void ChAppend(std::uint32_t i) noexcept {
    Node& n = nodes[i];
    n.cprev = chTail[n.slot];
    n.cnext = kNil;
    if (chTail[n.slot] != kNil) nodes[chTail[n.slot]].cnext = i; else chHead[n.slot] = i;
    chTail[n.slot] = i;
  }
  void ChUnlink(std::uint32_t i) noexcept {
    Node& n = nodes[i];
    if (n.cprev != kNil) nodes[n.cprev].cnext = n.cnext; else chHead[n.slot] = n.cnext;
    if (n.cnext != kNil) nodes[n.cnext].cprev = n.cprev; else chTail[n.slot] = n.cprev;
    n.cprev = n.cnext = kNil;
  }

  // --- file table -------------------------------------------------------------------------
  std::uint32_t ResolveFile(const char* path, std::uint64_t now) noexcept {
    if (path == nullptr) return kNil;
    std::size_t len = 0;
    const std::uint32_t h = HashPath(path, &len);
    if (len == 0u || len >= kMaxPathBytes) return kNil;
    std::uint32_t freeEntry = kNil;
    std::uint32_t idle = kNil;
    for (std::uint32_t i = 0; i < fileCount; ++i) {
      FileEntry& e = files[i];
      if (!e.used) {
        if (freeEntry == kNil) freeEntry = i;
        continue;
      }
      if (e.hash == h && std::strcmp(e.path, path) == 0 && !(e.failed && e.refs == 0u)) {
        ++e.refs;
        e.lastUse = now;
        return i;
      }
      if (e.refs == 0u && (idle == kNil || e.lastUse < files[idle].lastUse)) idle = i;
    }
    std::uint32_t pick = freeEntry != kNil ? freeEntry : idle;
    if (pick == kNil) return kNil;
    FileEntry& e = files[pick];
    if (e.used && e.id != kInvalidFile) fs->Close(e.id);  // idle: no reads in flight
    std::memcpy(e.path, path, len + 1u);
    e.hash = h;
    e.id = kInvalidFile;
    e.refs = 1;
    e.size = UINT64_MAX;
    e.lastUse = now;
    e.used = true;
    e.failed = false;
    return pick;
  }
  void ReleaseFile(std::uint32_t& i) noexcept {
    if (i == kNil) return;
    FileEntry& e = files[i];
    if (e.refs > 0u) --e.refs;
    if (e.refs == 0u && e.failed) e.used = false;  // retry the open for later requests
    i = kNil;
  }
  bool EnsureOpen(std::uint32_t i) noexcept {
    if (i == kNil) return false;
    FileEntry& e = files[i];
    if (e.id != kInvalidFile) return true;
    if (e.failed) return false;
    e.id = fs->Open(e.path);
    if (e.id == kInvalidFile) {
      e.failed = true;
      openFailures.Add();
      return false;
    }
    e.size = fs->Size(e.id);
    return true;
  }
  void CloseAllFiles() noexcept {
    for (std::uint32_t i = 0; i < fileCount; ++i) {
      FileEntry& e = files[i];
      if (e.used && e.id != kInvalidFile) fs->Close(e.id);
      e = FileEntry{};
    }
  }

  // --- node lifetime --------------------------------------------------------------------
  void FreeNode(std::uint32_t i) noexcept {
    nodes[i] = Node{};
    freeNodes.Push(i);
  }
  // Stream node leaves the system without delivery: chunk back, reservation ended.
  void DropStream(std::uint32_t i) noexcept {
    Node& n = nodes[i];
    if (n.state == NodeState::Pending) Unlink(i);
    ChUnlink(i);
    if (n.chunk != kInvalidChunk) ReleaseChunk(n.chunk);
    ReleaseFile(n.file);
    Ch(n.slot).Unreserve();
    FreeNode(i);
  }
  void FinishFile(FileRequest* req, FileState state, std::uint64_t bytesRead, std::uint64_t size) noexcept {
    const FileCallback cb = req->onComplete;
    void* user = req->user;
    req->bytesRead = bytesRead;
    req->fileSize = size;
    fileOutstanding.fetch_sub(1u, std::memory_order_acq_rel);
    filesCompleted.Add();
    // Last access to *req: the submitter may reuse or free it once it sees the state.
    req->state.store(static_cast<std::uint32_t>(state), std::memory_order_release);
    if (cb != nullptr) cb(user, FileResult{state, bytesRead, size});
  }
  void FinishFileNode(std::uint32_t i, FileState state) noexcept {
    Node& n = nodes[i];
    if (n.state == NodeState::Pending) Unlink(i);
    const std::uint64_t size = n.file != kNil ? files[n.file].size : UINT64_MAX;
    FileRequest* req = n.req;
    const std::uint64_t done = n.done;
    ReleaseFile(n.file);
    FreeNode(i);
    FinishFile(req, state, done, size);
  }

  // --- intake ---------------------------------------------------------------------------
  void DrainInputs(std::uint64_t now, bool cancelAll) noexcept {
    QueuedRead q;
    while (streamQueue.TryPop(q)) {
      StreamChannel& ch = Ch(q.req.streamSlot);
      const std::uint32_t chunk = q.req.dst != nullptr ? pool->IndexOf(q.req.dst) : kInvalidChunk;
      const std::uint32_t idx = (cancelAll || q.epoch != ch.Epoch()) ? kNil : freeNodes.Pop();
      if (idx == base::kInvalidIndex || idx == kNil) {
        if (chunk != kInvalidChunk) ReleaseChunk(chunk);
        ch.Unreserve();
        staleDropped.Add();
        continue;
      }
      Node& n = nodes[idx];
      n = Node{};
      n.kind = NodeKind::Stream;
      n.cls = ClassOfPriority(q.req.priority);
      n.enqueueNs = now;
      n.seq = ++nextSeq;
      n.slot = q.req.streamSlot;
      n.epoch = q.epoch;
      n.tag = q.req.chunkIndex;
      n.chunk = chunk;
      n.bytes = q.req.bytes;
      n.offset = q.req.offset;
      n.file = ResolveFile(q.req.path, now);
      ChAppend(idx);
      if (n.file == kNil) {  // bad path or table full: fails in order
        n.state = NodeState::Completed;
        n.result = ChunkStatus::OpenFailed;
        openFailures.Add();
        if (n.chunk != kInvalidChunk) {
          ReleaseChunk(n.chunk);
          n.chunk = kInvalidChunk;
        }
      } else {
        PushBack(idx);
      }
    }
    FileRequest* req = nullptr;
    while (fileQueue.TryPop(req)) {
      if (cancelAll || req->cancel.load(std::memory_order_acquire) != 0u) {
        FinishFile(req, FileState::Cancelled, 0, UINT64_MAX);
        continue;
      }
      const std::uint32_t file = ResolveFile(req->path, now);
      const std::uint32_t idx = file == kNil ? kNil : freeNodes.Pop();
      if (file == kNil || idx == base::kInvalidIndex) {
        std::uint32_t f = file;
        ReleaseFile(f);
        if (file == kNil) openFailures.Add();
        FinishFile(req, FileState::Error, 0, UINT64_MAX);
        continue;
      }
      Node& n = nodes[idx];
      n = Node{};
      n.kind = NodeKind::File;
      n.cls = req->ioClass < IoClass::Count ? req->ioClass : IoClass::Bank;
      n.enqueueNs = now;
      n.seq = ++nextSeq;
      n.req = req;
      n.file = file;
      PushBack(idx);
    }
    self->drainEpoch_.value.fetch_add(1u, std::memory_order_acq_rel);
  }

  // Cancelled file requests waiting in the class lists.
  void SweepCancelledFiles() noexcept {
    for (std::uint32_t c = 0; c < kIoClassCount; ++c) {
      std::uint32_t i = head[c];
      while (i != kNil) {
        const std::uint32_t next = nodes[i].next;
        if (nodes[i].kind == NodeKind::File && nodes[i].req->cancel.load(std::memory_order_acquire) != 0u) {
          FinishFileNode(i, FileState::Cancelled);
        }
        i = next;
      }
    }
  }

  // --- delivery (in request order per channel) ------------------------------------------------
  void Deliver(std::uint32_t slot) noexcept {
    StreamChannel& ch = Ch(slot);
    const std::uint32_t epoch = ch.Epoch();
    std::uint32_t i = chHead[slot];
    while (i != kNil) {
      Node& n = nodes[i];
      const std::uint32_t next = n.cnext;
      if (n.epoch != epoch) {
        // Stale: never a barrier. In-flight reads are discarded when they complete.
        if (n.state != NodeState::InFlight) {
          if (n.state == NodeState::Pending) staleDropped.Add();
          DropStream(i);
        }
        i = next;
        continue;
      }
      if (n.state != NodeState::Completed) break;
      ChunkRecord rec{};
      rec.offset = n.offset;
      rec.chunk = n.result == ChunkStatus::Ok ? n.chunk : kInvalidChunk;
      rec.bytes = n.result == ChunkStatus::Ok ? n.resultBytes : 0u;
      rec.requested = n.bytes;
      rec.tag = n.tag;
      rec.epoch = n.epoch;
      rec.status = n.result;
      if (n.result != ChunkStatus::Ok && n.chunk != kInvalidChunk) ReleaseChunk(n.chunk);
      ChUnlink(i);
      ReleaseFile(n.file);
      FreeNode(i);
      if (rec.status == ChunkStatus::Ok) {
        ch.readPos_.store(StreamChannel::PackReadPos(rec.epoch, rec.offset + rec.bytes), std::memory_order_release);
      } else {
        ch.errors_.fetch_add(1u, std::memory_order_relaxed);
      }
      if (rec.chunk != kInvalidChunk) Unhold(rec.chunk);  // ownership passes to the consumer
      if (!ch.ring_.TryPush(rec)) {
        // Cannot happen (reservations bound the ring); keep the books balanced anyway.
        if (rec.chunk != kInvalidChunk) ReleaseChunk(rec.chunk);
        ch.Unreserve();
      } else {
        ch.delivered_.fetch_add(1u, std::memory_order_release);
      }
      i = next;
    }
  }

  // Delivers what became ready on every channel and drops stale queued work of
  // cancelled channels, so their reservations come back quickly.
  void DeliverAll() noexcept {
    for (std::uint32_t s = 0; s < self->channelCount_; ++s) {
      if (chHead[s] != kNil) Deliver(s);
    }
  }

  // --- completion --------------------------------------------------------------------------
  void PollInFlight() noexcept {
    std::uint32_t k = 0;
    while (k < inflightCount) {
      const std::uint32_t i = inflight[k];
      Node& n = nodes[i];
      const IoStatus st = fs->Poll(IoToken{n.token});
      const IoState s = StateOf(st);
      if (s == IoState::Pending) {
        ++k;
        continue;
      }
      inflight[k] = inflight[--inflightCount];
      inflightStat.store(inflightCount, std::memory_order_relaxed);
      const bool ok = s == IoState::Done;
      if (ok) {
        readsCompleted.Add();
        bytesCompleted.Add(st.bytes);
      } else {
        readErrors.Add();
      }
      if (n.kind == NodeKind::Stream) {
        StreamChannel& ch = Ch(n.slot);
        if (n.epoch != ch.Epoch()) {
          staleCompleted.Add();
          n.state = NodeState::Completed;  // not in a class list any more
          DropStream(i);
          continue;
        }
        n.state = NodeState::Completed;
        n.result = ok ? ChunkStatus::Ok : ChunkStatus::ReadError;
        n.resultBytes = ok ? (st.bytes < n.bytes ? st.bytes : n.bytes) : 0u;
        ReleaseFile(n.file);
        Deliver(n.slot);
      } else {
        n.state = NodeState::Pending;
        if (!ok) {
          n.state = NodeState::Completed;
          FinishFileNode(i, FileState::Error);
          continue;
        }
        const std::uint32_t got = st.bytes < n.piece ? st.bytes : n.piece;
        n.done += got;
        const bool eof = got < n.piece;
        if (eof || n.done >= n.req->bytes) {
          n.state = NodeState::Completed;
          FinishFileNode(i, FileState::Done);
          continue;
        }
        if (n.req->cancel.load(std::memory_order_acquire) != 0u) {
          n.state = NodeState::Completed;
          FinishFileNode(i, FileState::Cancelled);
          continue;
        }
        // Next piece keeps its place at the front of its class, but waits on its own
        // for aging (a long bank load must not become "aged" and pre-empt music).
        n.enqueueNs = Now();
        PushFront(i);
      }
    }
  }

  // --- issue --------------------------------------------------------------------------------
  enum class IssueResult { Issued, Finished, PoolEmpty, FsBusy };

  std::uint32_t Cost(const Node& n) const noexcept {
    if (n.kind == NodeKind::Stream) return n.bytes;
    if (n.req->dst == nullptr) return 0u;
    return FilePiece(n);
  }
  std::uint32_t FilePiece(const Node& n) const noexcept {
    const std::uint64_t left = n.req->bytes > n.done ? n.req->bytes - n.done : 0u;
    std::uint64_t piece = cfg.filePieceBytes == 0u ? (1u << 20) : cfg.filePieceBytes;
    if (cfg.bytesPerWindow != 0u && piece > cfg.bytesPerWindow) piece = cfg.bytesPerWindow;
    if (piece > left) piece = left;
    if (piece > 0x7FFFFFFFu) piece = 0x7FFFFFFFu;
    return static_cast<std::uint32_t>(piece);
  }

  // Pool chunks are handed out in CHANNEL ORDER: before stream node i borrows
  // a chunk, every earlier current-epoch Pending node of its channel gets one
  // first (oldest first). Invariant: a node holding a chunk the IO thread
  // borrowed has all earlier current requests of its channel holding a chunk
  // too (or already issued / completed). Such a channel can always complete
  // and deliver its chunk-holding requests in order, so a borrowed chunk never
  // waits forever behind a request that cannot get one. The previous code gave
  // the chunk to whichever request was issued first: after a class change
  // inside a channel the later (higher-class) request took the last chunk and
  // then waited for delivery behind the earlier one, which could never get a
  // chunk (IoDefect.ClassChangeInsideAChannelDeadlocksWithoutCallerChunks).
  // A later request may still be ISSUED first (priority), once the earlier ones
  // hold their chunks. On a failed Acquire the chunks already granted stay with
  // the earlier requests (the invariant holds for them).
  bool GrantChunksInOrder(std::uint32_t i) noexcept {
    Node& n = nodes[i];
    const std::uint32_t epoch = Ch(n.slot).Epoch();
    for (std::uint32_t j = chHead[n.slot]; j != kNil && j != i; j = nodes[j].cnext) {
      Node& m = nodes[j];
      if (m.state != NodeState::Pending || m.epoch != epoch || m.chunk != kInvalidChunk) continue;
      m.chunk = pool->Acquire(m.slot);
      if (m.chunk == kInvalidChunk) return false;
    }
    n.chunk = pool->Acquire(n.slot);
    return n.chunk != kInvalidChunk;
  }

  IssueResult Issue(std::uint32_t i) noexcept {
    Node& n = nodes[i];
    Unlink(i);
    if (n.kind == NodeKind::Stream) {
      StreamChannel& ch = Ch(n.slot);
      if (n.epoch != ch.Epoch()) {
        n.state = NodeState::Completed;
        staleDropped.Add();
        DropStream(i);
        return IssueResult::Finished;
      }
      if (!EnsureOpen(n.file)) {
        n.state = NodeState::Completed;
        n.result = ChunkStatus::OpenFailed;
        ReleaseFile(n.file);
        Deliver(n.slot);
        return IssueResult::Finished;
      }
      if (n.chunk == kInvalidChunk && !GrantChunksInOrder(i)) {
        poolStarved.Add();
        PushFront(i);
        return IssueResult::PoolEmpty;
      }
      const std::uint64_t token = ++nextToken;
      if (!fs->ReadAsync(files[n.file].id, n.offset, pool->Data(n.chunk), n.bytes, IoToken{token})) {
        fsRejects.Add();
        PushFront(i);
        return IssueResult::FsBusy;
      }
      n.token = token;
      n.state = NodeState::InFlight;
      readsIssued.Add();
      bytesIssued.Add(n.bytes);
      inflight[inflightCount++] = i;
      inflightStat.store(inflightCount, std::memory_order_relaxed);
      return IssueResult::Issued;
    }
    // File request
    FileRequest* req = n.req;
    if (req->cancel.load(std::memory_order_acquire) != 0u) {
      n.state = NodeState::Completed;
      FinishFileNode(i, FileState::Cancelled);
      return IssueResult::Finished;
    }
    if (!EnsureOpen(n.file)) {
      n.state = NodeState::Completed;
      FinishFileNode(i, FileState::Error);
      return IssueResult::Finished;
    }
    const std::uint64_t size = files[n.file].size;
    const std::uint64_t pos = req->offset + n.done;
    if (req->dst == nullptr || n.done >= req->bytes || (size != UINT64_MAX && pos >= size)) {
      n.state = NodeState::Completed;
      FinishFileNode(i, FileState::Done);
      return IssueResult::Finished;
    }
    const std::uint32_t piece = FilePiece(n);
    const std::uint64_t token = ++nextToken;
    if (!fs->ReadAsync(files[n.file].id, pos, static_cast<std::uint8_t*>(req->dst) + n.done, piece,
                       IoToken{token})) {
      fsRejects.Add();
      PushFront(i);
      return IssueResult::FsBusy;
    }
    n.token = token;
    n.piece = piece;
    n.state = NodeState::InFlight;
    readsIssued.Add();
    bytesIssued.Add(piece);
    inflight[inflightCount++] = i;
    inflightStat.store(inflightCount, std::memory_order_relaxed);
    return IssueResult::Issued;
  }

  void RollWindow(std::uint64_t now) noexcept {
    if (!windowInit) {
      windowInit = true;
      windowStart = now;
      windowUsed = 0;
      return;
    }
    const std::uint64_t w = cfg.windowNs == 0u ? 1u : cfg.windowNs;
    if (now < windowStart) {
      // The clock went back (IoClock contract violation): restart the windows here
      // instead of waiting until the clock is past the old window again.
      windowStart = now;
      windowUsed = 0;
      windowWaited = false;
      return;
    }
    if (now - windowStart >= w) {
      windowStart += ((now - windowStart) / w) * w;
      windowUsed = 0;
      windowWaited = false;
    }
  }

  // A node can be issued now: file requests always; stream requests if they
  // already own a chunk or the pool may still have one (streamsBlocked = an
  // Acquire failed during this IssueLoop).
  bool Eligible(const Node& n, bool streamsBlocked) const noexcept {
    return n.kind != NodeKind::Stream || n.chunk != kInvalidChunk || !streamsBlocked;
  }
  bool Aged(const Node& n, std::uint64_t now) const noexcept {
    return n.enqueueNs <= now && now - n.enqueueNs >= cfg.agingNs;
  }
  // Selection (6.1):
  //  1. among ELIGIBLE nodes that waited agingNs: the longest waiting; ties:
  //     better class rank, then acceptance order (seq);
  //  2. otherwise the first eligible node of the best-ranked class that has one.
  // Every pending node is looked at, not only the class heads. The previous
  // version only looked at heads and skipped a whole class when its head had
  // no chunk and the pool was empty, so requests that DID own a chunk (caller
  // chunks, or a chunk kept after a refused ReadAsync) behind such a head were
  // never issued; when they held the last free chunks nothing ever completed
  // (IoDefect.QueuedCallerChunksBehindChunklessHeadDeadlock). Now an eligible
  // node is found wherever it is, so: whenever at least one pending request
  // can be issued, Select returns one (progress). Delivery order inside a
  // channel is unaffected (the channel list, Deliver()).
  std::uint32_t Select(std::uint64_t now, bool streamsBlocked, bool* aged) const noexcept {
    std::uint32_t best = kNil;
    std::uint32_t bestRank = 0;
    std::uint32_t oldest = kNil;
    for (std::uint32_t c = 0; c < kIoClassCount; ++c) {
      const std::uint32_t rank = cfg.classRank[c];
      bool firstTaken = false;
      for (std::uint32_t i = head[c]; i != kNil; i = nodes[i].next) {
        const Node& n = nodes[i];
        if (!Eligible(n, streamsBlocked)) continue;
        if (!firstTaken) {
          firstTaken = true;
          if (best == kNil || rank < bestRank) {
            best = i;
            bestRank = rank;
          }
        }
        if (Aged(n, now)) {
          if (oldest == kNil) {
            oldest = i;
          } else {
            const Node& o = nodes[oldest];
            const std::uint32_t orank = cfg.classRank[static_cast<std::uint32_t>(o.cls)];
            if (n.enqueueNs < o.enqueueNs || (n.enqueueNs == o.enqueueNs && (rank < orank || (rank == orank && n.seq < o.seq)))) {
              oldest = i;
            }
          }
        }
      }
    }
    if (oldest != kNil) {
      *aged = oldest != best;
      return oldest;
    }
    *aged = false;
    return best;
  }

  // Returns true when issuing stopped because of the bandwidth budget.
  bool IssueLoop(std::uint64_t now) noexcept {
    RollWindow(now);
    bool streamsBlocked = false;
    while (inflightCount < cfg.maxInFlight) {
      bool aged = false;
      const std::uint32_t i = Select(now, streamsBlocked, &aged);
      if (i == kNil) return false;
      const std::uint32_t cost = Cost(nodes[i]);
      // Cost-0 work (size queries, finished file requests) never waits for bandwidth.
      if (cost != 0u && cfg.bytesPerWindow != 0u && windowUsed > 0u && windowUsed + cost > cfg.bytesPerWindow) {
        if (!windowWaited) {
          windowWaited = true;
          budgetWaits.Add();
        }
        return true;
      }
      const IssueResult r = Issue(i);
      if (r == IssueResult::FsBusy) return false;
      if (r == IssueResult::PoolEmpty) {
        streamsBlocked = true;
        continue;
      }
      if (aged) agedServed.Add();
      if (r == IssueResult::Issued) {
        windowUsed += cost;
        if (windowUsed > maxWindowBytes.Get()) maxWindowBytes.v.store(windowUsed, std::memory_order_relaxed);
      }
    }
    return false;
  }

  // IO-side stall: a current request outstanding for stallNs or longer.
  void UpdateStalls(std::uint64_t now) noexcept {
    for (std::uint32_t s = 0; s < self->channelCount_; ++s) {
      StreamChannel& ch = Ch(s);
      const std::uint32_t epoch = ch.Epoch();
      bool stalled = false;
      for (std::uint32_t i = chHead[s]; i != kNil; i = nodes[i].cnext) {
        Node& n = nodes[i];
        if (n.epoch != epoch || n.state == NodeState::Completed) continue;
        if (now >= n.enqueueNs && now - n.enqueueNs >= cfg.stallNs) {
          stalled = true;
          if (!n.stallReported) {
            n.stallReported = true;
            ch.stallEvents_.fetch_add(1u, std::memory_order_relaxed);
            stallEvents.Add();
          }
        }
      }
      const std::uint32_t v = stalled ? 1u : 0u;
      if (ch.stalled_.load(std::memory_order_relaxed) != v) ch.stalled_.store(v, std::memory_order_release);
    }
  }

  bool HasWork() const noexcept {
    if (inflightCount != 0u) return true;
    // Requests accepted during this iteration (e.g. submitted from a completion
    // callback) are still in the input queues.
    if (streamQueue.SizeApprox() != 0u || fileQueue.SizeApprox() != 0u) return true;
    for (std::uint32_t c = 0; c < kIoClassCount; ++c) {
      if (head[c] != kNil) return true;
    }
    return false;
  }

  // One loop iteration. Returns true if work remains.
  bool Step() noexcept {
    // Lost-wakeup argument. Producer (Post/Submit/Cancel/FS notify): push to a
    // queue (or write the epoch), THEN Wake(): wakePending.exchange(1, acq_rel),
    // semaphore Post only if it read 0. IO thread: exchange(0, acq_rel) HERE,
    // then drain. Both are RMWs on one atomic, so they are totally ordered and
    // each reads the value written by the one before it:
    //  - producer's exchange first: ours reads its 1 and synchronises with it
    //    (release -> acquire), so the push is visible to the drain below;
    //  - ours first: the producer's exchange reads our 0 and posts the
    //    semaphore, so the next Wait/WaitFor returns and we drain again.
    // (The previous plain store(0) did not synchronise with the producer: a
    // store does not read, so the push could be invisible to this drain while
    // the producer saw the flag still 1 and did not post: a formal lost wakeup.)
    wakePending.value.exchange(0u, std::memory_order_acq_rel);
    const std::uint64_t now = Now();
    DrainInputs(now, false);
    SweepCancelledFiles();
    PollInFlight();
    DeliverAll();
    const bool budgetBlocked = IssueLoop(now);
    UpdateStalls(now);
    return budgetBlocked || HasWork();
  }

  // Shutdown: nothing new is issued; everything queued ends; in-flight reads
  // are waited for (the file system may still write into their buffers).
  void FinalDrain() noexcept {
    for (;;) {
      const std::uint64_t now = Now();
      DrainInputs(now, true);
      for (std::uint32_t c = 0; c < kIoClassCount; ++c) {
        while (head[c] != kNil) {
          const std::uint32_t i = head[c];
          if (nodes[i].kind == NodeKind::Stream) {
            staleDropped.Add();
            DropStream(i);
          } else {
            FinishFileNode(i, FileState::Cancelled);
          }
        }
      }
      // Completed-but-undelivered and in-flight stream nodes.
      std::uint32_t k = 0;
      while (k < inflightCount) {
        const std::uint32_t i = inflight[k];
        Node& n = nodes[i];
        const IoStatus st = fs->Poll(IoToken{n.token});
        if (StateOf(st) == IoState::Pending) {
          ++k;
          continue;
        }
        inflight[k] = inflight[--inflightCount];
        inflightStat.store(inflightCount, std::memory_order_relaxed);
        n.state = NodeState::Completed;
        if (n.kind == NodeKind::Stream) {
          staleCompleted.Add();
          DropStream(i);
        } else {
          FinishFileNode(i, FileState::Cancelled);
        }
      }
      if (inflightCount == 0u) break;
      platform::SleepFor(cfg.pollIntervalNs == 0u ? 100000u : cfg.pollIntervalNs);
    }
    for (std::uint32_t s = 0; s < self->channelCount_; ++s) {
      while (chHead[s] != kNil) DropStream(chHead[s]);
      Ch(s).stalled_.store(0u, std::memory_order_release);
    }
    CloseAllFiles();
  }

  static void ThreadMain(void* p) noexcept {
    Impl& im = *static_cast<Impl*>(p);
    while (im.stop.load(std::memory_order_acquire) == 0u) {
      const bool more = im.Step();
      if (im.stop.load(std::memory_order_acquire) != 0u) break;
      if (more) {
        im.wake.WaitFor(im.cfg.pollIntervalNs == 0u ? 100000u : im.cfg.pollIntervalNs);
      } else {
        im.wake.Wait();
      }
    }
    im.FinalDrain();
  }
};

// ---------------------------------------------------------------------------
// StreamChannel
// ---------------------------------------------------------------------------
void StreamChannel::Cancel() noexcept {
  epoch_.value.fetch_add(1u, std::memory_order_acq_rel);
  if (owner_ != nullptr) owner_->Wake();
}

bool StreamChannel::Pop(ChunkRecord& out) noexcept {
  const std::uint32_t epoch = Epoch();
  ChunkRecord rec{};
  while (ring_.TryPop(rec)) {
    if (rec.epoch == epoch) {
      out = rec;
      return true;
    }
    Release(rec);
  }
  return false;
}

const ChunkRecord* StreamChannel::Peek() noexcept {
  const std::uint32_t epoch = Epoch();
  for (;;) {
    const ChunkRecord* r = ring_.Peek();
    if (r == nullptr || r->epoch == epoch) return r;
    ChunkRecord stale{};
    if (ring_.TryPop(stale)) Release(stale);
  }
}

void StreamChannel::Release(const ChunkRecord& record) noexcept {
  if (record.chunk != kInvalidChunk && pool_ != nullptr) pool_->Release(record.chunk);
  Unreserve();
}

bool StreamChannel::Reclaim() noexcept {
  ChunkRecord rec{};
  while (ring_.TryPop(rec)) Release(rec);
  return Outstanding() == 0u;
}

// ---------------------------------------------------------------------------
// IoScheduler
// ---------------------------------------------------------------------------
bool IoScheduler::Init(IAllocator& allocator, IFileSystem& fs, StreamPool& pool,
                       const IoSchedulerConfig& config) noexcept {
  Destroy();
  if (!pool.Initialized() || config.maxStreams == 0u || config.ringChunks == 0u || config.maxInFlight == 0u) {
    return false;
  }
  alloc_ = &allocator;
  impl_ = base::New<Impl>(allocator, MemTag::Misc);
  if (impl_ == nullptr) {
    alloc_ = nullptr;
    return false;
  }
  Impl& im = *impl_;
  im.self = this;
  im.alloc = &allocator;
  im.fs = &fs;
  im.pool = &pool;
  im.cfg = config;
  if (im.cfg.clock.nowNs == nullptr) {
    im.cfg.clock.nowNs = &MonotonicThunk;
    im.cfg.clock.user = nullptr;
  }
  if (im.cfg.maxOpenFiles == 0u) im.cfg.maxOpenFiles = config.maxStreams + 16u;
  if (im.cfg.fileQueueCapacity == 0u) im.cfg.fileQueueCapacity = 1u;
  if (im.cfg.requestQueueCapacity == 0u) im.cfg.requestQueueCapacity = 1u;
  const std::uint32_t ring = static_cast<std::uint32_t>(base::NextPowerOfTwo(config.ringChunks < 2u ? 2u : config.ringChunks));

  bool ok = true;
  channelCount_ = config.maxStreams;
  channels_ = base::AllocateArray<StreamChannel>(allocator, channelCount_, MemTag::StreamPool);
  ok = ok && channels_ != nullptr;
  for (std::uint32_t s = 0; ok && s < channelCount_; ++s) {
    StreamChannel& ch = channels_[s];
    ok = ch.ring_.Init(allocator, ring, MemTag::StreamPool);
    ch.pool_ = &pool;
    ch.owner_ = this;
    ch.capacity_ = ring;
  }
  const std::uint64_t nodeCount = static_cast<std::uint64_t>(channelCount_) * ring + im.cfg.fileQueueCapacity;
  ok = ok && nodeCount < 0x7FFFFFFFu;
  if (ok) {
    im.nodeCount = static_cast<std::uint32_t>(nodeCount);
    im.nodes = base::AllocateArray<Node>(allocator, im.nodeCount, MemTag::Misc);
    ok = im.nodes != nullptr && im.freeNodes.Init(allocator, im.nodeCount, MemTag::Misc);
  }
  if (ok) {
    im.inflight = base::AllocateArray<std::uint32_t>(allocator, im.cfg.maxInFlight, MemTag::Misc);
    im.chHead = base::AllocateArray<std::uint32_t>(allocator, channelCount_, MemTag::Misc);
    im.chTail = base::AllocateArray<std::uint32_t>(allocator, channelCount_, MemTag::Misc);
    im.fileCount = im.cfg.maxOpenFiles;
    im.files = base::AllocateArray<FileEntry>(allocator, im.fileCount, MemTag::Misc);
    ok = im.inflight != nullptr && im.chHead != nullptr && im.chTail != nullptr && im.files != nullptr;
  }
  if (ok) {
    im.heldCount = pool.ChunkCount();
    im.held = base::AllocateArray<std::atomic<std::uint8_t>>(allocator, im.heldCount, MemTag::Misc);
    ok = im.held != nullptr;
  }
  ok = ok && im.streamQueue.Init(allocator, im.cfg.requestQueueCapacity, MemTag::Misc) &&
       im.fileQueue.Init(allocator, im.cfg.fileQueueCapacity, MemTag::Misc);
  if (!ok) {
    Destroy();
    return false;
  }
  for (std::uint32_t s = 0; s < channelCount_; ++s) im.chHead[s] = im.chTail[s] = kNil;
  drainEpoch_.value.store(0u, std::memory_order_relaxed);
  im.ready = true;
  im.accepting.store(1u, std::memory_order_release);
  return true;
}

bool IoScheduler::Start() noexcept {
  if (impl_ == nullptr || !impl_->ready || impl_->started || impl_->shutDown) return false;
  platform::ThreadDesc desc;
  desc.name = impl_->cfg.threadName != nullptr ? impl_->cfg.threadName : "snd-io";
  desc.priority = platform::ThreadPriority::Normal;
  if (!impl_->thread.Start(desc, &Impl::ThreadMain, impl_)) return false;
  impl_->started = true;
  return true;
}

bool IoScheduler::Running() const noexcept { return impl_ != nullptr && impl_->started && !impl_->shutDown; }

bool IoScheduler::Accepting() const noexcept {
  return impl_ != nullptr && impl_->accepting.load(std::memory_order_seq_cst) != 0u;
}

void IoScheduler::Shutdown() noexcept {
  if (impl_ == nullptr || impl_->shutDown) return;
  Impl& im = *impl_;
  // Post/Submit: inPost++ (seq_cst), load accepting (seq_cst), push, inPost--.
  // Here: store accepting = 0 (seq_cst), then wait for inPost == 0, and only
  // then let FinalDrain run. In the single total order of the seq_cst
  // operations either the call's load comes after our store (it sees 0 and
  // refuses), or its increment comes before our loads of inPost, which then
  // wait until its push is done (the decrement is a release, our load an
  // acquire): the request is in the queue before FinalDrain drains it. So a
  // request for which Post/Submit returned true always reaches a final state.
  // The previous code checked `accepting` and pushed later without this
  // handshake: Shutdown + FinalDrain could run in between, leaving an accepted
  // request in the queue until Destroy (IoDefect.ShutdownRacingWithPostAndSubmitStrandsNothing).
  im.accepting.store(0u, std::memory_order_seq_cst);
  while (im.inPost.load(std::memory_order_seq_cst) != 0u) platform::YieldThread();
  if (!im.ready) {
    im.shutDown = true;
    return;
  }
  if (im.started) {
    im.stop.store(1u, std::memory_order_release);
    im.wake.Post();
    im.thread.Join();  // the thread runs FinalDrain before it exits
    im.started = false;
  } else {
    im.FinalDrain();
  }
  im.shutDown = true;
}

void IoScheduler::Destroy() noexcept {
  if (impl_ != nullptr) {
    Shutdown();
    Impl& im = *impl_;
    // Requests that slipped into the queues after the final drain.
    if (im.ready) im.DrainInputs(im.Now(), true);
    IAllocator& a = *alloc_;
    base::FreeArray(a, im.nodes, im.nodeCount, MemTag::Misc);
    base::FreeArray(a, im.inflight, im.cfg.maxInFlight, MemTag::Misc);
    base::FreeArray(a, im.chHead, channelCount_, MemTag::Misc);
    base::FreeArray(a, im.chTail, channelCount_, MemTag::Misc);
    base::FreeArray(a, im.files, im.fileCount, MemTag::Misc);
    base::FreeArray(a, im.held, im.heldCount, MemTag::Misc);
    im.freeNodes.Destroy();
    im.streamQueue.Destroy();
    im.fileQueue.Destroy();
    base::Delete(a, impl_, MemTag::Misc);
    impl_ = nullptr;
  }
  if (channels_ != nullptr) {
    for (std::uint32_t s = 0; s < channelCount_; ++s) channels_[s].Reclaim();
    base::FreeArray(*alloc_, channels_, channelCount_, MemTag::StreamPool);
    channels_ = nullptr;
  }
  channelCount_ = 0;
  alloc_ = nullptr;
}

namespace {
// Marks a Post/Submit call in progress for Shutdown (see Shutdown()).
class PostGuard {
 public:
  explicit PostGuard(std::atomic<std::uint32_t>& c) noexcept : c_(c) { c_.fetch_add(1u, std::memory_order_seq_cst); }
  ~PostGuard() { c_.fetch_sub(1u, std::memory_order_release); }
  PostGuard(const PostGuard&) = delete;
  PostGuard& operator=(const PostGuard&) = delete;

 private:
  std::atomic<std::uint32_t>& c_;
};
}  // namespace

bool IoScheduler::Post(const source::IoReadRequest& req) noexcept {
  Impl* im = impl_;
  if (im == nullptr) return false;
  PostGuard guard(im->inPost);
  // seq_cst load after the seq_cst increment of inPost (Dekker with Shutdown).
  if (im->accepting.load(std::memory_order_seq_cst) == 0u || req.streamSlot >= channelCount_ || req.bytes == 0u ||
      req.bytes > im->pool->ChunkBytes()) {
    im->streamRejected.Add();
    return false;
  }
  std::uint32_t chunk = kInvalidChunk;
  if (req.dst != nullptr) {
    // Decision d: the chunk must be borrowed, belong to this stream slot (or be
    // unattributed, kNoOwner) and not already be in the scheduler's hands.
    chunk = im->pool->IndexOf(req.dst);
    if (chunk == kInvalidChunk || !im->pool->IsBorrowed(chunk) || chunk >= im->heldCount) {
      im->streamRejected.Add();
      return false;
    }
    const std::uint32_t owner = im->pool->OwnerOf(chunk);
    if ((owner != req.streamSlot && owner != kNoOwner) ||
        im->held[chunk].exchange(1u, std::memory_order_acq_rel) != 0u) {
      im->streamRejected.Add();
      return false;
    }
  }
  StreamChannel& ch = channels_[req.streamSlot];
  if (ch.reserved_.value.fetch_add(1u, std::memory_order_acq_rel) >= ch.capacity_) {
    ch.Unreserve();
    im->Unhold(chunk);
    im->streamRejected.Add();
    return false;
  }
  QueuedRead q{req, ch.Epoch(), 0u};
  if (!im->streamQueue.TryPush(q)) {
    ch.Unreserve();
    im->Unhold(chunk);
    im->streamRejected.Add();
    return false;
  }
  im->streamPosted.Add();
  Wake();
  return true;
}

bool IoScheduler::PostIo(void* ctx, const source::IoReadRequest& req) noexcept {
  return ctx != nullptr && static_cast<IoScheduler*>(ctx)->Post(req);
}

void IoScheduler::Wake() noexcept {
  Impl* im = impl_;
  if (im == nullptr) return;
  if (im->wakePending.value.exchange(1u, std::memory_order_acq_rel) == 0u) im->wake.Post();
}

void IoScheduler::WakeThunk(void* ctx) noexcept {
  if (ctx != nullptr) static_cast<IoScheduler*>(ctx)->Wake();
}

bool IoScheduler::Submit(FileRequest& request) noexcept {
  Impl* im = impl_;
  if (im == nullptr) return false;
  PostGuard guard(im->inPost);
  if (im->accepting.load(std::memory_order_seq_cst) == 0u) return false;
  if (request.path == nullptr || (request.dst != nullptr && request.bytes == 0u)) return false;
  std::uint32_t s = request.state.load(std::memory_order_acquire);
  if (s == static_cast<std::uint32_t>(FileState::Queued)) return false;
  if (im->fileOutstanding.fetch_add(1u, std::memory_order_acq_rel) >= im->cfg.fileQueueCapacity) {
    im->fileOutstanding.fetch_sub(1u, std::memory_order_acq_rel);
    return false;
  }
  request.cancel.store(0u, std::memory_order_relaxed);
  request.bytesRead = 0;
  request.fileSize = UINT64_MAX;
  if (!request.state.compare_exchange_strong(s, static_cast<std::uint32_t>(FileState::Queued),
                                             std::memory_order_acq_rel)) {
    im->fileOutstanding.fetch_sub(1u, std::memory_order_acq_rel);
    return false;
  }
  if (!im->fileQueue.TryPush(&request)) {
    request.state.store(static_cast<std::uint32_t>(FileState::Idle), std::memory_order_release);
    im->fileOutstanding.fetch_sub(1u, std::memory_order_acq_rel);
    return false;
  }
  Wake();
  return true;
}

void IoScheduler::Cancel(FileRequest& request) noexcept {
  request.cancel.store(1u, std::memory_order_release);
  Wake();
}

bool IoScheduler::RunOnce() noexcept {
  if (impl_ == nullptr || !impl_->ready || impl_->started || impl_->shutDown) return false;
  return impl_->Step();
}

IoStats IoScheduler::Stats() const noexcept {
  IoStats s;
  if (impl_ == nullptr) return s;
  const Impl& im = *impl_;
  s.readsIssued = im.readsIssued.Get();
  s.bytesIssued = im.bytesIssued.Get();
  s.readsCompleted = im.readsCompleted.Get();
  s.bytesCompleted = im.bytesCompleted.Get();
  s.readErrors = im.readErrors.Get();
  s.openFailures = im.openFailures.Get();
  s.streamPosted = im.streamPosted.Get();
  s.streamRejected = im.streamRejected.Get();
  s.staleDropped = im.staleDropped.Get();
  s.staleCompleted = im.staleCompleted.Get();
  s.fsRejects = im.fsRejects.Get();
  s.budgetWaits = im.budgetWaits.Get();
  s.poolStarved = im.poolStarved.Get();
  s.agedServed = im.agedServed.Get();
  s.stallEvents = im.stallEvents.Get();
  s.filesCompleted = im.filesCompleted.Get();
  s.maxWindowBytes = im.maxWindowBytes.Get();
  s.inFlight = im.inflightStat.load(std::memory_order_relaxed);
  s.pending = im.pendingStat.load(std::memory_order_relaxed);
  return s;
}

}  // namespace snd::io
