// OS audio workgroups (design.md 2.3 "優先度", 12.1 "スレッド優先度とワークグループ",
// CONTRACTS.md D-65).
//
// The audio thread and the RT workers should be scheduled together with the
// thread that services the output device, so the OS does not see them as
// unrelated work (priority inversion, CPU frequency, core selection). The
// output device publishes a Workgroup; every RT thread joins it from itself:
//
//   platform::WorkgroupMembership m;                  // one per thread
//   std::uint32_t gen = 0;
//   m.Join(device->CurrentWorkgroup(&gen));           // at thread start, outside SND_RT_SCOPE
//   ... if device->WorkgroupGeneration() != gen: m.Join(device->CurrentWorkgroup(&gen));
//   m.Leave();                                        // before the thread exits
//
// What a Workgroup is per OS:
//   Apple (macOS 11+, iOS 14+): the os_workgroup_t of the device IO thread
//     (kAudioOutputUnitProperty_OSWorkgroup). Join = os_workgroup_join.
//   Windows: the MMCSS task the WASAPI device thread registered with
//     ("Pro Audio"). Join = AvSetMmThreadCharacteristicsW with the same task
//     index, so the thread is in the same MMCSS task.
//   Linux / others: none (Valid() is false); SCHED_FIFO at the same priority
//     (thread.hpp) already places the threads in one class. Join returns
//     NoWorkgroup and does nothing.
//
// Threads: Join and Leave act on the CALLING thread and must be called from
// the same thread. They are non-RT in the strict sense (a system call each);
// call them at thread start or when the generation changes, not per block.
// The membership keeps its own reference to the workgroup, so the device may
// replace or release its workgroup while threads are still joined.
#ifndef SND_PLATFORM_WORKGROUP_HPP
#define SND_PLATFORM_WORKGROUP_HPP

#include <cstdint>

namespace snd::platform {

enum class WorkgroupKind : std::uint8_t {
  None = 0,
  AppleOsWorkgroup = 1,  // native = os_workgroup_t
  WindowsMmcss = 2,      // taskIndex = MMCSS task index (DWORD)
};

struct Workgroup {
  WorkgroupKind kind = WorkgroupKind::None;
  void* native = nullptr;        // AppleOsWorkgroup: os_workgroup_t (not retained by this struct)
  std::uint32_t taskIndex = 0;   // WindowsMmcss: task index (> 0 once registered)
  bool Valid() const noexcept {
    return (kind == WorkgroupKind::AppleOsWorkgroup && native != nullptr) ||
           (kind == WorkgroupKind::WindowsMmcss && taskIndex != 0u);
  }
};

enum class WorkgroupJoinResult : std::uint8_t {
  Joined = 0,
  NoWorkgroup = 1,   // the workgroup is not Valid() (Linux, device not started, old OS)
  Unsupported = 2,   // this build / OS version has no workgroup API
  Failed = 3,        // the OS refused (e.g. the workgroup was cancelled)
};

const char* WorkgroupJoinResultName(WorkgroupJoinResult r) noexcept;

class WorkgroupMembership {
 public:
  WorkgroupMembership() noexcept = default;
  ~WorkgroupMembership();  // leaves if still joined (must run on the joining thread)
  WorkgroupMembership(const WorkgroupMembership&) = delete;
  WorkgroupMembership& operator=(const WorkgroupMembership&) = delete;

  // Leaves the current workgroup (if any), then joins `group` from the calling thread.
  WorkgroupJoinResult Join(const Workgroup& group) noexcept;
  void Leave() noexcept;
  bool Joined() const noexcept { return joined_; }
  WorkgroupKind JoinedKind() const noexcept { return kind_; }

 private:
  // Apple: os_workgroup_join_token_s (40 bytes in the 11.0 SDK); Windows: AVRT HANDLE.
  alignas(16) unsigned char token_[64] = {};
  void* native_ = nullptr;  // retained os_workgroup_t / MMCSS HANDLE
  WorkgroupKind kind_ = WorkgroupKind::None;
  bool joined_ = false;
};

}  // namespace snd::platform

#endif  // SND_PLATFORM_WORKGROUP_HPP
