// CPU feature detection and denormal control (design.md 2.4 デノーマル, 3.4).
//
// SetFlushDenormals / ScopedFlushDenormals: FTZ+DAZ on x86 (MXCSR bits 15
// and 6), FZ on AArch64 (FPCR bit 24). Affects the CALLING thread only; the
// audio thread and every worker set it at thread start (2.4). RT-safe (one
// register read/write), any thread.
//
// GetCpuFeatures: detected once (thread-safe static init), RT-safe after the
// first call. The SIMD dispatch itself lives in the dsp-owned simd headers.
#ifndef SND_PLATFORM_CPU_HPP
#define SND_PLATFORM_CPU_HPP

#include <cstdint>

namespace snd::platform {

struct CpuFeatures {
  // x86 / x86-64 (AVX-family flags also require OS support via XGETBV)
  bool sse2 = false;
  bool sse3 = false;
  bool ssse3 = false;
  bool sse41 = false;
  bool sse42 = false;
  bool avx = false;
  bool avx2 = false;
  bool fma = false;
  bool f16c = false;
  bool avx512f = false;
  // ARM
  bool neon = false;
  std::uint32_t logicalCores = 1;
  bool denormalControl = false;  // SetFlushDenormals has an effect on this CPU
};

const CpuFeatures& GetCpuFeatures() noexcept;

// Opaque floating-point control state of the calling thread.
struct FpControl {
  std::uint64_t raw = 0;
};

// Returns the previous state (pass it to RestoreFpControl).
FpControl SetFlushDenormals(bool enable) noexcept;
void RestoreFpControl(FpControl state) noexcept;
// True if flush-to-zero (and DAZ where present) is active on this thread.
bool FlushDenormalsEnabled() noexcept;

class ScopedFlushDenormals {
 public:
  ScopedFlushDenormals() noexcept : saved_(SetFlushDenormals(true)) {}
  ~ScopedFlushDenormals() { RestoreFpControl(saved_); }
  ScopedFlushDenormals(const ScopedFlushDenormals&) = delete;
  ScopedFlushDenormals& operator=(const ScopedFlushDenormals&) = delete;

 private:
  FpControl saved_;
};

}  // namespace snd::platform

#endif  // SND_PLATFORM_CPU_HPP
