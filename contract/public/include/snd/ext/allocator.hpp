// IAllocator (design.md 12.3). FROZEN CONTRACT.
#ifndef SND_EXT_ALLOCATOR_HPP
#define SND_EXT_ALLOCATOR_HPP

#include <cstddef>

#include "snd/ext.h"
#include "snd/ext/types.hpp"

namespace snd {

// Called at initialisation and from normal workers (bank loading) only, never
// from the audio thread or RT workers. Must be thread-safe.
struct IAllocator {
  virtual ~IAllocator() = default;
  // bytes > 0, align is a power of two >= 16. Returns nullptr on failure.
  virtual void* Allocate(std::size_t bytes, std::size_t align, MemTag tag) = 0;
  // p may be nullptr; tag equals the allocation's tag.
  virtual void Free(void* p, MemTag tag) = 0;
};

namespace bridge {
inline void* SND_CALL AllocatorAllocate(void* u, std::size_t b, std::size_t a, snd_mem_tag t) {
  return static_cast<IAllocator*>(u)->Allocate(b, a, static_cast<MemTag>(t));
}
inline void SND_CALL AllocatorFree(void* u, void* p, snd_mem_tag t) {
  static_cast<IAllocator*>(u)->Free(p, static_cast<MemTag>(t));
}
}  // namespace bridge

// C++ object -> C table (used by the C++ API wrapper).
inline snd_allocator ToC(IAllocator& a) noexcept {
  return snd_allocator{&a, &bridge::AllocatorAllocate, &bridge::AllocatorFree};
}

// C table -> C++ interface (used inside the library for tables given through the C API).
class CAllocator final : public IAllocator {
 public:
  explicit CAllocator(const snd_allocator& c) noexcept : c_(c) {}
  void* Allocate(std::size_t bytes, std::size_t align, MemTag tag) override {
    return c_.allocate(c_.user, bytes, align, static_cast<snd_mem_tag>(tag));
  }
  void Free(void* p, MemTag tag) override { c_.deallocate(c_.user, p, static_cast<snd_mem_tag>(tag)); }
  const snd_allocator& table() const noexcept { return c_; }

 private:
  snd_allocator c_;
};

}  // namespace snd

#endif  // SND_EXT_ALLOCATOR_HPP
