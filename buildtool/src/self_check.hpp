// Self-check of written banks (principle: a bank the build accepts always
// passes the runtime's ParseBank). The rules are a generated copy of
// bank/src/validate.cpp (buildtool must not depend on bank, CONTRACTS.md D-02);
// regenerate self_check.cpp with tools: see the header comment there.
#ifndef SND_BUILDTOOL_SRC_SELF_CHECK_HPP
#define SND_BUILDTOOL_SRC_SELF_CHECK_HPP

#include <cstdint>
#include <string>
#include <vector>

namespace snd::buildtool::detail {

// bank-format.md §8 on a complete bank image. false and the reason otherwise.
bool SelfCheckBank(const std::vector<std::uint8_t>& bank, std::string* why);

}  // namespace snd::buildtool::detail

#endif  // SND_BUILDTOOL_SRC_SELF_CHECK_HPP
