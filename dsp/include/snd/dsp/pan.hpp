// Panning, spread and downmix (design.md 4.5, 4.6, 2.6).
//
// Speaker layouts and channel order (WAVE / SMPTE order):
//   Mono        C
//   Stereo      L R
//   Quad        L R BL BR
//   Surround51  L R C LFE SL SR
//   Surround71  L R C LFE BL BR SL SR
// Azimuth: radians, 0 = front, positive = to the right (clockwise seen from
// above), +-pi = behind. Speaker azimuths:
//   Stereo, 5.1, 7.1: L/R +-30 deg, C 0;  Quad: L/R +-45 deg, BL/BR +-135 deg
//   5.1: SL/SR +-110 deg;  7.1: SL/SR +-90 deg, BL/BR +-150 deg.
//
// All gains computed here are energy preserving for a point source (sum of
// squared gains = 1) unless stated otherwise. Matrices are snd::PanMatrix
// (RealVoice.panMatrix): gain[input channel][output channel].
#ifndef SND_DSP_PAN_HPP
#define SND_DSP_PAN_HPP

#include <cstdint>

#include "snd/dsp/kernels.hpp"
#include "snd/internal/common.hpp"

namespace snd::dsp {

enum class SpeakerLayout : std::uint32_t {
  Mono = 0,
  Stereo = 1,
  Quad = 2,
  Surround51 = 3,
  Surround71 = 4,
};
std::uint32_t LayoutChannels(SpeakerLayout layout) noexcept;
// 1 -> Mono, 2 -> Stereo, 4 -> Quad, 6 -> 5.1, 8 -> 7.1; other counts -> false.
bool LayoutForChannels(std::uint32_t channels, SpeakerLayout* out) noexcept;
// LFE channel index of the layout, or -1.
int LayoutLfeChannel(SpeakerLayout layout) noexcept;

enum class PanLaw : std::uint32_t {
  ConstantPower = 0,  // sin/cos, -3 dB at centre (energy preserving)
  Linear = 1,         // -6 dB at centre (amplitude preserving)
  Compromise = 2,     // -4.5 dB at centre (sqrt of the product of the two)
};

// pan in [-1 (left), 1 (right)].
void StereoPanGains(float pan, PanLaw law, float* left, float* right) noexcept;

// Gains of a point source at `azimuth` spread over an arc of spread * 360
// degrees (spread in [0, 1]; 0 = point, 1 = all around), for every channel of
// `layout`. LFE gets 0. Pairwise constant-power panning between adjacent
// speakers (stereo: pan = sin(azimuth), so rear sources fold to the front).
void ComputePanGains(SpeakerLayout layout, float azimuth, float spread, float* gains) noexcept;

// 3D pan matrix for a voice with `inChannels` channels: every input channel is
// placed at azimuth + offset, offsets spread evenly over spread * 90 degrees
// (multi-channel sources widen with spread), then panned with ComputePanGains;
// rows are scaled by 1 / sqrt(positioned channels). The LFE channel of a 6 / 8
// channel source is not positioned: it goes to the LFE of `out` (gain 1) if
// `out` has one, otherwise it is dropped.
// Rows >= inChannels and columns >= LayoutChannels(out) are zero.
void ComputePanMatrix(std::uint32_t inChannels, SpeakerLayout out, float azimuth, float spread,
                      PanMatrix* m) noexcept;

// 2D pan matrix: the input layout (by channel count) is downmixed to `out`,
// then balanced with `pan` in [-1, 1] (left/right channels of `out` only).
// pan = 0 is the plain downmix.
void ComputeStereoPanMatrix(std::uint32_t inChannels, SpeakerLayout out, float pan, PanLaw law,
                            PanMatrix* m) noexcept;

// Downmix / upmix matrix between layouts (ITU-R BS.775 style):
//   5.1 -> stereo: L' = L + 0.7071 C + 0.7071 SL, R' = R + 0.7071 C + 0.7071 SR
//   7.1 -> 5.1: SL' = 0.7071 (SL + BL), SR' = 0.7071 (SR + BR)
//   quad -> stereo: L' = L + 0.7071 BL ...
//   any -> mono: 0.7071 L + 0.7071 R + C; surrounds 0.5 each for quad and 5.1,
//            0.3536 each for the 7.1 sides and rears (7.1 goes through 5.1: same power)
//   mono -> stereo: 0.7071 each; mono -> 5.1/7.1: C; stereo -> 5.1/7.1: L, R only
// LFE: passed through when `to` has an LFE (5.1 and 7.1 both ways); otherwise
// dropped, unless includeLfe (then mixed at 0.7071 into L and R, or into mono).
// Same layout -> identity.
void ComputeDownmixMatrix(SpeakerLayout from, SpeakerLayout to, PanMatrix* m,
                          bool includeLfe = false) noexcept;

void ClearPanMatrix(PanMatrix* m) noexcept;
void ScalePanMatrix(PanMatrix* m, float gain) noexcept;
// out = a + (b - a) * t (2D/3D blend, 4.5).
void LerpPanMatrix(const PanMatrix& a, const PanMatrix& b, float t, PanMatrix* out) noexcept;
// out = a * b (apply a, then b): out[i][k] = sum_j a[i][j] * b[j][k].
void MultiplyPanMatrix(const PanMatrix& a, const PanMatrix& b, PanMatrix* out) noexcept;

// out[o] (+)= sum_i in[i] * g_io(t), every gain ramping linearly from `from` to
// `to` across the block (D-26 convention). Pairs whose gain is 0 at both ends
// are skipped. accumulate = false overwrites out[0..outChannels).
void ApplyPanMatrix(const float* const* in, std::uint32_t inChannels, float* const* out,
                    std::uint32_t outChannels, std::uint32_t frames, const PanMatrix& from,
                    const PanMatrix& to, bool accumulate = true) noexcept;

}  // namespace snd::dsp

#endif  // SND_DSP_PAN_HPP
