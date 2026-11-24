#ifndef SND_SOURCE_WAVEFORM_HPP
#define SND_SOURCE_WAVEFORM_HPP
#include <algorithm>
#include <cmath>
#include <cstdint>
#include "snd/format/bank_format.hpp"
namespace snd::source::detail {
// Band-limited step residual (2-point PolyBLEP) for a unit upward step at phase 0:
// t = phase after the step in cycles, dt = cycles per sample.
inline double StepResidual(double t, double dt) noexcept {
  if (t < dt) {
    const double x = t / dt;  // [0, 1)
    return -0.5 * (1.0 - x) * (1.0 - x);
  }
  if (t > 1.0 - dt) {
    const double x = (t - 1.0) / dt;  // (-1, 0)
    return 0.5 * (1.0 + x) * (1.0 + x);
  }
  return 0.0;
}

// Band-limited ramp residual (2-point PolyBLAMP, the integral of StepResidual) for a
// unit change of slope (per sample) at phase 0.
inline double RampResidual(double t, double dt) noexcept {
  double x;
  if (t < dt) {
    x = t / dt;
  } else if (t > 1.0 - dt) {
    x = (1.0 - t) / dt;
  } else {
    return 0.0;
  }
  const double y = 1.0 - x;
  return y * y * y * (1.0 / 6.0);
}

inline double Waveform(std::uint32_t shape, double ph, double dt, double duty) noexcept {
  using W = bank_format::Waveform;
  switch (static_cast<W>(shape)) {
    case W::Sine: return std::sin(6.283185307179586476925 * ph);
    case W::Saw: return 2.0 * ph - 1.0 - 2.0 * StepResidual(ph, dt);
    case W::Triangle: {
      const double other = ph < 0.5 ? ph + 0.5 : ph - 0.5;
      double y = ph < 0.5 ? 4.0 * ph - 1.0 : 3.0 - 4.0 * ph;
      y += 8.0 * dt * RampResidual(ph, dt);
      return y - 8.0 * dt * RampResidual(other, dt);
    }
    default: {
      const bool pulse = shape == static_cast<std::uint32_t>(W::Pulse);
      const double d = pulse ? std::clamp(duty, 0.001, 0.999) : 0.5;
      double edge = ph - d;
      edge -= std::floor(edge);
      double y = ph < d ? 1.0 : -1.0;
      y += 2.0 * StepResidual(ph, dt);
      y -= 2.0 * StepResidual(edge, dt);
      return pulse ? y - (2.0 * d - 1.0) : y;
    }
  }
}
}  // namespace snd::source::detail
#endif
