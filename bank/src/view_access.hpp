// Internal access to BankView's private state (validate.cpp, prepared.cpp).
#ifndef SND_BANK_SRC_VIEW_ACCESS_HPP
#define SND_BANK_SRC_VIEW_ACCESS_HPP

#include "snd/bank/bank_view.hpp"

namespace snd::bank {

struct ViewAccess {
  static const std::uint8_t*& File(BankView& v) noexcept { return v.file_; }
  static bf::FileHeader& Header(BankView& v) noexcept { return v.header_; }
  static ChunkRef* Chunks(BankView& v) noexcept { return v.chunks_; }
  static TableView* Tables(BankView& v) noexcept { return v.tables_; }
  static bool& Writable(BankView& v) noexcept { return v.writable_; }
};

}  // namespace snd::bank

#endif  // SND_BANK_SRC_VIEW_ACCESS_HPP
