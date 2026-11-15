// Replacement global operator new/delete for test executables: reports heap
// use inside an RT scope (design.md 14). Linked via sonus_testkit_rthooks.
#include <cstddef>
#include <cstdlib>
#include <new>

#include "snd/internal/rt_check.hpp"

extern "C" void snd_test_rt_mark_hooks(int libc, int newDelete);

// When the libc hooks are active (glibc, no sanitizer) allocate through glibc's
// internal entry points so the malloc hook does not count the same allocation
// twice. Otherwise malloc is not interposed and std::malloc is used.
#if defined(__GLIBC__) && !defined(SND_SANITIZER_ACTIVE)
extern "C" void* __libc_malloc(std::size_t) noexcept;
extern "C" void __libc_free(void*) noexcept;
extern "C" void* __libc_memalign(std::size_t, std::size_t) noexcept;
#define SND_RAW_MALLOC(n) __libc_malloc(n)
#define SND_RAW_FREE(p) __libc_free(p)
#define SND_RAW_MEMALIGN(a, n) __libc_memalign((a), (n))
#else
#define SND_RAW_MALLOC(n) std::malloc(n)
#define SND_RAW_FREE(p) std::free(p)
#define SND_RAW_MEMALIGN(a, n) std::aligned_alloc((a), (n))
#endif

namespace {

struct MarkNewHooks {
  MarkNewHooks() noexcept { snd_test_rt_mark_hooks(0, 1); }
};
const MarkNewHooks gMark;

void* Allocate(std::size_t n, const char* site) noexcept {
  snd::rt::Report(snd::rt::Violation::Alloc, site);
  return SND_RAW_MALLOC(n == 0 ? 1 : n);
}

void* AllocateAligned(std::size_t n, std::size_t align, const char* site) noexcept {
  snd::rt::Report(snd::rt::Violation::Alloc, site);
  if (align < sizeof(void*)) align = sizeof(void*);
  const std::size_t rounded = ((n == 0 ? 1 : n) + align - 1) / align * align;
  return SND_RAW_MEMALIGN(align, rounded);
}

void Release(void* p, const char* site) noexcept {
  if (p == nullptr) return;
  snd::rt::Report(snd::rt::Violation::Free, site);
  SND_RAW_FREE(p);
}

}  // namespace

// Without exceptions an allocation failure terminates (there is no bad_alloc to throw).
void* operator new(std::size_t n) {
  void* p = Allocate(n, "operator new");
  if (p == nullptr) std::abort();
  return p;
}
void* operator new[](std::size_t n) {
  void* p = Allocate(n, "operator new[]");
  if (p == nullptr) std::abort();
  return p;
}
void* operator new(std::size_t n, const std::nothrow_t&) noexcept { return Allocate(n, "operator new"); }
void* operator new[](std::size_t n, const std::nothrow_t&) noexcept {
  return Allocate(n, "operator new[]");
}
void* operator new(std::size_t n, std::align_val_t a) {
  void* p = AllocateAligned(n, static_cast<std::size_t>(a), "operator new(align)");
  if (p == nullptr) std::abort();
  return p;
}
void* operator new[](std::size_t n, std::align_val_t a) {
  void* p = AllocateAligned(n, static_cast<std::size_t>(a), "operator new[](align)");
  if (p == nullptr) std::abort();
  return p;
}
void* operator new(std::size_t n, std::align_val_t a, const std::nothrow_t&) noexcept {
  return AllocateAligned(n, static_cast<std::size_t>(a), "operator new(align)");
}
void* operator new[](std::size_t n, std::align_val_t a, const std::nothrow_t&) noexcept {
  return AllocateAligned(n, static_cast<std::size_t>(a), "operator new[](align)");
}

void operator delete(void* p) noexcept { Release(p, "operator delete"); }
void operator delete[](void* p) noexcept { Release(p, "operator delete[]"); }
void operator delete(void* p, std::size_t) noexcept { Release(p, "operator delete"); }
void operator delete[](void* p, std::size_t) noexcept { Release(p, "operator delete[]"); }
void operator delete(void* p, const std::nothrow_t&) noexcept { Release(p, "operator delete"); }
void operator delete[](void* p, const std::nothrow_t&) noexcept { Release(p, "operator delete[]"); }
void operator delete(void* p, std::align_val_t) noexcept { Release(p, "operator delete(align)"); }
void operator delete[](void* p, std::align_val_t) noexcept { Release(p, "operator delete[](align)"); }
void operator delete(void* p, std::size_t, std::align_val_t) noexcept {
  Release(p, "operator delete(align)");
}
void operator delete[](void* p, std::size_t, std::align_val_t) noexcept {
  Release(p, "operator delete[](align)");
}
void operator delete(void* p, std::align_val_t, const std::nothrow_t&) noexcept {
  Release(p, "operator delete(align)");
}
void operator delete[](void* p, std::align_val_t, const std::nothrow_t&) noexcept {
  Release(p, "operator delete[](align)");
}
