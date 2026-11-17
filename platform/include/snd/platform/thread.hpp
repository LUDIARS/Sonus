// Threads and scheduling priority (design.md 2 thread table, 2.3).
//
// Thread creation, Join and priority changes are NON-RT (init / shutdown).
//
// Priority policy (Linux): ThreadPriority::Realtime asks for SCHED_FIFO at
// ThreadDesc::realtimePriority. If the process lacks the permission
// (EPERM: no CAP_SYS_NICE / RLIMIT_RTPRIO), the thread keeps running at
// normal priority and the result says PriorityResult::FellBack — it is never
// an error.
// Windows: THREAD_PRIORITY_TIME_CRITICAL (the audio thread and RT workers
// also join the WASAPI device's MMCSS task, workgroup.hpp). Apple: the Mach
// time-constraint policy (nominal 5.3 ms period; joining the device's
// os_workgroup, workgroup.hpp, gives the real deadline). Other OSes: not
// applied, the result is PriorityResult::Unsupported.
#ifndef SND_PLATFORM_THREAD_HPP
#define SND_PLATFORM_THREAD_HPP

#include <atomic>
#include <cstddef>
#include <cstdint>

namespace snd::platform {

enum class ThreadPriority : std::uint8_t {
  Normal = 0,    // normal workers, IO, live link
  Realtime = 1,  // audio thread, RT workers, timer-driven device (same group, 2.3)
};

enum class PriorityResult : std::uint8_t {
  Applied = 0,      // requested priority is in effect
  FellBack = 1,     // realtime refused (permissions); running at normal priority
  Unsupported = 2,  // this OS build does not implement priorities (stub)
  Failed = 3,       // unexpected OS error; running at whatever the OS gave
};

const char* PriorityResultName(PriorityResult r) noexcept;

// Stack guaranteed to the audio thread and the RT workers (CONTRACTS.md D-47):
// the sinc resampler uses ~32 KiB above ratio 4, plus effect / source scratch;
// plugin code may use about 64 KiB of it (snd/ext.h).
inline constexpr std::size_t kRtThreadMinStackBytes = 256u * 1024u;

struct ThreadDesc {
  const char* name = "snd";  // shown in debuggers (Linux: first 15 bytes)
  ThreadPriority priority = ThreadPriority::Normal;
  int realtimePriority = 80;  // SCHED_FIFO 1..99 when priority == Realtime
  std::size_t stackBytes = 0;  // 0 = OS default
  // Lower bound of the stack (0 = none). With stackBytes == 0 the OS default is
  // kept when it is at least this large, otherwise this size is requested; with
  // stackBytes != 0 the larger of the two is requested. Start fails if the OS
  // refuses the size.
  std::size_t minStackBytes = 0;
};

using ThreadFn = void (*)(void* user);

class Thread {
 public:
  Thread() noexcept = default;
  ~Thread();  // joins if still joinable
  Thread(const Thread&) = delete;
  Thread& operator=(const Thread&) = delete;

  // Non-RT. Starts fn(user) on a new thread and applies the priority.
  // false if the OS could not create the thread.
  bool Start(const ThreadDesc& desc, ThreadFn fn, void* user) noexcept;
  // Non-RT. Waits for the thread to finish. No-op if not started.
  void Join() noexcept;
  bool Joinable() const noexcept { return started_; }
  // Result of the priority request made by Start.
  PriorityResult Priority() const noexcept { return priority_; }

 private:
  struct Native;
  alignas(8) unsigned char native_[16] = {};
  bool started_ = false;
  PriorityResult priority_ = PriorityResult::Applied;
};

// Non-RT. Applies `priority` to the calling thread (same policy as above).
PriorityResult SetCurrentThreadPriority(ThreadPriority priority, int realtimePriority = 80) noexcept;
// Non-RT. Names the calling thread (best effort).
void SetCurrentThreadName(const char* name) noexcept;
// Logical CPU count (>= 1).
std::uint32_t HardwareConcurrency() noexcept;

}  // namespace snd::platform

#endif  // SND_PLATFORM_THREAD_HPP
