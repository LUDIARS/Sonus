// Peak hold of the bus meters (design.md 11.1 ミキサー).
//
// Rule: a peak at or above the held value replaces it and restarts the hold;
// the held value stays for `holdNs`, then falls at `fallDbPerSecond` until a
// new peak catches it. The clip flag latches when a peak reaches 0 dBFS
// (linear 1.0) and stays until Reset.
#ifndef SND_TOOL_METER_BALLISTICS_HPP
#define SND_TOOL_METER_BALLISTICS_HPP

#include <cstdint>

namespace snd::tool {

inline constexpr float kMeterFloorDb = -120.0f;

float LinearToDb(float linear);  // kMeterFloorDb for 0 / NaN

struct PeakHoldParams {
  std::uint64_t holdNs = 1500000000ull;
  float fallDbPerSecond = 20.0f;
};

class PeakHold {
 public:
  void Update(float peakLinear, std::uint64_t nowNs, const PeakHoldParams& p = PeakHoldParams{});
  float HeldDb(std::uint64_t nowNs, const PeakHoldParams& p = PeakHoldParams{}) const;
  bool clipped() const { return clipped_; }
  void Reset();

 private:
  float heldDb_ = kMeterFloorDb;
  std::uint64_t setNs_ = 0;
  bool clipped_ = false;
};

}  // namespace snd::tool

#endif  // SND_TOOL_METER_BALLISTICS_HPP
