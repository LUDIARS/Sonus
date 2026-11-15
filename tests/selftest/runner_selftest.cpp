// Tests of the test runner itself.
#include <cstdint>
#include <cstring>

#include "snd/test/test.hpp"

SND_TEST(Runner, PassingChecksDoNotFail) {
  SND_CHECK(1 + 1 == 2);
  SND_CHECK_EQ(3, 3);
  SND_CHECK_EQ(3u, 3);  // mixed signedness compares by value
  SND_CHECK_NE(-1, 0xFFFFFFFFu);
  SND_CHECK_NEAR(0.1 + 0.2, 0.3, 1e-12);
  SND_CHECK(!snd::test::CurrentTestFailed());
}

SND_TEST(Runner, FailuresAreCapturedByExpectScope) {
  snd::test::ExpectFailureScope expect;
  SND_CHECK(false);
  SND_CHECK_EQ(1, 2);
  SND_CHECK_NEAR(1.0, 2.0, 0.5);
  if (expect.failures() != 3u) snd::test::ReportFailure(__FILE__, __LINE__, "failures == 3", nullptr);
  // The last message carries both operands.
  if (std::strstr(expect.lastMessage(), "lhs") == nullptr) {
    snd::test::ReportFailure(__FILE__, __LINE__, "message has operands", expect.lastMessage());
  }
}

namespace {
int gRequireReached = 0;
void RequireHelper() {
  SND_REQUIRE(false);
  gRequireReached = 1;  // must not be reached
}
}  // namespace

SND_TEST(Runner, RequireStopsTheFunction) {
  {
    snd::test::ExpectFailureScope expect;
    RequireHelper();
    SND_CHECK_EQ(expect.failures(), 1u);
  }
  SND_CHECK_EQ(gRequireReached, 0);
}

SND_TEST(Runner, NaNIsNeverNear) {
  snd::test::ExpectFailureScope expect;
  volatile double zero = 0.0;
  SND_CHECK_NEAR(zero / zero, 0.0, 1.0);
  if (expect.failures() != 1u) snd::test::ReportFailure(__FILE__, __LINE__, "NaN fails", nullptr);
}

SND_TEST(Runner, ReportsCurrentTestName) {
  SND_CHECK(std::strcmp(snd::test::CurrentTestName(), "ReportsCurrentTestName") == 0);
}
