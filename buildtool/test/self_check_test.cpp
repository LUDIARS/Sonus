// buildtool's self-check of written banks against the same corruption cases as
// the runtime's ParseBank (bank/test/corruption_cases.hpp, BankValidate.SharedCorruptionCases).
#include <string>
#include <vector>

#include "corruption_cases.hpp"
#include "self_check.hpp"
#include "snd/test/test.hpp"

SND_TEST(BuildtoolSelfCheck, AcceptsAValidBank) {
  std::string why;
  SND_CHECK_MSG(snd::buildtool::detail::SelfCheckBank(fixture::RichBank().Build(), &why), "%s", why.c_str());
}

SND_TEST(BuildtoolSelfCheck, RejectsTheSharedCorruptionCases) {
  for (const corruption::Case& k : corruption::Cases()) {
    std::string why;
    const bool ok = snd::buildtool::detail::SelfCheckBank(corruption::Build(k), &why);
    SND_CHECK_MSG(!ok, "%s: accepted by the self-check", k.name);
  }
}
