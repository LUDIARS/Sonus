#include "snd/bank/bank_manager.hpp"

#include <atomic>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <new>
#include <string_view>
#include <thread>  // yield while Destroy waits for cancelled reads (bank cannot see platform, 12.1)

#include "bank_io.hpp"
#include "snd/base/mutex.hpp"
#include "snd/io/io_scheduler.hpp"

namespace snd::bank {
namespace {

constexpr std::uint32_t kMaxLanguages = SND_MAX_LANGUAGES;  // contract 1.1

enum class Stage : std::uint8_t {
  Free = 0,
  Open,        // waiting for PumpIo to open the file
  Reading,     // reads in flight (PumpIo)
  Validating,  // job submitted
  Validated,   // job finished, Pump decides (dependencies, DSP pool)
  Ready,       // registration queued, not popped yet
  Registered,  // registration popped (LOADING until the audio side applies it, then LOADED)
  Unloading,   // BeginUnload accepted, waiting for Release
  Failed,
};

}  // namespace

struct BankManager::Impl {
  struct Slot;
  struct JobCtx {
    Impl* impl;
    std::uint32_t index;
  };
  struct Slot {
    Stage stage = Stage::Free;
    std::uint32_t generation = 1;
    char* path = nullptr;
    std::uint32_t baseLen = 0;  // language banks: length of "<base>" in "<base>.<lang>.sndb", 0 unknown
    Id langId = 0;              // language of the bank: from the path at request, confirmed by the header
    std::uint8_t* image = nullptr;
    std::uint64_t imageBytes = 0;
    detail::FileRead read;           // DirectFileSystem mode
    detail::SchedulerRead sched;     // Scheduler mode
    bool cancel = false;
    std::uint64_t requestSeq = 0;    // RequestLoad acceptance order (duplicates, D-50)
    // validation job (job writes prepared/jobResult/jobError; manager reads after jobCounter == 0)
    JobCtx ctx{};
    std::uint32_t jobCounter = 0;
    std::uint32_t cancelFlag = 0;
    PreparedBank* prepared = nullptr;
    snd_result jobResult = SND_OK;
    ParseError jobError;
    // registration bookkeeping
    std::uint32_t dependents = 0;
    bool dspReserved = false;
    snd_result result = SND_OK;
    char reason[192] = {};
  };
  struct Language {
    Id id;
    char* name;
  };

  BankManagerDesc desc;
  IAllocator* alloc = nullptr;
  mutable base::Mutex mutex;
  Slot* slots = nullptr;
  notify::BankSlotState* states = nullptr;
  bool ownStates = false;
  std::uint32_t* queue = nullptr;  // ready registrations (slot indices), FIFO ring
  std::uint32_t queueHead = 0;
  std::uint32_t queueCount = 0;
  Language languages[kMaxLanguages] = {};
  std::uint32_t languageCount = 0;
  Id currentLanguage = 0;
  std::uint64_t nextRequestSeq = 0;

  // --- helpers (mutex held) -------------------------------------------------------------
  Slot* Get(HandleRef h) noexcept {
    if (h.IsNone() || h.slot >= desc.maxBanks) return nullptr;
    Slot& s = slots[h.slot];
    return (s.stage != Stage::Free && s.generation == h.generation) ? &s : nullptr;
  }
  const Slot* Get(HandleRef h) const noexcept { return const_cast<Impl*>(this)->Get(h); }
  HandleRef HandleOf(const Slot& s) const noexcept {
    return HandleRef{static_cast<std::uint32_t>(&s - slots), s.generation};
  }
  void SetState(const Slot& s, snd_bank_state st, snd_result r) noexcept {
    notify::BankSlotState& ns = states[&s - slots];
    ns.result.store(r, std::memory_order_relaxed);
    ns.state.store(st, std::memory_order_release);
  }
  snd_bank_state AtomicState(const Slot& s) const noexcept {
    return states[&s - slots].state.load(std::memory_order_acquire);
  }
  bool JobPending(const Slot& s) const noexcept {
    return std::atomic_ref<std::uint32_t>(const_cast<std::uint32_t&>(s.jobCounter))
               .load(std::memory_order_acquire) != 0u;
  }
  char* Dup(const char* text, std::size_t n) noexcept {
    char* p = static_cast<char*>(alloc->Allocate(n + 1, 16, MemTag::Misc));
    if (p != nullptr) {
      std::memcpy(p, text, n);
      p[n] = '\0';
    }
    return p;
  }
  void Fail(Slot& s, snd_result r, const char* why) noexcept {
    if (s.cancel) {  // a cancelled load ends UNLOADED whatever happened to it (never FAILED)
      FreeSlot(s);
      return;
    }
    FreeLoadMemory(s);
    s.stage = Stage::Failed;
    s.result = r;
    std::size_t n = 0;  // "<path>: <why>", truncated to the buffer
    for (const char* part : {s.path != nullptr ? static_cast<const char*>(s.path) : "?", ": ", why}) {
      for (const char* c = part; *c != '\0' && n + 1 < sizeof(s.reason); ++c) s.reason[n++] = *c;
    }
    s.reason[n] = '\0';
    SetState(s, SND_BANK_STATE_FAILED, r);
  }
  bool SchedulerMode() const noexcept { return desc.ioMode == BankIoMode::Scheduler; }
  // A read (or size query) still in the IO path: its target and path must stay.
  bool IoInFlight(const Slot& s) const noexcept {
    return SchedulerMode() ? s.sched.InFlight() : s.read.inflight;
  }
  // Frees everything a load holds except the path (no job may be running).
  void FreeLoadMemory(Slot& s) noexcept {
    if (IoInFlight(s)) return;  // never free a read target in flight (caller waits)
    if (!SchedulerMode() && s.read.file != kInvalidFile) {
      detail::CloseBankFile(*desc.fileSystem, s.read.file);
      s.read.file = kInvalidFile;
    }
    s.sched.phase = detail::SchedulerRead::Phase::Idle;
    s.sched.cancelAsked = false;
    if (s.dspReserved && desc.dsp.release != nullptr && s.prepared != nullptr) {
      desc.dsp.release(desc.dsp.user, s.prepared->dspNeed);
    }
    s.dspReserved = false;
    if (s.prepared != nullptr) {
      for (std::uint32_t i = 0; i < s.prepared->dependencyCount; ++i) {
        Slot* d = Get(s.prepared->dependencies[i]);
        if (d != nullptr && d->dependents > 0) --d->dependents;
      }
      DestroyPrepared(s.prepared, *alloc);
      s.prepared = nullptr;
    }
    if (s.image != nullptr) {
      alloc->Free(s.image, MemTag::Bank);
      s.image = nullptr;
    }
    s.imageBytes = 0;
    s.read = detail::FileRead{};
  }
  void FreeSlot(Slot& s) noexcept {
    FreeLoadMemory(s);
    if (s.path != nullptr) alloc->Free(s.path, MemTag::Misc);
    s.path = nullptr;
    s.baseLen = 0;
    s.langId = 0;
    s.cancel = false;
    s.result = SND_OK;
    s.reason[0] = '\0';
    s.stage = Stage::Free;
    // D-46: issued generations stay below 2^31 (2^31 - 1 wraps to 1).
    s.generation = s.generation + 1u >= 0x80000000u ? 1u : s.generation + 1u;
    notify::BankSlotState& ns = states[&s - slots];
    ns.generation.store(s.generation, std::memory_order_relaxed);
    SetState(s, SND_BANK_STATE_UNLOADED, SND_OK);
  }
  void StartLoad(Slot& s) noexcept {
    s.stage = Stage::Open;
    s.requestSeq = ++nextRequestSeq;
    s.cancel = false;
    s.result = SND_OK;
    s.reason[0] = '\0';
    s.dependents = 0;
    // A retry reports through on_bank_loaded again (CONTRACTS decision c).
    states[&s - slots].notified.store(0, std::memory_order_relaxed);
    SetState(s, SND_BANK_STATE_LOADING, SND_OK);
    GuessLanguage(s);
  }
  Slot* FindLoaded(Id bankId, std::uint32_t languageId) noexcept {
    for (std::uint32_t i = 0; i < desc.maxBanks; ++i) {
      Slot& s = slots[i];
      if ((s.stage == Stage::Ready || s.stage == Stage::Registered) && s.prepared != nullptr &&
          s.prepared->bankId == bankId && (languageId == 0 || s.prepared->languageId == languageId)) {
        return &s;
      }
    }
    return nullptr;
  }
  const char* LanguageName(Id id) const noexcept {
    for (std::uint32_t i = 0; i < languageCount; ++i) {
      if (languages[i].id == id) return languages[i].name;
    }
    return nullptr;
  }
  snd_result AddLanguage(const char* name, std::size_t n) noexcept {
    const Id id = HashId(std::string_view(name, n));
    if (const char* known = LanguageName(id); known != nullptr) {
      // Same name: nothing to do. Another name with the same id: hash collision (D-50).
      return std::strlen(known) == n && std::memcmp(known, name, n) == 0 ? SND_OK : SND_ERR_INVALID_ARGUMENT;
    }
    if (languageCount == kMaxLanguages) return SND_ERR_LIMIT_REACHED;
    char* copy = Dup(name, n);
    if (copy == nullptr) return SND_ERR_OUT_OF_BUDGET;
    languages[languageCount++] = Language{id, copy};
    return SND_OK;
  }
  // Splits "<base>.<lang>.sndb"; false if the path does not have that shape.
  static bool SplitLanguagePath(const char* path, std::size_t* baseLen, std::string_view* lang) noexcept {
    if (path == nullptr) return false;
    const std::size_t n = std::strlen(path);
    static constexpr char kExt[] = ".sndb";
    const std::size_t ext = sizeof(kExt) - 1;
    if (n <= ext || std::strcmp(path + n - ext, kExt) != 0) return false;
    const std::size_t end = n - ext;
    std::size_t dot = end;
    while (dot > 0 && path[dot - 1] != '.' && path[dot - 1] != '/') --dot;
    if (dot == 0 || path[dot - 1] != '.' || dot == end) return false;
    *baseLen = dot - 1;
    *lang = std::string_view(path + dot, end - dot);
    return true;
  }
  // At request time: a path "<base>.<lang>.sndb" with a known language name is
  // taken as that language bank until the header says otherwise (so a bank that
  // is still loading takes part in a language switch, 10.4).
  void GuessLanguage(Slot& s) noexcept {
    s.baseLen = 0;
    s.langId = 0;
    std::size_t base = 0;
    std::string_view lang;
    if (!SplitLanguagePath(s.path, &base, &lang)) return;
    const Id id = HashId(lang);
    if (LanguageName(id) == nullptr) return;
    s.baseLen = static_cast<std::uint32_t>(base);
    s.langId = id;
  }
  // After validation the header is authoritative.
  void LearnLanguagePath(Slot& s) noexcept {
    s.baseLen = 0;
    s.langId = s.prepared != nullptr ? s.prepared->languageId : 0;
    if (s.langId == 0) return;
    std::size_t base = 0;
    std::string_view lang;
    if (!SplitLanguagePath(s.path, &base, &lang) || HashId(lang) != s.langId) return;
    s.baseLen = static_cast<std::uint32_t>(base);
    (void)AddLanguage(lang.data(), lang.size());
  }
  // Registration queue (FIFO ring of slot indices).
  void PushQueue(std::uint32_t idx) noexcept {
    queue[(queueHead + queueCount) % desc.maxBanks] = idx;
    ++queueCount;
    CheckQueue();
  }
  void RemoveFromQueue(std::uint32_t idx) noexcept {
    std::uint32_t kept = 0;
    for (std::uint32_t i = 0; i < queueCount; ++i) {
      const std::uint32_t v = queue[(queueHead + i) % desc.maxBanks];
      if (v != idx) queue[(queueHead + kept++) % desc.maxBanks] = v;
    }
    queueCount = kept;
    CheckQueue();
  }
  // Invariants: count <= capacity, each slot at most once and READY (debug builds).
  void CheckQueue() const noexcept {
#ifndef NDEBUG
    if (queueCount > desc.maxBanks) std::abort();
    for (std::uint32_t i = 0; i < queueCount; ++i) {
      const std::uint32_t a = queue[(queueHead + i) % desc.maxBanks];
      if (a >= desc.maxBanks || slots[a].stage != Stage::Ready) std::abort();
      for (std::uint32_t j = i + 1; j < queueCount; ++j) {
        if (queue[(queueHead + j) % desc.maxBanks] == a) std::abort();
      }
    }
#endif
  }

  static void ValidateJob(void* data) {
    auto* c = static_cast<JobCtx*>(data);
    Impl* impl = c->impl;
    Slot& s = impl->slots[c->index];
    s.jobError = ParseError{};
    s.jobResult = PrepareBank(s.image, s.imageBytes, impl->desc.prepare, *impl->alloc, &s.prepared, &s.jobError);
    if (s.jobResult == SND_OK && impl->desc.dsp.compute != nullptr) {
      s.jobResult = impl->RuntimeDspNeed(*s.prepared, &s.jobError);
      if (s.jobResult != SND_OK) {
        DestroyPrepared(s.prepared, *impl->alloc);
        s.prepared = nullptr;
      }
    }
  }

  // D-54: the runtime's DSP pool need, from the bank's effect rows and the
  // runtime Config (normal worker, in the validation job).
  snd_result RuntimeDspNeed(PreparedBank& pb, ParseError* err) noexcept {
    const BankView& v = pb.view;
    const std::uint32_t fxRows = v.Rows(bf::chunk::EFFX);
    if (fxRows == 0) {
      pb.dspNeed = bf::DspPoolNeed{0u, 0u, 0u};
      return SND_OK;
    }
    auto* descs = static_cast<snd_effect_desc*>(alloc->Allocate(sizeof(snd_effect_desc) * fxRows, 16, MemTag::Misc));
    if (descs == nullptr) {
      std::snprintf(err->message, sizeof(err->message), "cannot allocate %u effect descriptions", fxRows);
      return SND_ERR_OUT_OF_BUDGET;
    }
    const auto busParent = v.Column<col::BUSS_parent>();
    const auto busChannels = v.Column<col::BUSS_channels>();
    const auto busFirst = v.Column<col::BUSS_effectFirst>();
    const auto busCount = v.Column<col::BUSS_effectCount>();
    const auto type = v.Column<col::EFFX_effectType>();
    const auto paramFirst = v.Column<col::EFFX_paramFirst>();
    const auto paramCount = v.Column<col::EFFX_paramCount>();
    const auto blobOffset = v.Column<col::EFFX_blobOffset>();
    const auto blobBytes = v.Column<col::EFFX_blobBytes>();
    const auto values = v.Column<col::EFPV_value>();
    const ByteSpan efbl = v.Chunk(bf::chunk::EFBL);
    for (std::uint32_t r = 0; r < fxRows; ++r) {
      snd_effect_desc& d = descs[r];
      d = snd_effect_desc{};
      d.effect_type = type[r];
      d.channels = desc.outputChannels;  // an effect of no bus
      d.max_frames = desc.blockSize;
      d.param_count = paramCount[r];
      d.initial_params = d.param_count != 0 && values.data() != nullptr ? values.data() + paramFirst[r] : nullptr;
      if (d.initial_params == nullptr) d.param_count = 0;
      d.blob_bytes = blobBytes[r];
      d.blob = d.blob_bytes != 0 ? efbl.data + blobOffset[r] : nullptr;
      d.sample_rate = desc.sampleRate;
    }
    // Bus channels: the bus's, else the nearest parent's, else the output's (validated: parents after the bus).
    for (std::uint32_t b = 0; b < v.Rows(bf::chunk::BUSS); ++b) {
      std::uint32_t ch = 0;
      std::uint32_t at = b;
      for (std::uint32_t depth = 0; at != bf::kNone && at < busChannels.rows() && depth < 4096u; ++depth) {
        ch = busChannels[at];
        if (ch != 0u) break;
        at = busParent[at];
      }
      if (ch == 0u) ch = desc.outputChannels;
      for (std::uint32_t k = 0; k < busCount[b]; ++k) {
        const std::uint32_t fx = busFirst[b] + k;
        if (fx < fxRows) descs[fx].channels = ch;
      }
    }
    bf::DspPoolNeed need{0u, 0u, 0u};
    const bool ok = desc.dsp.compute(desc.dsp.user, descs, fxRows, desc.sampleRate, &need);
    alloc->Free(descs, MemTag::Misc);
    if (!ok) {
      std::snprintf(err->message, sizeof(err->message), "the DSP sizing hook refused the bank's %u effects", fxRows);
      return SND_ERR_INVALID_ARGUMENT;
    }
    pb.dspNeed = need;  // reserved; the bank's DSPN stays in builtDspNeed
    return SND_OK;
  }

  void PumpIoLocked() noexcept;
  void PumpSchedulerLocked() noexcept;
  snd_result CheckUnloadLocked(HandleRef bank, std::uint32_t mode, bool referencedByPlaybacks) const noexcept;
  // Checks the size and allocates the image (fails the slot otherwise).
  bool AllocateImage(Slot& s, std::uint64_t size) noexcept;
  // Reading done: validation and preparation on a normal worker (10.3).
  void StartValidation(Slot& s, std::uint32_t index) noexcept;
  bool PumpLocked() noexcept;
  bool Decide(Slot& s) noexcept;
};

using Impl = BankManager::Impl;

// ---------------------------------------------------------------------------
// Creation
// ---------------------------------------------------------------------------
snd_result BankManager::Create(const BankManagerDesc& desc, BankManager** out) noexcept {
  if (out == nullptr) return SND_ERR_INVALID_ARGUMENT;
  *out = nullptr;
  const bool ioOk = desc.ioMode == BankIoMode::Scheduler ? desc.scheduler != nullptr
                    : desc.ioMode == BankIoMode::DirectFileSystem ? desc.fileSystem != nullptr && desc.ioChunkBytes != 0
                                                                  : false;
  if (desc.allocator == nullptr || !ioOk || desc.jobs == nullptr || desc.maxBanks == 0 ||
      desc.maxBanks > 0x7FFFFFFFu) {
    return SND_ERR_INVALID_ARGUMENT;
  }
  IAllocator& a = *desc.allocator;
  void* mm = a.Allocate(sizeof(BankManager), 16, MemTag::Misc);
  void* im = a.Allocate(sizeof(Impl), 64, MemTag::Misc);
  void* sm = a.Allocate(sizeof(Impl::Slot) * desc.maxBanks, 64, MemTag::Misc);
  void* qm = a.Allocate(sizeof(std::uint32_t) * desc.maxBanks, 16, MemTag::Misc);
  void* nm = desc.slotStates == nullptr
                 ? a.Allocate(sizeof(notify::BankSlotState) * desc.maxBanks, 64, MemTag::Notify)
                 : nullptr;
  if (mm == nullptr || im == nullptr || sm == nullptr || qm == nullptr ||
      (desc.slotStates == nullptr && nm == nullptr)) {
    a.Free(mm, MemTag::Misc);
    a.Free(im, MemTag::Misc);
    a.Free(sm, MemTag::Misc);
    a.Free(qm, MemTag::Misc);
    a.Free(nm, MemTag::Notify);
    return SND_ERR_OUT_OF_BUDGET;
  }
  auto* m = new (mm) BankManager();
  auto* impl = new (im) Impl();
  impl->desc = desc;
  impl->alloc = desc.allocator;
  impl->slots = static_cast<Impl::Slot*>(sm);
  for (std::uint32_t i = 0; i < desc.maxBanks; ++i) new (&impl->slots[i]) Impl::Slot();
  impl->queue = static_cast<std::uint32_t*>(qm);
  if (desc.slotStates != nullptr) {
    impl->states = desc.slotStates;
  } else {
    impl->ownStates = true;
    impl->states = static_cast<notify::BankSlotState*>(nm);
    for (std::uint32_t i = 0; i < desc.maxBanks; ++i) new (&impl->states[i]) notify::BankSlotState();
  }
  for (std::uint32_t i = 0; i < desc.maxBanks; ++i) {
    impl->states[i].state.store(SND_BANK_STATE_UNLOADED, std::memory_order_relaxed);
    impl->states[i].result.store(SND_OK, std::memory_order_relaxed);
    impl->states[i].generation.store(1, std::memory_order_relaxed);
    impl->states[i].notified.store(0, std::memory_order_relaxed);
  }
  m->impl_ = impl;
  *out = m;
  return SND_OK;
}

void BankManager::Destroy(BankManager* manager) noexcept {
  if (manager == nullptr) return;
  Impl* impl = manager->impl_;
  IAllocator& a = *impl->alloc;
  bool anyJob = false;
  if (impl->SchedulerMode()) {
    // Requests still in the scheduler target slot memory: cancel them, and wait
    // for them when the IO thread is there to finish them (header comment).
    {
      base::LockGuard lock(impl->mutex);
      for (std::uint32_t i = 0; i < impl->desc.maxBanks; ++i) {
        detail::CancelRead(*impl->desc.scheduler, impl->slots[i].sched);
      }
    }
    if (impl->desc.scheduler->Running()) {
      for (std::uint32_t i = 0; i < impl->desc.maxBanks; ++i) {
        while (impl->slots[i].sched.InFlight()) std::this_thread::yield();
      }
    }
  }
  {
    base::LockGuard lock(impl->mutex);
    for (std::uint32_t i = 0; i < impl->desc.maxBanks; ++i) {
      Impl::Slot& s = impl->slots[i];
      if (s.stage == Stage::Validating && impl->JobPending(s)) {
        anyJob = true;  // the job still writes this slot: leak it
        continue;
      }
      if (impl->IoInFlight(s)) {
        anyJob = true;  // a read still targets the image / path: leak it
        continue;
      }
      if (s.stage != Stage::Free) impl->FreeSlot(s);
    }
    for (std::uint32_t i = 0; i < impl->languageCount; ++i) a.Free(impl->languages[i].name, MemTag::Misc);
    impl->languageCount = 0;
  }
  if (anyJob) return;  // documented leak: slots must outlive the running job / read
  if (impl->ownStates) {
    for (std::uint32_t i = 0; i < impl->desc.maxBanks; ++i) impl->states[i].~BankSlotState();
    a.Free(impl->states, MemTag::Notify);
  }
  for (std::uint32_t i = 0; i < impl->desc.maxBanks; ++i) impl->slots[i].~Slot();
  a.Free(impl->slots, MemTag::Misc);
  a.Free(impl->queue, MemTag::Misc);
  impl->~Impl();
  a.Free(impl, MemTag::Misc);
  manager->~BankManager();
  a.Free(manager, MemTag::Misc);
}

// ---------------------------------------------------------------------------
// Loading
// ---------------------------------------------------------------------------
HandleRef BankManager::RequestLoad(const char* path, snd_result* result) noexcept {
  snd_result dummy;
  snd_result& r = result != nullptr ? *result : dummy;
  if (path == nullptr || path[0] == '\0') {
    r = SND_ERR_INVALID_ARGUMENT;
    return HandleRef{0, 0};
  }
  base::LockGuard lock(impl_->mutex);
  Impl::Slot* freeSlot = nullptr;
  for (std::uint32_t i = 0; i < impl_->desc.maxBanks; ++i) {
    Impl::Slot& s = impl_->slots[i];
    if (s.stage == Stage::Free) {
      if (freeSlot == nullptr) freeSlot = &s;
      continue;
    }
    if (s.stage == Stage::Unloading || s.cancel || std::strcmp(s.path, path) != 0) continue;
    if (s.stage == Stage::Failed) impl_->StartLoad(s);  // retry in place, same handle
    r = SND_OK;
    return impl_->HandleOf(s);
  }
  if (freeSlot == nullptr) {
    r = SND_ERR_LIMIT_REACHED;
    return HandleRef{0, 0};
  }
  freeSlot->path = impl_->Dup(path, std::strlen(path));
  if (freeSlot->path == nullptr) {
    r = SND_ERR_OUT_OF_BUDGET;
    return HandleRef{0, 0};
  }
  impl_->StartLoad(*freeSlot);
  impl_->states[freeSlot - impl_->slots].generation.store(freeSlot->generation, std::memory_order_relaxed);
  r = SND_OK;
  return impl_->HandleOf(*freeSlot);
}

void BankManager::PumpIo() noexcept {
  base::LockGuard lock(impl_->mutex);
  impl_->PumpIoLocked();
}

bool Impl::AllocateImage(Slot& s, std::uint64_t size) noexcept {
  if (size > static_cast<std::uint64_t>(SIZE_MAX) - 64 || size == 0) {
    Fail(s, size == 0 ? SND_ERR_CORRUPT : SND_ERR_OUT_OF_BUDGET,
         size == 0 ? "the bank file is empty" : "the bank file is too large");
    return false;
  }
  s.image = static_cast<std::uint8_t*>(alloc->Allocate(static_cast<std::size_t>(size), 64, MemTag::Bank));
  if (s.image == nullptr) {
    Fail(s, SND_ERR_OUT_OF_BUDGET, "cannot allocate memory for the bank image");
    return false;
  }
  s.imageBytes = size;
  return true;
}

void Impl::StartValidation(Slot& s, std::uint32_t i) noexcept {
  s.stage = Stage::Validating;
  s.ctx = JobCtx{this, i};
  s.jobCounter = 1;
  s.cancelFlag = 0;
  s.prepared = nullptr;
  s.jobResult = SND_ERR_NOT_READY;
  JobDesc job{};
  job.fn = &Impl::ValidateJob;
  job.data = &s.ctx;
  job.done_counter = &s.jobCounter;
  job.cancel_flag = &s.cancelFlag;
  desc.jobs->Submit(&job, 1, JobLane::Background);
}

// Scheduler mode (stage 1.5): size query, then one whole-file read, both
// FileRequests of IoClass::Bank; only their final states are looked at here.
void Impl::PumpSchedulerLocked() noexcept {
  io::IoScheduler& io = *desc.scheduler;
  using Phase = detail::SchedulerRead::Phase;
  for (std::uint32_t i = 0; i < desc.maxBanks; ++i) {
    Slot& s = slots[i];
    detail::SchedulerRead& r = s.sched;
    if (s.stage != Stage::Open && s.stage != Stage::Reading) continue;
    if (s.cancel) {  // freed by Pump once nothing is in flight
      detail::CancelRead(io, r);
      if (r.Finished()) r.phase = Phase::Idle;
      continue;
    }
    if (r.phase == Phase::Idle) {
      const detail::SubmitResult sr = s.stage == Stage::Open
                                          ? detail::SubmitSizeQuery(io, r, s.path)
                                          : detail::SubmitWholeRead(io, r, s.path, s.image, s.imageBytes);
      if (sr == detail::SubmitResult::ShutDown) Fail(s, SND_ERR_IO, "the IO scheduler is shut down");
      continue;  // Retry: the file queue is full, try at the next PumpIo
    }
    if (!r.Finished()) continue;
    const io::FileResult res = r.req.Result();
    r.phase = Phase::Idle;
    if (res.state == io::FileState::Cancelled) {
      Fail(s, SND_ERR_IO, "the IO scheduler shut down before the bank was read");
      continue;
    }
    if (res.state != io::FileState::Done) {
      Fail(s, SND_ERR_IO, s.stage == Stage::Open ? "cannot open the bank file" : "reading the bank file failed");
      continue;
    }
    if (s.stage == Stage::Open) {
      if (!AllocateImage(s, res.fileSize)) continue;
      s.stage = Stage::Reading;
      const detail::SubmitResult sr = detail::SubmitWholeRead(io, r, s.path, s.image, s.imageBytes);
      if (sr == detail::SubmitResult::ShutDown) Fail(s, SND_ERR_IO, "the IO scheduler is shut down");
      continue;
    }
    if (res.bytesRead != s.imageBytes) {
      char why[128];
      std::snprintf(why, sizeof(why), "short read: %llu of %llu bytes (the file changed?)",
                    static_cast<unsigned long long>(res.bytesRead), static_cast<unsigned long long>(s.imageBytes));
      Fail(s, SND_ERR_IO, why);
      continue;
    }
    StartValidation(s, i);
  }
}

void Impl::PumpIoLocked() noexcept {
  if (SchedulerMode()) {
    PumpSchedulerLocked();
    return;
  }
  IFileSystem& fs = *desc.fileSystem;
  for (std::uint32_t i = 0; i < desc.maxBanks; ++i) {
    Slot& s = slots[i];
    if (s.stage == Stage::Open) {
      if (s.cancel) continue;  // freed by Pump
      FileId file = kInvalidFile;
      std::uint64_t size = 0;
      if (detail::OpenBankFile(fs, s.path, &file, &size) != SND_OK) {
        Fail(s, SND_ERR_IO, "cannot open the bank file");
        continue;
      }
      s.read.file = file;  // closed by FreeLoadMemory / after reading
      if (!AllocateImage(s, size)) continue;
      s.read.fs = &fs;
      s.read.file = file;
      s.read.dst = s.image;
      s.read.size = size;
      s.read.done = 0;
      s.read.chunkBytes = desc.ioChunkBytes;
      s.read.tokenBase = desc.ioTokenBase + (std::uint64_t{i} << 32);
      s.stage = Stage::Reading;
    }
    if (s.stage == Stage::Reading) {
      char why[128] = {};
      const detail::ReadStep st = detail::StepRead(s.read, s.cancel, why, sizeof(why));
      if (st == detail::ReadStep::Pending) continue;
      if (st == detail::ReadStep::Failed) {
        Fail(s, SND_ERR_IO, why);
        continue;
      }
      detail::CloseBankFile(fs, s.read.file);
      s.read.file = kInvalidFile;
      if (s.cancel) continue;  // freed by Pump
      StartValidation(s, i);
    }
  }
}

void BankManager::Pump() noexcept {
  base::LockGuard lock(impl_->mutex);
  while (impl_->PumpLocked()) {
  }
  // Nothing is in IO / validation any more but banks still wait for
  // dependencies: they can never be satisfied (missing bank or a cycle).
  bool loading = false;
  for (std::uint32_t i = 0; i < impl_->desc.maxBanks; ++i) {
    const Stage st = impl_->slots[i].stage;
    if ((st == Stage::Open || st == Stage::Reading || st == Stage::Validating) && !impl_->slots[i].cancel) {
      loading = true;
    }
  }
  if (loading) return;
  // Fail one bank whose dependency is missing at a time, then let the others
  // decide again (a second load of the same bank waits for the first one, so
  // it gets its own reason once the first has failed).
  for (;;) {
    Impl::Slot* victim = nullptr;
    std::uint32_t missing = 0;
    for (std::uint32_t i = 0; i < impl_->desc.maxBanks && victim == nullptr; ++i) {
      Impl::Slot& s = impl_->slots[i];
      if (s.stage != Stage::Validated) continue;
      for (std::uint32_t d = 0; d < s.prepared->dependencyCount; ++d) {
        const bf::Dependency dep = s.prepared->view.dependency(d);
        if (impl_->FindLoaded(dep.bankId, 0) == nullptr) {
          victim = &s;
          missing = dep.bankId;
          break;
        }
      }
    }
    if (victim == nullptr) break;
    char why[160];
    std::snprintf(why, sizeof(why), "dependency bank %08x is not loaded", missing);
    impl_->Fail(*victim, SND_ERR_BANK_DEPENDENCY, why);
    while (impl_->PumpLocked()) {
    }
  }
}

// One pass; true if anything changed (dependency chains resolve over passes).
bool Impl::PumpLocked() noexcept {
  bool changed = false;
  for (std::uint32_t i = 0; i < desc.maxBanks; ++i) {
    Slot& s = slots[i];
    switch (s.stage) {
      case Stage::Open:
        if (s.cancel && !IoInFlight(s)) {
          FreeSlot(s);
          changed = true;
        }
        break;
      case Stage::Reading:
        if (s.cancel && !IoInFlight(s)) {
          FreeSlot(s);
          changed = true;
        }
        break;
      case Stage::Validating:
        if (JobPending(s)) break;
        changed = true;
        if (s.cancel) {
          FreeSlot(s);
        } else if (s.jobResult != SND_OK) {
          const ParseError e = s.jobError;
          Fail(s, s.jobResult, e.message[0] != '\0' ? e.message : "validation job did not run");
        } else {
          s.stage = Stage::Validated;
          LearnLanguagePath(s);
        }
        break;
      case Stage::Validated:
        if (s.cancel) {
          FreeSlot(s);
          changed = true;
        } else if (Decide(s)) {
          changed = true;
        }
        break;
      default:
        break;
    }
  }
  return changed;
}

// Dependencies (10.3) and the DSP pool (3.3). true if the slot left Validated.
bool Impl::Decide(Slot& s) noexcept {
  PreparedBank& pb = *s.prepared;
  // The same bank (bankId + language) twice: the request order decides (D-50).
  for (std::uint32_t i = 0; i < desc.maxBanks; ++i) {
    const Slot& o = slots[i];
    if (&o == &s || o.cancel) continue;
    const bool earlier = o.requestSeq < s.requestSeq;
    // An earlier request whose identity is not known yet (reading / validating):
    // wait for its result. These stages always end without waiting for others.
    if (earlier && (o.stage == Stage::Open || o.stage == Stage::Reading || o.stage == Stage::Validating)) {
      return false;
    }
    // Only stages whose `prepared` is settled (a Validating slot's job may still write it).
    const bool settled = o.stage == Stage::Ready || o.stage == Stage::Registered || o.stage == Stage::Unloading ||
                         o.stage == Stage::Validated;
    if (!settled || o.prepared == nullptr) continue;
    if (o.prepared->bankId == pb.bankId && o.prepared->languageId == pb.languageId) {
      // The old copy is being unloaded: register once it is released, never fail for it.
      if (o.stage == Stage::Unloading) return false;
      if (o.stage == Stage::Validated) {
        if (earlier) return false;  // the earlier request is still deciding: wait for its outcome
        continue;                   // a later request: this one decides first
      }
      char why[192];
      std::snprintf(why, sizeof(why), "bank %08x is already loaded or loading from '%.120s'", pb.bankId,
                    o.path != nullptr ? o.path : "?");
      Fail(s, SND_ERR_BANK_ALREADY_LOADED, why);  // contract 1.1 (D-50)
      return true;
    }
  }
  for (std::uint32_t d = 0; d < pb.dependencyCount; ++d) {
    const bf::Dependency dep = pb.view.dependency(d);
    Slot* target = FindLoaded(dep.bankId, 0);
    if (target == nullptr) return false;  // maybe still loading: wait (Pump fails it when nothing is)
    if (target->prepared->view.contentHash() != dep.buildHash) {
      char why[160];
      std::snprintf(why, sizeof(why), "dependency bank %08x was built as %016llx but %016llx is loaded",
                    dep.bankId, static_cast<unsigned long long>(dep.buildHash),
                    static_cast<unsigned long long>(target->prepared->view.contentHash()));
      Fail(s, SND_ERR_BANK_DEPENDENCY, why);
      return true;
    }
  }
  if (pb.dspNeed.instanceBytes > 0 && desc.dsp.reserve != nullptr) {
    if (!desc.dsp.reserve(desc.dsp.user, pb.dspNeed)) {
      Fail(s, SND_ERR_OUT_OF_BUDGET, "DSP instance pool cannot hold the bank's effects");
      return true;
    }
    s.dspReserved = true;
  }
  for (std::uint32_t d = 0; d < pb.dependencyCount; ++d) {
    Slot* target = FindLoaded(pb.view.dependency(d).bankId, 0);
    ++target->dependents;
    pb.dependencies[d] = HandleOf(*target);
  }
  pb.handle = HandleOf(s);
  s.stage = Stage::Ready;
  PushQueue(static_cast<std::uint32_t>(&s - slots));
  return true;
}

bool BankManager::PopRegistration(cmd::BankRegister* out) noexcept {
  if (out == nullptr) return false;
  base::LockGuard lock(impl_->mutex);
  while (impl_->queueCount > 0) {
    const std::uint32_t idx = impl_->queue[impl_->queueHead];
    impl_->queueHead = (impl_->queueHead + 1) % impl_->desc.maxBanks;
    --impl_->queueCount;
    Impl::Slot& s = impl_->slots[idx];
    if (s.stage != Stage::Ready) continue;  // cancelled meanwhile
    s.stage = Stage::Registered;
    out->bank = impl_->HandleOf(s);
    out->bankId = s.prepared->bankId;
    out->languageId = s.prepared->languageId;
    out->prepared = s.prepared;
    return true;
  }
  return false;
}

void BankManager::MarkRegistered(HandleRef bank) noexcept {
  base::LockGuard lock(impl_->mutex);
  Impl::Slot* s = impl_->Get(bank);
  if (s != nullptr && s->stage == Stage::Registered) impl_->SetState(*s, SND_BANK_STATE_LOADED, SND_OK);
}

// ---------------------------------------------------------------------------
// Queries
// ---------------------------------------------------------------------------
snd_bank_state BankManager::State(HandleRef bank) const noexcept {
  base::LockGuard lock(impl_->mutex);
  const Impl::Slot* s = impl_->Get(bank);
  return s != nullptr ? impl_->AtomicState(*s) : SND_BANK_STATE_UNLOADED;
}

snd_result BankManager::Result(HandleRef bank) const noexcept {
  base::LockGuard lock(impl_->mutex);
  const Impl::Slot* s = impl_->Get(bank);
  return s != nullptr ? s->result : SND_ERR_INVALID_HANDLE;
}

const char* BankManager::FailureReason(HandleRef bank) const noexcept {
  base::LockGuard lock(impl_->mutex);
  const Impl::Slot* s = impl_->Get(bank);
  return s != nullptr ? s->reason : "";
}

const PreparedBank* BankManager::Prepared(HandleRef bank) const noexcept {
  base::LockGuard lock(impl_->mutex);
  const Impl::Slot* s = impl_->Get(bank);
  if (s == nullptr) return nullptr;
  return (s->stage == Stage::Ready || s->stage == Stage::Registered || s->stage == Stage::Unloading)
             ? s->prepared
             : nullptr;
}

HandleRef BankManager::FindBank(Id bankId, std::uint32_t languageId) const noexcept {
  base::LockGuard lock(impl_->mutex);
  const Impl::Slot* s = const_cast<Impl*>(impl_)->FindLoaded(bankId, languageId);
  return s != nullptr ? impl_->HandleOf(*s) : HandleRef{0, 0};
}

std::uint32_t BankManager::DependentCount(HandleRef bank) const noexcept {
  base::LockGuard lock(impl_->mutex);
  const Impl::Slot* s = impl_->Get(bank);
  return s != nullptr ? s->dependents : 0u;
}

notify::BankSlotState* BankManager::SlotStates() const noexcept { return impl_->states; }
std::uint32_t BankManager::MaxBanks() const noexcept { return impl_->desc.maxBanks; }

// ---------------------------------------------------------------------------
// Unload
// ---------------------------------------------------------------------------
snd_result BankManager::CheckUnload(HandleRef bank, std::uint32_t mode, bool referencedByPlaybacks) const noexcept {
  base::LockGuard lock(impl_->mutex);
  return impl_->CheckUnloadLocked(bank, mode, referencedByPlaybacks);
}

snd_result Impl::CheckUnloadLocked(HandleRef bank, std::uint32_t mode, bool referencedByPlaybacks) const noexcept {
  const Slot* s = Get(bank);
  if (s == nullptr || s->cancel) return SND_ERR_INVALID_HANDLE;
  if (mode != SND_UNLOAD_STOP_AND_UNLOAD && mode != SND_UNLOAD_IF_UNUSED) return SND_ERR_INVALID_ARGUMENT;
  if (s->dependents > 0) return SND_ERR_BANK_IN_USE;
  if (mode == SND_UNLOAD_IF_UNUSED && referencedByPlaybacks) return SND_ERR_BANK_IN_USE;
  return SND_OK;
}

snd_result BankManager::BeginUnload(HandleRef bank, std::uint32_t mode, bool referencedByPlaybacks) noexcept {
  // Check and change under one lock: no dependant can register in between.
  base::LockGuard lock(impl_->mutex);
  const snd_result r = impl_->CheckUnloadLocked(bank, mode, referencedByPlaybacks);
  if (r != SND_OK) return r;
  Impl::Slot* s = impl_->Get(bank);
  if (s == nullptr) return SND_ERR_INVALID_HANDLE;
  switch (s->stage) {
    case Stage::Open:
    case Stage::Reading:
    case Stage::Validating:
    case Stage::Validated:
      s->cancel = true;  // Pump frees it once no read / job touches the slot
      std::atomic_ref<std::uint32_t>(s->cancelFlag).store(1u, std::memory_order_release);
      impl_->SetState(*s, SND_BANK_STATE_UNLOADING, SND_OK);
      return SND_OK;
    case Stage::Ready:  // never handed to core: also leaves the registration queue
      s->stage = Stage::Failed;  // not READY any more while the queue is compacted
      impl_->RemoveFromQueue(bank.slot);
      impl_->FreeSlot(*s);
      return SND_OK;
    case Stage::Failed:
      impl_->FreeSlot(*s);
      return SND_OK;
    case Stage::Registered:
      s->stage = Stage::Unloading;
      impl_->SetState(*s, SND_BANK_STATE_UNLOADING, SND_OK);
      return SND_OK;
    case Stage::Unloading:
      return SND_OK;
    case Stage::Free:
      break;
  }
  return SND_ERR_INVALID_HANDLE;
}

void BankManager::AbortUnload(HandleRef bank) noexcept {
  base::LockGuard lock(impl_->mutex);
  Impl::Slot* s = impl_->Get(bank);
  if (s == nullptr || s->stage != Stage::Unloading) return;
  s->stage = Stage::Registered;
  impl_->SetState(*s, SND_BANK_STATE_LOADED, SND_OK);
}

snd_result BankManager::Release(HandleRef bank) noexcept {
  base::LockGuard lock(impl_->mutex);
  Impl::Slot* s = impl_->Get(bank);
  if (s == nullptr) return SND_ERR_INVALID_HANDLE;
  if (s->stage != Stage::Unloading && s->stage != Stage::Registered) return SND_ERR_INVALID_ARGUMENT;
  if (s->dependents > 0) return SND_ERR_BANK_IN_USE;
  impl_->FreeSlot(*s);
  return SND_OK;
}

// ---------------------------------------------------------------------------
// Languages (10.4)
// ---------------------------------------------------------------------------
snd_result BankManager::DeclareLanguage(const char* name) noexcept {
  // Contract 1.1 (D-50, snd_declare_language): 1..SND_MAX_LANGUAGE_NAME_BYTES-1
  // bytes, no '/', '.' or '\\' (the name is one segment of "<base>.<name>.sndb").
  if (name == nullptr || name[0] == '\0') return SND_ERR_INVALID_ARGUMENT;
  const std::size_t n = std::strlen(name);
  if (n >= SND_MAX_LANGUAGE_NAME_BYTES) return SND_ERR_INVALID_ARGUMENT;
  for (std::size_t i = 0; i < n; ++i) {
    if (name[i] == '/' || name[i] == '.' || name[i] == '\\') return SND_ERR_INVALID_ARGUMENT;
  }
  base::LockGuard lock(impl_->mutex);
  return impl_->AddLanguage(name, n);
}

Id BankManager::CurrentLanguage() const noexcept {
  base::LockGuard lock(impl_->mutex);
  return impl_->currentLanguage;
}

std::uint32_t BankManager::PlanLanguageSwitch(Id language, LanguageSwitch* out, std::uint32_t capacity,
                                              snd_result* result) const noexcept {
  snd_result dummy;
  snd_result& r = result != nullptr ? *result : dummy;
  r = SND_OK;
  base::LockGuard lock(impl_->mutex);
  const char* name = impl_->LanguageName(language);
  if (name == nullptr) {
    r = SND_ERR_NOT_FOUND;
    return 0;
  }
  std::uint32_t n = 0;
  for (std::uint32_t i = 0; i < impl_->desc.maxBanks; ++i) {
    const Impl::Slot& s = impl_->slots[i];
    // Loaded and still loading language banks of another language (10.4).
    if (s.stage == Stage::Free || s.stage == Stage::Unloading || s.stage == Stage::Failed || s.cancel) continue;
    if (s.langId == 0 || s.langId == language) continue;
    if (s.baseLen == 0) continue;  // path does not follow "<base>.<lang>.sndb"
    if (n == capacity || out == nullptr) {
      r = SND_ERR_LIMIT_REACHED;
      break;
    }
    LanguageSwitch& e = out[n];
    e = LanguageSwitch{};
    const int w = std::snprintf(e.newPath, sizeof(e.newPath), "%.*s.%s.sndb", static_cast<int>(s.baseLen),
                                s.path, name);
    if (w < 0 || static_cast<std::size_t>(w) >= sizeof(e.newPath)) {
      r = SND_ERR_LIMIT_REACHED;
      continue;
    }
    e.oldBank = impl_->HandleOf(s);
    ++n;
  }
  return n;
}

std::uint32_t BankManager::SetLanguage(Id language, LanguageSwitch* out, std::uint32_t capacity,
                                       snd_result* result) noexcept {
  snd_result r = SND_OK;
  const std::uint32_t n = PlanLanguageSwitch(language, out, capacity, &r);
  if (r != SND_ERR_NOT_FOUND) {
    {
      base::LockGuard lock(impl_->mutex);
      impl_->currentLanguage = language;
    }
    for (std::uint32_t i = 0; i < n; ++i) {
      snd_result lr = SND_OK;
      RequestLoad(out[i].newPath, &lr);
      if (lr != SND_OK && r == SND_OK) r = lr;
    }
  }
  if (result != nullptr) *result = r;
  return n;
}

}  // namespace snd::bank
