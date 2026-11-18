// Peak hold of the bus meters.
#include "snd/tool/meter_ballistics.hpp"

#include <algorithm>
#include <cmath>

namespace snd::tool {

float LinearToDb(float linear) {
  const float a = std::fabs(linear);
  if (!(a > 0.0f)) return kMeterFloorDb;
  return std::max(kMeterFloorDb, 20.0f * std::log10(a));
}

float PeakHold::HeldDb(std::uint64_t nowNs, const PeakHoldParams& p) const {
  if (nowNs <= setNs_ + p.holdNs) return heldDb_;
  const double fallen = static_cast<double>(nowNs - setNs_ - p.holdNs) * 1e-9 * p.fallDbPerSecond;
  return std::max(kMeterFloorDb, static_cast<float>(heldDb_ - fallen));
}

void PeakHold::Update(float peakLinear, std::uint64_t nowNs, const PeakHoldParams& p) {
  const float db = LinearToDb(peakLinear);
  if (std::fabs(peakLinear) >= 1.0f) clipped_ = true;
  if (db >= HeldDb(nowNs, p)) {
    heldDb_ = db;
    setNs_ = nowNs;
  }
}

void PeakHold::Reset() {
  heldDb_ = kMeterFloorDb;
  setNs_ = 0;
  clipped_ = false;
}

}  // namespace snd::tool
