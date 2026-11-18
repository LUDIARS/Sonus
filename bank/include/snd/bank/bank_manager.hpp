// Bank slots, the loading path, dependencies and release (design.md 10.3, 10.4).
//
// Loading path (10.3), one bank:
//
//   game thread    RequestLoad(path)            slot -> LOADING (same path: existing handle)
//   any non-RT     PumpIo()                     BankIoMode::Scheduler (default): submits io FileRequests
//                                               (size query, then one whole-file read, IoClass::Bank) to
//                                               the io module's IoScheduler and polls them; its IO thread
//                                               does the IFileSystem calls (6.5, 10.3).
//                                               BankIoMode::DirectFileSystem (tests, single-threaded
//                                               hosts): PumpIo runs on the IO thread and calls the
//                                               IFileSystem itself (Open / Size / ReadAsync / Poll / Close).
//                                               Then submits the validation job (background lane)
//   normal worker  (job) PrepareBank            validate (§8) + DSP need + dev override layer
//   any non-RT     Pump()                       dependencies loaded and matching? DSP pool reserved?
//                                               -> queue a cmd::BankRegister, or FAILED
//   core           PopRegistration(&cmd)        records the BankRegister command (Loader origin)
//   audio thread   applies it, stores SND_BANK_STATE_LOADED into SlotStates()[slot].state
//                  (or a non-RT caller uses MarkRegistered)
//
// Unload: CheckUnload answers both UnloadBank modes' preconditions (dependants,
// "referenced by playbacks" is supplied by core). BeginUnload sets UNLOADING;
// core unregisters at a block boundary, waits for its epoch, then calls
// Release(handle): memory goes back to the IAllocator and the slot to UNLOADED.
// A bank that is still loading (or failed) is cancelled / freed by
// BeginUnload + Pump without ever being registered.
//
// IO (stage 1.5, A1-11): src/bank_io.cpp holds both paths. Scheduler mode
// never calls the IFileSystem: every byte goes through IoScheduler, which
// orders bank reads with the stream reads by class and bandwidth.
//
// Cancellation and shutdown (Scheduler mode):
//  - BeginUnload / a cancelled load of a bank whose read is in flight calls
//    IoScheduler::Cancel(FileRequest); the slot (path, image) is freed only
//    once the request Finished() (the scheduler may still write the image until
//    then), by a later Pump(). Cancelled loads end UNLOADED, never FAILED.
//  - IoScheduler::Shutdown() while banks are loading: their FileRequests end
//    Cancelled; those banks end FAILED with SND_ERR_IO ("IO scheduler shut
//    down") at the next PumpIo(), and every later RequestLoad fails the same
//    way (the scheduler accepts nothing any more). Validated / registered banks
//    are not affected.
//  - Teardown order: stop the audio thread, IoScheduler::Shutdown(), drain the
//    job system (no validation job left), BankManager::Destroy(), then
//    IoScheduler::Destroy(). Destroy() with a request still in flight cancels
//    it and, if the scheduler's IO thread is running, waits for it to finish;
//    without a running IO thread such slots are leaked (as with a running job).
//    By design Destroy() waits for as long as the file system takes to finish
//    the read in flight: an IFileSystem that never completes a read makes it
//    wait forever (the image may not be freed under the read).
//
// The same bank twice (bankId + language, D-50): the request order decides
// (RequestLoad's acceptance sequence; a retry in place is a new request). A
// load waits before registering while an EARLIER request has not produced its
// result: still reading / validating (its identity is not known yet), or the
// same bank validated and waiting itself. The earlier one succeeding fails the
// later with SND_ERR_BANK_ALREADY_LOADED; the earlier one failing or being
// cancelled lets the later register. While an old slot of the same bank is
// UNLOADING (same path or not), a new load is read and validated but waits
// for the old slot's Release() before it registers (never fails for it).
// A bank that depends on such a waiting copy waits too (its dependency is not
// "missing" while the copy waits for the Release; that holds transitively),
// and so does a bank whose dependency's copy waits for an earlier copy's
// outcome. SND_ERR_BANK_DEPENDENCY is given only once it is certain that no
// copy of the dependency will ever be registered (a really missing bank, a
// cycle); every such bank of a round fails together, so the outcome does not
// depend on slot order. Dependencies are matched by bankId and buildHash only,
// not by language: a dependency resolves to a registered copy whose bankId AND
// content hash match, in whatever slot (several such copies: the one requested
// first gets the reference). Only when no matching copy is registered and no
// copy of that bankId can still come does the bank fail ("built as ... but ... is loaded" when only a
// mismatching copy is loaded; this is decided once nothing is reading or
// validating any more). Not by language (which language copy a dependency should resolve to is
// left to stage 2, snd_set_language). Side effect
// of the request order: a later, unrelated bank also waits to register until
// the earlier requests finished reading and validating.
//
// Threads: every member function is safe to call from any non-RT thread (an
// internal mutex serialises them; the validation job does not take it). In
// DirectFileSystem mode PumpIo() must be called on the thread that owns the
// IFileSystem. The audio thread only touches SlotStates() atomics and
// PreparedBank data.
#ifndef SND_BANK_BANK_MANAGER_HPP
#define SND_BANK_BANK_MANAGER_HPP

#include <cstdint>

#include "snd/bank/prepared.hpp"
#include "snd/ext.h"
#include "snd/ext/allocator.hpp"
#include "snd/ext/file_system.hpp"
#include "snd/ext/job_system.hpp"
#include "snd/internal/commands.hpp"
#include "snd/internal/common.hpp"
#include "snd/internal/notify.hpp"
#include "snd/types.h"

namespace snd::io {
class IoScheduler;  // snd/io/io_scheduler.hpp (bank depends on io, 12.1)
}  // namespace snd::io

namespace snd::bank {

// Where the bank bytes come from (see the header comment).
enum class BankIoMode : std::uint8_t {
  Scheduler = 0,         // default: io::IoScheduler FileRequests (IoClass::Bank)
  DirectFileSystem = 1,  // IFileSystem calls from PumpIo (tests, single-threaded hosts)
};

// DSP instance pool hook (3.3): core / dsp reserve the bank's need from the
// pool. Returning false fails the load with SND_ERR_OUT_OF_BUDGET.
//
// Contract 1.1 D-54: the need is the RUNTIME's. When `compute` is set, the
// validation job builds one snd_effect_desc per EFFX row (effect type, EFPV
// values, EFBL blob, max_frames = BankManagerDesc::blockSize, sample_rate =
// BankManagerDesc::sampleRate, channels = the bus's, else the nearest parent's,
// else BankManagerDesc::outputChannels; an EFFX row of no bus uses
// outputChannels) and calls compute(); the composition root passes
// dsp::ComputeDspPoolNeed with Config.plugins.effects (bank cannot depend on
// dsp, 12.1). The result goes to PreparedBank::dspNeed and is what reserve()
// gets; the bank's DSPN stays in PreparedBank::builtDspNeed as a reference
// (a difference is not an error). compute == nullptr: the bank's DSPN is used.
// compute returning false fails the load with SND_ERR_INVALID_ARGUMENT.
struct DspPoolHooks {
  void* user = nullptr;
  bool (*reserve)(void* user, const bank_format::DspPoolNeed& need) = nullptr;
  void (*release)(void* user, const bank_format::DspPoolNeed& need) = nullptr;
  bool (*compute)(void* user, const snd_effect_desc* descs, std::uint32_t count, std::uint32_t sampleRate,
                  bank_format::DspPoolNeed* out) = nullptr;
};

struct BankManagerDesc {
  IAllocator* allocator = nullptr;    // required
  BankIoMode ioMode = BankIoMode::Scheduler;
  io::IoScheduler* scheduler = nullptr;  // required in Scheduler mode (must outlive the manager)
  IFileSystem* fileSystem = nullptr;  // required in DirectFileSystem mode (PumpIo on the IO thread)
  IJobSystem* jobs = nullptr;         // required (background lane)
  std::uint32_t maxBanks = 64;        // Config.maxBanks
  PrepareOptions prepare;             // platform, dev override layer, devReserve
  std::uint32_t ioChunkBytes = 1u << 20;  // DirectFileSystem: bytes per ReadAsync (Scheduler: its filePieceBytes)
  std::uint64_t ioTokenBase = 0;      // DirectFileSystem: high bits for snd_io_token values (unique per FS user)
  // External slot state array (core's notification block), maxBanks entries.
  // nullptr: the manager allocates its own (MemTag::Notify).
  notify::BankSlotState* slotStates = nullptr;
  DspPoolHooks dsp;
  // Runtime values the effect descs are built with (D-54): Config.sampleRate,
  // Config.blockSize, and the channel count of the output (master) bus used for
  // buses that are 0 up to the root.
  std::uint32_t sampleRate = 48000;
  std::uint32_t blockSize = 256;
  std::uint32_t outputChannels = 2;
};

// One step of a language switch (10.4): unload `oldBank` once `newPath` is
// loaded and its voices have finished.
struct LanguageSwitch {
  HandleRef oldBank{};
  char newPath[256] = {};
};

class BankManager {
 public:
  // Allocates the manager and its slot table from desc.allocator (MemTag::Misc).
  static snd_result Create(const BankManagerDesc& desc, BankManager** out) noexcept;
  // Frees every bank. Jobs still in flight keep their memory (leaked on
  // purpose rather than freed under a running job): drain the job system first.
  static void Destroy(BankManager* manager) noexcept;

  // --- game thread -------------------------------------------------------------------
  // Starts loading `path`. Same path while LOADING / LOADED: the existing
  // handle (result SND_OK). A FAILED slot of the same path is retried in place.
  // No free slot: none handle and SND_ERR_LIMIT_REACHED.
  HandleRef RequestLoad(const char* path, snd_result* result = nullptr) noexcept;

  // --- IO (Scheduler mode: any non-RT thread; DirectFileSystem: the IO thread) ---------
  void PumpIo() noexcept;

  // --- any non-RT thread ---------------------------------------------------------------
  // Finalises validated loads (dependencies, DSP pool), cancels, queues registrations.
  void Pump() noexcept;
  // Convenience for single-threaded hosts and tests: PumpIo() then Pump().
  void Update() noexcept {
    PumpIo();
    Pump();
  }
  // Next registration to record (FIFO). false when none is ready.
  bool PopRegistration(cmd::BankRegister* out) noexcept;
  // Marks a popped registration as applied (equivalent to the audio thread
  // storing SND_BANK_STATE_LOADED into SlotStates()).
  void MarkRegistered(HandleRef bank) noexcept;

  // State / last result; stale handles read UNLOADED / SND_ERR_INVALID_HANDLE.
  snd_bank_state State(HandleRef bank) const noexcept;
  snd_result Result(HandleRef bank) const noexcept;
  // Reason of the last failure of this slot (empty if none).
  const char* FailureReason(HandleRef bank) const noexcept;
  // Validated bank of a slot that is registering or loaded (nullptr otherwise).
  const PreparedBank* Prepared(HandleRef bank) const noexcept;
  // Slot of a registering / loaded bank (languageId 0 = any).
  HandleRef FindBank(Id bankId, std::uint32_t languageId = 0) const noexcept;
  // Number of registering / loaded banks that depend on `bank`.
  std::uint32_t DependentCount(HandleRef bank) const noexcept;

  // --- unload (10.3) -------------------------------------------------------------------
  // SND_OK if `bank` may be unloaded with `mode` (snd_unload_mode).
  // SND_ERR_BANK_IN_USE when a loaded bank depends on it (both modes) or, for
  // IF_UNUSED, when `referencedByPlaybacks`. SND_ERR_INVALID_HANDLE if stale.
  snd_result CheckUnload(HandleRef bank, std::uint32_t mode, bool referencedByPlaybacks) const noexcept;
  // CheckUnload, then (under the same lock, no gap between the check and the
  // change): loaded -> UNLOADING (core records cmd::BankUnload);
  // loading / failed / not yet popped -> cancelled and freed by Pump.
  snd_result BeginUnload(HandleRef bank, std::uint32_t mode, bool referencedByPlaybacks) noexcept;
  // The audio side refused the unload (IF_UNUSED found references): back to LOADED.
  void AbortUnload(HandleRef bank) noexcept;
  // "Release now": the bank is unregistered and no reference is left (epoch).
  // Frees image, prepared data and overlay, returns DSP pool, slot -> UNLOADED.
  snd_result Release(HandleRef bank) noexcept;

  // --- languages (10.4) ------------------------------------------------------------------
  // Language bank files are named "<base>.<language>.sndb" (buildtool). The
  // manager knows a language's name from DeclareLanguage or from the path of
  // a loaded language bank whose segment hashes to its languageId.
  // DeclareLanguage (D-50): same name again -> SND_OK; a different name with
  // the same id (hash collision) -> SND_ERR_INVALID_ARGUMENT.
  snd_result DeclareLanguage(const char* name) noexcept;
  // Fills `out` with one entry per loaded/loading language bank of another
  // language: the bank to drop and the path of its `language` counterpart.
  // Returns the number of entries (*result: SND_ERR_NOT_FOUND if the
  // language's name is unknown, SND_ERR_LIMIT_REACHED if `capacity` was too small).
  std::uint32_t PlanLanguageSwitch(Id language, LanguageSwitch* out, std::uint32_t capacity,
                                   snd_result* result = nullptr) const noexcept;
  Id CurrentLanguage() const noexcept;
  // PlanLanguageSwitch + RequestLoad of every new path; sets CurrentLanguage.
  // `out` receives the plan (old banks to unload when the new ones are LOADED).
  std::uint32_t SetLanguage(Id language, LanguageSwitch* out, std::uint32_t capacity,
                            snd_result* result = nullptr) noexcept;

  // --- core ----------------------------------------------------------------------------
  notify::BankSlotState* SlotStates() const noexcept;
  std::uint32_t MaxBanks() const noexcept;

  struct Impl;

 private:
  BankManager() = default;
  ~BankManager() = default;
  Impl* impl_ = nullptr;
};

}  // namespace snd::bank

#endif  // SND_BANK_BANK_MANAGER_HPP
