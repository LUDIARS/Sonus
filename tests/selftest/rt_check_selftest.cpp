// Tests of the RT-safety detection hooks (design.md 14).
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <thread>

#include "snd/internal/rt_check.hpp"
#include "snd/mocks/memory_file_system.hpp"
#include "snd/test/rt_guard.hpp"
#include "snd/test/test.hpp"

namespace rtk = snd::test::rt;
using snd::rt::Violation;

namespace {
// Defeat optimisation of new/delete pairs (allowed to be elided by the compiler).
void* volatile gSink = nullptr;
}  // namespace

SND_TEST(RtCheck, NothingRecordedOutsideScope) {
  if (!snd::rt::kEnabled) return;
  int* p = new int(3);
  gSink = p;
  delete p;
  std::mutex m;
  m.lock();
  m.unlock();
  SND_CHECK_EQ(rtk::Total(), 0u);
  SND_CHECK(!snd::rt::InRtScope());
}

SND_TEST(RtCheck, OperatorNewInScopeIsDetected) {
  if (!snd::rt::kEnabled) return;
  SND_REQUIRE(rtk::NewHooksActive());
  {
    rtk::ExpectViolations expect;
    SND_RT_SCOPE();
    SND_CHECK(snd::rt::InRtScope());
    int* p = new int(7);
    gSink = p;
    delete p;
  }
  SND_CHECK_EQ(rtk::Count(Violation::Alloc), 1u);
  SND_CHECK_EQ(rtk::Count(Violation::Free), 1u);
  SND_CHECK_EQ(rtk::UnexpectedTotal(), 0u);
  rtk::Record r[4];
  SND_REQUIRE_EQ(rtk::Records(r, 4), 2u);
  SND_CHECK(r[0].kind == Violation::Alloc && r[0].expected);
}

SND_TEST(RtCheck, ExplicitNotesAreDetected) {
  if (!snd::rt::kEnabled) return;
  {
    rtk::ExpectViolations expect;
    snd::rt::RtScope scope;
    SND_RT_NOTE(Lock);
    SND_RT_NOTE(FileIo);
    SND_RT_NOTE(Log);
  }
  SND_CHECK_EQ(rtk::Count(Violation::Lock), 1u);
  SND_CHECK_EQ(rtk::Count(Violation::FileIo), 1u);
  SND_CHECK_EQ(rtk::Count(Violation::Log), 1u);
}

SND_TEST(RtCheck, MockFileSystemNotesFileIo) {
  if (!snd::rt::kEnabled) return;
  snd::mock::MemoryFileSystem fs;
  fs.AddFile("a.bin", std::vector<std::uint8_t>{1, 2, 3});
  {
    rtk::ExpectViolations expect;
    SND_RT_SCOPE();
    const snd::FileId id = fs.Open("a.bin");
    gSink = reinterpret_cast<void*>(static_cast<std::uintptr_t>(id));
  }
  SND_CHECK(rtk::Count(Violation::FileIo) >= 1u);
}

SND_TEST(RtCheck, LibcHooksDetectMallocMutexAndFiles) {
  if (!snd::rt::kEnabled) return;
  if (!rtk::LibcHooksActive()) {
    snd::test::Note("libc hooks inactive in this build (sanitizer or non-glibc); skipped");
    return;
  }
  std::mutex m;
  {
    rtk::ExpectViolations expect;
    SND_RT_SCOPE();
    void* p = std::malloc(16);
    gSink = p;
    std::free(p);
    m.lock();
    m.unlock();
    std::FILE* f = std::fopen("/nonexistent-sonus-rt-check", "rb");
    if (f != nullptr) std::fclose(f);
  }
  SND_CHECK(rtk::Count(Violation::Alloc) >= 1u);
  SND_CHECK(rtk::Count(Violation::Free) >= 1u);
  SND_CHECK(rtk::Count(Violation::Lock) >= 1u);
  SND_CHECK(rtk::Count(Violation::FileIo) >= 1u);
}

SND_TEST(RtCheck, UnexpectedViolationIsCountedAndResettable) {
  if (!snd::rt::kEnabled) return;
  {
    SND_RT_SCOPE();
    SND_RT_NOTE(Blocking);  // not inside ExpectViolations
  }
  SND_CHECK_EQ(rtk::UnexpectedTotal(), 1u);
  // Reset so that the runner does not fail this test; the failing path is
  // covered by the rt_violation_fails_test executable (WILL_FAIL).
  rtk::Reset();
  SND_CHECK_EQ(rtk::Total(), 0u);
}

SND_TEST(RtCheck, ScopesNestAndArePerThread) {
  if (!snd::rt::kEnabled) return;
  std::atomic<bool> otherInScope{true};
  {
    SND_RT_SCOPE();
    {
      SND_RT_SCOPE();
      SND_CHECK(snd::rt::InRtScope());
    }
    SND_CHECK(snd::rt::InRtScope());
    std::thread t;
    {
      snd::rt::SuspendScope allow;  // creating a thread allocates; not part of the check
      t = std::thread([&] { otherInScope = snd::rt::InRtScope(); });
      t.join();
    }
  }
  SND_CHECK(!snd::rt::InRtScope());
  SND_CHECK(!otherInScope.load());
  SND_CHECK_EQ(rtk::Total(), 0u);
}

SND_TEST(RtCheck, ReleaseBuildsCompileHooksOut) {
  // With SND_RT_CHECKS=0 the scope is an empty type and the macros vanish.
  // This build may have checks on; the property is asserted at compile time.
  static_assert(sizeof(snd::rt::RtScope) <= 1, "RtScope must stay a zero-cost marker");
  SND_CHECK(true);
}
