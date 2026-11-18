/*
 * Sonus contract version and portability macros.
 *
 * FROZEN CONTRACT (spec/interface/CONTRACTS.md). Do not edit without a
 * contract change proposal; any change bumps SND_CONTRACT_VERSION_*.
 */
#ifndef SND_VERSION_H
#define SND_VERSION_H

/*
 * Contract version. MAJOR changes break source or binary compatibility of a
 * frozen file; MINOR changes only add (new functions, new enum values at the
 * end, new fields guarded by struct_size, new bank chunks / columns, new live
 * link messages). The bank format and the live link protocol carry their own
 * versions (snd/format/<name>.hpp) which are listed in CONTRACTS.md.
 */
#define SND_CONTRACT_VERSION_MAJOR 1
#define SND_CONTRACT_VERSION_MINOR 2
#define SND_CONTRACT_VERSION ((SND_CONTRACT_VERSION_MAJOR << 16) | SND_CONTRACT_VERSION_MINOR)

#if defined(__cplusplus)
#  define SND_EXTERN_C_BEGIN extern "C" {
#  define SND_EXTERN_C_END }
#  define SND_STATIC_ASSERT(cond, msg) static_assert(cond, msg)
#  define SND_NOEXCEPT noexcept
#else
#  define SND_EXTERN_C_BEGIN
#  define SND_EXTERN_C_END
#  define SND_STATIC_ASSERT(cond, msg) _Static_assert(cond, msg)
#  define SND_NOEXCEPT
#endif

/* Symbol visibility. Static builds need nothing; shared builds define SND_SHARED. */
#if defined(SND_SHARED)
#  if defined(_WIN32)
#    if defined(SND_BUILDING)
#      define SND_API __declspec(dllexport)
#    else
#      define SND_API __declspec(dllimport)
#    endif
#  else
#    define SND_API __attribute__((visibility("default")))
#  endif
#else
#  define SND_API
#endif

/* Calling convention of every callback and API function (empty on all current targets). */
#define SND_CALL

#endif /* SND_VERSION_H */
