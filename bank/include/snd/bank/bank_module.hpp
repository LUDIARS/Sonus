// bank module (design.md 12.1): bank loading, validation, registration, release, dependencies.
// Dependencies (frozen, root CMakeLists.txt): base, io.
//
// Headers:
//   snd/bank/bank_view.hpp     typed zero-copy access to a validated bank (columns, blobs, PHYS, ...)
//   snd/bank/validate.hpp      ParseBank: bank-format.md §8 validation
//   snd/bank/prepared.hpp      PreparedBank (what cmd::BankRegister::prepared points to), dev override layer
//   snd/bank/bank_manager.hpp  slots, loading path (IO -> job -> registration), dependencies, unload, languages
#ifndef SND_BANK_BANK_MODULE_HPP
#define SND_BANK_BANK_MODULE_HPP

namespace snd::bank {

// Module identification, used by the smoke test.
const char* ModuleName() noexcept;

}  // namespace snd::bank

#endif  // SND_BANK_BANK_MODULE_HPP
