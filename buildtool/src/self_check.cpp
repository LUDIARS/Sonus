// Self-check of written banks with the runtime's validation rules: the
// contract's shared checker snd/format/bank_check.hpp (contract 1.1, A1-13).
// buildtool does not depend on the bank module (CONTRACTS.md D-02); the rules
// live in the contract so that ParseBank and this check are the same code.
#include "self_check.hpp"

#include <cstring>

#include "snd/format/bank_check.hpp"

namespace snd::buildtool::detail {

bool SelfCheckBank(const std::vector<std::uint8_t>& bank, std::string* why) {
  namespace bf = ::snd::bank_format;
  // The checker reads typed columns in place: give it a 64-byte aligned copy.
  std::vector<std::uint64_t> storage((bank.size() + 7) / 8 + 8);
  auto* base = reinterpret_cast<std::uint8_t*>(storage.data());
  while (reinterpret_cast<std::uintptr_t>(base) % 64 != 0) ++base;
  if (!bank.empty()) std::memcpy(base, bank.data(), bank.size());
  bf::FileHeader h{};
  if (bank.size() >= sizeof(h)) std::memcpy(&h, bank.data(), sizeof(h));
  bf::check::CheckOptions opt;
  opt.platform = h.platform;  // the build's own platform tag
  bf::check::CheckError err;
  // CheckedBank is large (one table view per known chunk): keep it off the stack.
  std::vector<bf::check::CheckedBank> out(1);
  if (bf::check::CheckBank(base, bank.size(), opt, out.data(), &err) == SND_OK) return true;
  if (why != nullptr) *why = err.message;
  return false;
}

}  // namespace snd::buildtool::detail
