// platform module (design.md 12.1): output devices, thread priority and work groups, clocks, FTZ/DAZ, SIMD abstraction.
// Dependencies (frozen, root CMakeLists.txt): base.
//
// Headers owned by the base/platform/jobs team (all under snd/platform/):
//   clock.hpp          MonotonicNanoseconds (RT-safe), SleepFor/SleepUntil (non-RT)
//   thread.hpp         Thread (pthread), ThreadPriority / PriorityResult (SCHED_FIFO with fallback)
//   semaphore.hpp      Semaphore created up front (futex): the RT-allowed wake/wait primitive
//   cpu.hpp            CpuFeatures, SetFlushDenormals / ScopedFlushDenormals (FTZ/DAZ)
//   output_device.hpp  TimerOutputDevice (silent, timer driven), CreateOutputDevice factory
// The SIMD abstraction (simd*.hpp) is owned by the dsp team.
#ifndef SND_PLATFORM_PLATFORM_MODULE_HPP
#define SND_PLATFORM_PLATFORM_MODULE_HPP

namespace snd::platform {

// Module identification, used by the smoke test.
const char* ModuleName() noexcept;

}  // namespace snd::platform

#endif  // SND_PLATFORM_PLATFORM_MODULE_HPP
