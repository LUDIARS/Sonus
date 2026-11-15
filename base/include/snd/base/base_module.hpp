// base module (design.md 12.1): allocators, arenas, pools, handles, SPSC queues, atomics, hashing, lock-free logging.
// Dependencies (frozen, root CMakeLists.txt): (none).
//
// Headers (all under snd/base/):
//   allocator.hpp   MallocAllocator, AccountingAllocator (per-MemTag stats/budgets), AllocateArray, AlignedBuffer
//   arena.hpp       LinearArena (frame arena), ThreadArenaSet (per RT thread)
//   pool.hpp        IndexFreeList, ConcurrentIndexFreeList, BlockPool, ConcurrentBlockPool
//   handle.hpp      handle bit helpers, HandleAllocator (game-side lock-free slot+generation issue)
//   sparse_set.hpp  rows:: operations on schema::TableRows (insert, swap-remove, generations)
//   chain.hpp       ChainPool + InlineChain (fixed-width array + chain of 16-entry blocks)
//   spsc.hpp        SpscRing, ArenaCycle (command arena publish/return loop)
//   mpmc.hpp        MpmcRing (bounded lock-free, for postJob/postIo and logs)
//   notify.hpp      end/release flag arrays, event ring, snapshot triple buffer + seqlock
//   hash.hpp        run-time HashId (== snd::HashId), IdHasher
//   atomic.hpp      CachePadded, CpuRelax, AtomicFetchMax, seqlock word copies
//   mutex.hpp       Mutex / LockGuard with SND_RT_NOTE(Lock)
//   log.hpp         LogRing (RT push of static format + args, non-RT formatting)
//   base.hpp        umbrella
#ifndef SND_BASE_BASE_MODULE_HPP
#define SND_BASE_BASE_MODULE_HPP

namespace snd::base {

// Module identification, used by the smoke test.
const char* ModuleName() noexcept;

}  // namespace snd::base

#endif  // SND_BASE_BASE_MODULE_HPP
