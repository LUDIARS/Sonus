// Bank slots, the loading path, dependencies and release (design.md 10.3, 10.4).
//
// Loading path (10.3), one bank:
//
//   game thread    RequestLoad(path)            slot -> LOADING (same path: existing handle)
//   IO thread      PumpIo()                     Open / Size / ReadAsync / Poll / Close (IFileSystem)
//                                               then submits the validation job (background lane)
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
// All IFileSystem calls are in PumpIo (and src/bank_io.cpp); the io module's
// scheduler will replace that single place in the next stage.
//
// Threads: every member function is safe to call from any non-RT thread (an
// internal mutex serialises them; the validation job does not take it). The
// audio thread only touches SlotStates() atomics and PreparedBank data.
#ifndef SND_BANK_BANK_MANAGER_HPP
#define SND_BANK_BANK_MANAGER_HPP

#include <cstdint>

#include "snd/bank/prepared.hpp"
#include "snd/ext/allocator.hpp"
#include "snd/ext/file_system.hpp"
#include "snd/ext/job_system.hpp"
#include "snd/internal/commands.hpp"
#include "snd/internal/common.hpp"
#include "snd/internal/notify.hpp"
#include "snd/types.h"

namespace snd::bank {

// DSP instance pool hook (3.3): core / dsp reserve the bank's DSPN need from
// the pool. Returning false fails the load with SND_ERR_OUT_OF_BUDGET.
struct DspPoolHooks {
  void* user = nullptr;
  bool (*reserve)(void* user, const bank_format::DspPoolNeed& need) = nullptr;
  void (*release)(void* user, const bank_format::DspPoolNeed& need) = nullptr;
};

struct BankManagerDesc {
  IAllocator* allocator = nullptr;    // required
  IFileSystem* fileSystem = nullptr;  // required (IO thread only)
  IJobSystem* jobs = nullptr;         // required (background lane)
  std::uint32_t maxBanks = 64;        // Config.maxBanks
  PrepareOptions prepare;             // platform, dev override layer, devReserve
  std::uint32_t ioChunkBytes = 1u << 20;  // bytes per ReadAsync
  std::uint64_t ioTokenBase = 0;      // high bits for snd_io_token values (unique per FS user)
  // External slot state array (core's notification block), maxBanks entries.
  // nullptr: the manager allocates its own (MemTag::Notify).
  notify::BankSlotState* slotStates = nullptr;
  DspPoolHooks dsp;
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

  // --- IO thread --------------------------------------------------------------------
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
  // CheckUnload, then: loaded -> UNLOADING (core records cmd::BankUnload);
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
