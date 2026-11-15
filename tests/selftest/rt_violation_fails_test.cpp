// Must FAIL (registered with WILL_FAIL): an allocation inside an RT scope
// without ExpectViolations makes the runner fail the test.
#include "snd/internal/rt_check.hpp"
#include "snd/test/test.hpp"

namespace {
void* volatile gSink = nullptr;
}

SND_TEST(RtViolation, AllocationInRtScopeFailsTheTest) {
#if SND_RT_CHECKS
  SND_RT_SCOPE();
  int* p = new int(1);
  gSink = p;
  delete p;
#else
  SND_CHECK(false);  // keep WILL_FAIL meaningful when checks are compiled out
#endif
}
