#include "snd/io/default_file_system.hpp"

#include <atomic>
#include <cstring>
#include <new>

#include "snd/base/mpmc.hpp"
#include "snd/internal/rt_check.hpp"
#include "snd/platform/clock.hpp"
#include "snd/platform/semaphore.hpp"
#include "snd/platform/thread.hpp"

#include "native_file.hpp"

namespace snd::io {

namespace {

constexpr std::uint32_t kMaxRootBytes = 2048;
constexpr std::uint32_t kMaxRelBytes = 2048;
constexpr std::uint32_t kIndexBits = 12;
constexpr std::uint32_t kIndexMask = (1u << kIndexBits) - 1u;
constexpr std::uint32_t kGenMask = 0xFFFFFFFFu >> kIndexBits;

enum : std::uint32_t { kSlotQueued = 1, kSlotDone = 2, kSlotError = 3 };

struct FileEntry {
  detail::NativeFile file = detail::kNoNativeFile;
  std::uint32_t gen = 0;
  std::uint32_t inflight = 0;  // IO thread
  std::uint64_t size = 0;
  bool used = false;
  bool closePending = false;
};

struct ReadSlot {
  // Written by the IO thread before the slot is queued (published by the queue).
  std::uint64_t token = 0;
  std::uint64_t offset = 0;
  void* dst = nullptr;
  std::uint32_t bytes = 0;
  std::uint32_t fileIndex = 0;
  detail::NativeFile file = detail::kNoNativeFile;
  bool active = false;  // IO thread
  // Written by the reader thread.
  std::uint32_t result = 0;
  std::atomic<std::uint32_t> state{0};
  std::atomic<std::uint32_t> notifying{0};  // reader is inside the completion notify
};

}  // namespace

struct DefaultFileSystem::Impl {
  char root[kMaxRootBytes] = {};
  std::uint32_t rootLen = 0;
  detail::NativeRoot native;  // the OS side (native_file.hpp)
  DefaultFileSystemConfig cfg{};
  FileEntry* files = nullptr;
  ReadSlot* slots = nullptr;
  base::MpmcRing<std::uint32_t> queue;
  platform::Semaphore work{0};
  platform::Thread* threads = nullptr;
  std::uint32_t threadCount = 0;
  std::atomic<std::uint32_t> stop{0};
  std::atomic<CompletionNotifyFn> notify{nullptr};
  std::atomic<void*> notifyUser{nullptr};
  std::atomic<std::uint32_t> openCount{0};
  std::atomic<std::uint32_t> inflight{0};

  FileEntry* Lookup(FileId id) noexcept {
    const std::uint32_t index = (id & kIndexMask);
    if (index == 0u || index > cfg.maxOpenFiles) return nullptr;
    FileEntry& e = files[index - 1u];
    if (!e.used || e.closePending || e.gen != (id >> kIndexBits)) return nullptr;
    return &e;
  }

  void CloseEntry(FileEntry& e) noexcept {
    detail::CloseFile(e.file);
    e.file = detail::kNoNativeFile;
    e.used = false;
    e.closePending = false;
    openCount.fetch_sub(1u, std::memory_order_relaxed);
  }

  void Perform(ReadSlot& s) noexcept {
    std::uint32_t got = 0;
    const std::uint32_t state = detail::ReadAt(s.file, s.offset, s.dst, s.bytes, &got) ? kSlotDone : kSlotError;
    s.result = got;
    // The slot is not retired by Poll while `notifying` is set, so once the IO
    // thread has retired every read no reader is still inside the notify (its
    // target, e.g. the scheduler, may then be destroyed).
    const CompletionNotifyFn fn = notify.load(std::memory_order_acquire);
    if (fn != nullptr) s.notifying.store(1u, std::memory_order_relaxed);
    s.state.store(state, std::memory_order_release);
    if (fn != nullptr) {
      fn(notifyUser.load(std::memory_order_acquire));
      s.notifying.store(0u, std::memory_order_release);
    }
  }

  static void ReaderMain(void* p) noexcept {
    Impl& im = *static_cast<Impl*>(p);
    for (;;) {
      im.work.Wait();
      std::uint32_t index = 0;
      if (!im.queue.TryPop(index)) {
        if (im.stop.load(std::memory_order_acquire) != 0u) return;
        continue;
      }
      im.Perform(im.slots[index]);
    }
  }
};

DefaultFileSystem::~DefaultFileSystem() { Shutdown(); }

bool DefaultFileSystem::ValidatePath(const char* path) noexcept {
  if (path == nullptr || path[0] == '\0' || path[0] == '/') return false;
  std::int64_t depth = 0;
  std::size_t i = 0;
  for (;;) {
    const std::size_t start = i;
    while (path[i] != '\0' && path[i] != '/') {
      if (path[i] == '\\' || path[i] == ':') return false;  // D-103: no drive letters / streams
      ++i;
    }
    const std::size_t len = i - start;
    if (len == 2u && path[start] == '.' && path[start + 1u] == '.') {
      if (--depth < 0) return false;
    } else if (len == 1u && path[start] == '.') {
      // current directory
    } else if (len != 0u) {
      ++depth;
    }
    if (path[i] == '\0') break;
    ++i;
    if (i >= kMaxRelBytes) return false;
  }
  return i < kMaxRelBytes;
}

bool DefaultFileSystem::Init(IAllocator& allocator, const DefaultFileSystemConfig& config) noexcept {
  Shutdown();
  if (config.maxInFlight == 0u || config.maxOpenFiles == 0u || config.maxOpenFiles > kIndexMask - 1u ||
      config.readerThreads == 0u) {
    return false;
  }
  const char* root = config.root != nullptr && config.root[0] != '\0' ? config.root : ".";
  const std::size_t rootLen = std::strlen(root);
  if (rootLen + 1u >= kMaxRootBytes) return false;

  alloc_ = &allocator;
  impl_ = base::New<Impl>(allocator, MemTag::Misc);
  if (impl_ == nullptr) {
    alloc_ = nullptr;
    return false;
  }
  Impl& im = *impl_;
  im.cfg = config;
  std::memcpy(im.root, root, rootLen + 1u);
  im.rootLen = static_cast<std::uint32_t>(rootLen);
  while (im.rootLen > 1u && im.root[im.rootLen - 1u] == '/') im.root[--im.rootLen] = '\0';
  if (!im.native.Open(allocator, im.root)) {  // false: allocation (a missing root is not an error here)
    Shutdown();
    return false;
  }
  im.files = base::AllocateArray<FileEntry>(allocator, config.maxOpenFiles, MemTag::Misc);
  im.slots = base::AllocateArray<ReadSlot>(allocator, config.maxInFlight, MemTag::Misc);
  im.threads = base::AllocateArray<platform::Thread>(allocator, config.readerThreads, MemTag::Misc);
  bool ok = im.files != nullptr && im.slots != nullptr && im.threads != nullptr &&
            im.queue.Init(allocator, config.maxInFlight, MemTag::Misc);
  for (std::uint32_t i = 0; ok && i < config.readerThreads; ++i) {
    platform::ThreadDesc desc;
    desc.name = config.threadName != nullptr ? config.threadName : "snd-fsread";
    ok = im.threads[i].Start(desc, &Impl::ReaderMain, impl_);
    if (ok) im.threadCount = i + 1u;
  }
  if (!ok) {
    Shutdown();
    return false;
  }
  return true;
}

void DefaultFileSystem::Shutdown() noexcept {
  if (impl_ == nullptr) return;
  Impl& im = *impl_;
  im.stop.store(1u, std::memory_order_release);
  if (im.threadCount != 0u) im.work.Post(im.threadCount);
  for (std::uint32_t i = 0; i < im.threadCount; ++i) im.threads[i].Join();
  im.threadCount = 0;
  if (im.files != nullptr) {
    for (std::uint32_t i = 0; i < im.cfg.maxOpenFiles; ++i) {
      if (im.files[i].used) im.CloseEntry(im.files[i]);
    }
  }
  im.native.Close();
  IAllocator& a = *alloc_;
  base::FreeArray(a, im.files, im.cfg.maxOpenFiles, MemTag::Misc);
  base::FreeArray(a, im.slots, im.cfg.maxInFlight, MemTag::Misc);
  base::FreeArray(a, im.threads, im.cfg.readerThreads, MemTag::Misc);
  im.queue.Destroy();
  base::Delete(a, impl_, MemTag::Misc);
  impl_ = nullptr;
  alloc_ = nullptr;
}

void DefaultFileSystem::SetCompletionNotify(CompletionNotifyFn fn, void* user) noexcept {
  if (impl_ == nullptr) return;
  impl_->notifyUser.store(user, std::memory_order_release);
  impl_->notify.store(fn, std::memory_order_release);
}

FileId DefaultFileSystem::Open(const char* path) {
  SND_RT_NOTE(FileIo);
  if (impl_ == nullptr || !ValidatePath(path)) return kInvalidFile;
  Impl& im = *impl_;
  std::uint32_t index = 0;
  while (index < im.cfg.maxOpenFiles && im.files[index].used) ++index;
  if (index == im.cfg.maxOpenFiles) return kInvalidFile;
  std::uint64_t size = 0;
  const detail::NativeFile file = im.native.OpenBeneath(path, &size);
  if (file == detail::kNoNativeFile) return kInvalidFile;
  FileEntry& e = im.files[index];
  e.gen = (e.gen + 1u) & kGenMask;
  if (e.gen == 0u) e.gen = 1u;
  e.file = file;
  e.inflight = 0;
  e.size = size;
  e.used = true;
  e.closePending = false;
  im.openCount.fetch_add(1u, std::memory_order_relaxed);
  return (e.gen << kIndexBits) | (index + 1u);
}

std::uint64_t DefaultFileSystem::Size(FileId file) {
  SND_RT_NOTE(FileIo);
  if (impl_ == nullptr) return UINT64_MAX;
  FileEntry* e = impl_->Lookup(file);
  return e == nullptr ? UINT64_MAX : e->size;
}

bool DefaultFileSystem::ReadAsync(FileId file, std::uint64_t offset, void* dst, std::uint32_t bytes,
                                  IoToken token) {
  SND_RT_NOTE(FileIo);
  if (impl_ == nullptr || dst == nullptr) return false;
  Impl& im = *impl_;
  FileEntry* e = im.Lookup(file);
  if (e == nullptr) return false;
  std::uint32_t freeSlot = im.cfg.maxInFlight;
  for (std::uint32_t i = 0; i < im.cfg.maxInFlight; ++i) {
    const ReadSlot& s = im.slots[i];
    if (s.active) {
      if (s.token == token.value) return false;  // token already in flight
    } else if (freeSlot == im.cfg.maxInFlight) {
      freeSlot = i;
    }
  }
  if (freeSlot == im.cfg.maxInFlight) return false;
  ReadSlot& s = im.slots[freeSlot];
  s.token = token.value;
  s.offset = offset;
  s.dst = dst;
  s.bytes = bytes;
  s.fileIndex = (file & kIndexMask) - 1u;
  s.file = e->file;
  s.result = 0;
  s.active = true;
  s.state.store(kSlotQueued, std::memory_order_relaxed);
  if (!im.queue.TryPush(freeSlot)) {  // cannot happen: queue capacity >= maxInFlight
    s.active = false;
    return false;
  }
  ++e->inflight;
  im.inflight.fetch_add(1u, std::memory_order_relaxed);
  im.work.Post();
  return true;
}

IoStatus DefaultFileSystem::Poll(IoToken token) {
  SND_RT_NOTE(FileIo);
  if (impl_ == nullptr) return IoStatus{SND_IO_UNKNOWN_TOKEN, 0};
  Impl& im = *impl_;
  for (std::uint32_t i = 0; i < im.cfg.maxInFlight; ++i) {
    ReadSlot& s = im.slots[i];
    if (!s.active || s.token != token.value) continue;
    const std::uint32_t state = s.state.load(std::memory_order_acquire);
    if (state == kSlotQueued) return IoStatus{SND_IO_PENDING, 0};
    // The notify is a few atomics (and at most one wake syscall): wait it out.
    while (s.notifying.load(std::memory_order_acquire) != 0u) platform::YieldThread();
    const IoStatus result = state == kSlotDone ? IoStatus{SND_IO_DONE, s.result} : IoStatus{SND_IO_ERROR, 0};
    s.active = false;
    im.inflight.fetch_sub(1u, std::memory_order_relaxed);
    FileEntry& e = im.files[s.fileIndex];
    if (e.inflight > 0u) --e.inflight;
    if (e.inflight == 0u && e.closePending) im.CloseEntry(e);
    return result;
  }
  return IoStatus{SND_IO_UNKNOWN_TOKEN, 0};
}

void DefaultFileSystem::Close(FileId file) {
  SND_RT_NOTE(FileIo);
  if (impl_ == nullptr) return;
  FileEntry* e = impl_->Lookup(file);
  if (e == nullptr) return;
  if (e->inflight != 0u) {
    e->closePending = true;  // contract violation by the caller: close after the last read retires
    return;
  }
  impl_->CloseEntry(*e);
}

bool DefaultFileSystem::RootAvailable() const noexcept { return impl_ != nullptr && impl_->native.Available(); }

std::uint32_t DefaultFileSystem::OpenFileCount() const noexcept {
  return impl_ == nullptr ? 0u : impl_->openCount.load(std::memory_order_relaxed);
}

std::uint32_t DefaultFileSystem::InFlight() const noexcept {
  return impl_ == nullptr ? 0u : impl_->inflight.load(std::memory_order_relaxed);
}

}  // namespace snd::io
