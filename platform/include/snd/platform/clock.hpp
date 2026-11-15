// Monotonic clock and sleeping (design.md 1.2 principle 1, 2.2, 2.4).
//
// Monotonic*() read the OS monotonic clock (CLOCK_MONOTONIC on POSIX, vDSO on
// Linux): RT-safe, any thread — principle 1 explicitly allows it.
// Sleep*() block the calling thread: NON-RT (they note SND_RT_NOTE(Blocking)).
#ifndef SND_PLATFORM_CLOCK_HPP
#define SND_PLATFORM_CLOCK_HPP

#include <cstdint>

namespace snd::platform {

// Nanoseconds since an arbitrary fixed origin (process-wide, never goes back).
std::uint64_t MonotonicNanoseconds() noexcept;
inline std::uint64_t MonotonicMicroseconds() noexcept { return MonotonicNanoseconds() / 1000u; }

// Non-RT. Sleep for at least `ns` / until MonotonicNanoseconds() >= deadline.
void SleepFor(std::uint64_t ns) noexcept;
void SleepUntil(std::uint64_t deadlineNs) noexcept;
// Non-RT. Gives up the rest of the time slice.
void YieldThread() noexcept;

}  // namespace snd::platform

#endif  // SND_PLATFORM_CLOCK_HPP
