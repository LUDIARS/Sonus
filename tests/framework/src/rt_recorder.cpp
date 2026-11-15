// Records RT-safety violations reported through snd::rt::Report. Must not
// allocate, lock or perform IO: it runs inside malloc / mutex hooks.
#include <atomic>
#include <cstdlib>

#include "snd/internal/rt_check.hpp"
#include "snd/test/rt_guard.hpp"

namespace snd::test::rt {
namespace {

constexpr std::uint32_t kMaxRecords = 64;
constexpr std::uint32_t kKinds = static_cast<std::uint32_t>(Violation::Count);

std::atomic<std::uint32_t> gCounts[kKinds];
std::atomic<std::uint32_t> gTotal{0};
std::atomic<std::uint32_t> gUnexpected{0};
std::atomic<std::uint32_t> gRecordCount{0};
Record gRecords[kMaxRecords];
std::atomic<std::int32_t> gExpectDepth{0};
std::atomic<bool> gAbortOnUnexpected{false};
std::atomic<bool> gLibcHooks{false};
std::atomic<bool> gNewHooks{false};

void Handler(Violation kind, const char* site) noexcept {
  const bool expected = gExpectDepth.load(std::memory_order_acquire) > 0;
  const auto k = static_cast<std::uint32_t>(kind);
  if (k < kKinds) gCounts[k].fetch_add(1, std::memory_order_relaxed);
  gTotal.fetch_add(1, std::memory_order_relaxed);
  if (!expected) gUnexpected.fetch_add(1, std::memory_order_relaxed);
  const std::uint32_t idx = gRecordCount.fetch_add(1, std::memory_order_relaxed);
  if (idx < kMaxRecords) gRecords[idx] = Record{kind, site, expected};
  if (!expected && gAbortOnUnexpected.load(std::memory_order_relaxed)) std::abort();
}

}  // namespace

void Install() noexcept {
  const char* env = std::getenv("SND_RT_ABORT");
  gAbortOnUnexpected.store(env != nullptr && env[0] == '1');
  snd::rt::SetHandler(&Handler);
}

std::uint32_t Count(Violation kind) noexcept {
  const auto k = static_cast<std::uint32_t>(kind);
  return k < kKinds ? gCounts[k].load(std::memory_order_relaxed) : 0u;
}
std::uint32_t Total() noexcept { return gTotal.load(std::memory_order_relaxed); }
std::uint32_t UnexpectedTotal() noexcept { return gUnexpected.load(std::memory_order_relaxed); }

std::uint32_t Records(Record* out, std::uint32_t capacity) noexcept {
  std::uint32_t n = gRecordCount.load(std::memory_order_relaxed);
  if (n > kMaxRecords) n = kMaxRecords;
  if (n > capacity) n = capacity;
  for (std::uint32_t i = 0; i < n; ++i) out[i] = gRecords[i];
  return n;
}

void Reset() noexcept {
  for (auto& c : gCounts) c.store(0, std::memory_order_relaxed);
  gTotal.store(0, std::memory_order_relaxed);
  gUnexpected.store(0, std::memory_order_relaxed);
  gRecordCount.store(0, std::memory_order_relaxed);
}

bool LibcHooksActive() noexcept { return gLibcHooks.load(std::memory_order_relaxed); }
bool NewHooksActive() noexcept { return gNewHooks.load(std::memory_order_relaxed); }

ExpectViolations::ExpectViolations() noexcept { gExpectDepth.fetch_add(1, std::memory_order_acq_rel); }
ExpectViolations::~ExpectViolations() { gExpectDepth.fetch_sub(1, std::memory_order_acq_rel); }

}  // namespace snd::test::rt

// C entry points used by the hook object files (tests/framework/src/rt_hooks_*.c/cpp).
extern "C" void snd_test_rt_report(int kind, const char* site) {
  snd::rt::Report(static_cast<snd::rt::Violation>(kind), site);
}
extern "C" void snd_test_rt_mark_hooks(int libc, int newDelete) {
  if (libc) snd::test::rt::gLibcHooks.store(true);
  if (newDelete) snd::test::rt::gNewHooks.store(true);
}
extern "C" void snd_test_rt_suspend(int enter) {
#if SND_RT_CHECKS
  if (enter) {
    snd::rt::tSuspendDepth = snd::rt::tSuspendDepth + 1u;
  } else {
    snd::rt::tSuspendDepth = snd::rt::tSuspendDepth - 1u;
  }
#else
  static_cast<void>(enter);
#endif
}
