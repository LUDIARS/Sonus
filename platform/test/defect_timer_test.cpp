// FAILS on the current implementation (kept failing on purpose; see the report).
//
// TimerOutputDevice::Run computes the deadline of period k as
//     start + (k * periodFrames * 1e9) / sampleRate
// in 64-bit arithmetic. k * periodFrames * 1e9 == elapsed_ns * sampleRate, so
// it wraps after 2^64 / (sampleRate * 1e9) seconds of continuous running:
// 106.7 h at 48 kHz, 53.4 h at 96 kHz, 26.7 h at 192 kHz. After the wrap the
// deadline is in the past, the resync formula (now - start) * sampleRate wraps
// the same way, and the loop calls the device callback back to back without
// sleeping (a busy loop at the device thread's priority, SCHED_FIFO when
// granted). The same arithmetic is reached in ~4.3 s with sampleRate =
// 0xFFFFFFFF, which this test uses to reproduce it in real time.
#include <atomic>
#include <cstdint>

#include "snd/base/allocator.hpp"
#include "snd/platform/clock.hpp"
#include "snd/platform/output_device.hpp"
#include "snd/test/test.hpp"

using namespace snd::platform;

namespace {
std::atomic<std::uint64_t> gCalls{0};
void SND_CALL Count(void*, float*, std::uint32_t, std::uint32_t) { gCalls.fetch_add(1, std::memory_order_relaxed); }
}  // namespace

SND_TEST(DefectTimerDevice, NoCallbackBurstAfterTheDeadlineArithmeticWraps) {
  TimerDeviceConfig cfg;
  cfg.realtimeThread = false;  // the defect is a busy loop: do not run it at SCHED_FIFO
  TimerOutputDevice dev(snd::base::DefaultAllocator(), cfg);
  // period = 4294967 / 4294967295 s = 1.000 ms; the wrap happens at k ~ 4295 (~4.3 s).
  const std::uint32_t rate = 0xFFFFFFFFu, period = 4294967u;
  SND_REQUIRE(dev.Open(snd::DeviceDesc{rate, 1, period, 0}, &Count, nullptr));
  const std::uint64_t t0 = MonotonicNanoseconds();
  SND_REQUIRE(dev.Start());
  SleepFor(6000000000ull);
  dev.Stop();
  const double elapsedPeriods = static_cast<double>(MonotonicNanoseconds() - t0) * rate / period / 1e9;
  snd::test::Note("callbacks=%llu, elapsed periods=%.0f, late wakeups=%llu",
                  static_cast<unsigned long long>(gCalls.load()), elapsedPeriods,
                  static_cast<unsigned long long>(dev.LateWakeups()));
  SND_CHECK_MSG(static_cast<double>(gCalls.load()) <= elapsedPeriods + 2.0,
                "%llu callbacks in %.0f periods: the device thread stopped sleeping",
                static_cast<unsigned long long>(gCalls.load()), elapsedPeriods);
}
