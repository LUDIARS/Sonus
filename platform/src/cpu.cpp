#include "snd/platform/cpu.hpp"

#include "snd/platform/thread.hpp"

#if defined(__x86_64__) || defined(__i386__)
#include <cpuid.h>
#include <xmmintrin.h>
#define SND_PLATFORM_X86 1
#elif defined(__aarch64__)
#define SND_PLATFORM_ARM64 1
#endif

namespace snd::platform {

namespace {

#if defined(SND_PLATFORM_X86)
std::uint64_t ReadXcr0() noexcept {
  std::uint32_t lo = 0, hi = 0;
  __asm__ __volatile__("xgetbv" : "=a"(lo), "=d"(hi) : "c"(0));
  return (static_cast<std::uint64_t>(hi) << 32) | lo;
}
#endif

CpuFeatures Detect() noexcept {
  CpuFeatures f;
  f.logicalCores = HardwareConcurrency();
#if defined(SND_PLATFORM_X86)
  unsigned a = 0, b = 0, c = 0, d = 0;
  const unsigned maxLeaf = __get_cpuid_max(0, nullptr);
  if (maxLeaf >= 1u && __get_cpuid(1, &a, &b, &c, &d)) {
    f.sse2 = (d & bit_SSE2) != 0u;
    f.sse3 = (c & bit_SSE3) != 0u;
    f.ssse3 = (c & bit_SSSE3) != 0u;
    f.sse41 = (c & bit_SSE4_1) != 0u;
    f.sse42 = (c & bit_SSE4_2) != 0u;
    const bool osxsave = (c & bit_OSXSAVE) != 0u;
    const std::uint64_t xcr0 = osxsave ? ReadXcr0() : 0u;
    const bool ymm = (xcr0 & 0x6u) == 0x6u;      // XMM + YMM state
    const bool zmm = (xcr0 & 0xE6u) == 0xE6u;    // + opmask, ZMM_Hi256, Hi16_ZMM
    f.avx = ymm && (c & bit_AVX) != 0u;
    f.fma = f.avx && (c & bit_FMA) != 0u;
    f.f16c = f.avx && (c & bit_F16C) != 0u;
    if (maxLeaf >= 7u && __get_cpuid_count(7, 0, &a, &b, &c, &d)) {
      f.avx2 = f.avx && (b & bit_AVX2) != 0u;
      f.avx512f = zmm && (b & bit_AVX512F) != 0u;
    }
    f.denormalControl = f.sse2;
  }
#elif defined(SND_PLATFORM_ARM64)
  f.neon = true;
  f.denormalControl = true;
#endif
  return f;
}

#if defined(SND_PLATFORM_X86)
constexpr unsigned kFtz = 0x8000u;  // MXCSR.FZ
constexpr unsigned kDaz = 0x0040u;  // MXCSR.DAZ
#elif defined(SND_PLATFORM_ARM64)
constexpr std::uint64_t kFz = std::uint64_t{1} << 24;  // FPCR.FZ
std::uint64_t ReadFpcr() noexcept {
  std::uint64_t v;
  __asm__ __volatile__("mrs %0, fpcr" : "=r"(v));
  return v;
}
void WriteFpcr(std::uint64_t v) noexcept { __asm__ __volatile__("msr fpcr, %0" : : "r"(v)); }
#endif

}  // namespace

const CpuFeatures& GetCpuFeatures() noexcept {
  static const CpuFeatures features = Detect();
  return features;
}

FpControl SetFlushDenormals(bool enable) noexcept {
  FpControl prev;
#if defined(SND_PLATFORM_X86)
  const unsigned csr = _mm_getcsr();
  prev.raw = csr;
  // DAZ exists on every SSE2 CPU this code targets (x86-64 baseline).
  _mm_setcsr(enable ? (csr | kFtz | kDaz) : (csr & ~(kFtz | kDaz)));
#elif defined(SND_PLATFORM_ARM64)
  const std::uint64_t fpcr = ReadFpcr();
  prev.raw = fpcr;
  WriteFpcr(enable ? (fpcr | kFz) : (fpcr & ~kFz));
#else
  (void)enable;
#endif
  return prev;
}

void RestoreFpControl(FpControl state) noexcept {
#if defined(SND_PLATFORM_X86)
  _mm_setcsr(static_cast<unsigned>(state.raw));
#elif defined(SND_PLATFORM_ARM64)
  WriteFpcr(state.raw);
#else
  (void)state;
#endif
}

bool FlushDenormalsEnabled() noexcept {
#if defined(SND_PLATFORM_X86)
  return (_mm_getcsr() & (kFtz | kDaz)) == (kFtz | kDaz);
#elif defined(SND_PLATFORM_ARM64)
  return (ReadFpcr() & kFz) != 0u;
#else
  return false;
#endif
}

}  // namespace snd::platform
