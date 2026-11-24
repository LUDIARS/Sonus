// Convolution reverb: uniformly partitioned FFT convolution with a direct-form
// head (design.md 3.3 "DSPインスタンスプール", 7 "エフェクト"; CONTRACTS.md D-211..D-215).
//
// Two parts:
//   1. Prepared impulse responses (IR): built on a normal worker (not RT) into
//      caller memory, immutable afterwards, shared by any number of instances.
//      They hold the IR transformed to the frequency domain per partition, so
//      no instance repeats that work.
//   2. The standard effect `snd.fx.convolution` (EffectKind::Convolution): a
//      DSP-pool instance like every other standard effect (EffectStateBytes /
//      EffectInit / EffectProcess / EffectProcessBatch, no virtual calls). It
//      finds its IR through a process-wide registry of prepared IRs by the
//      parameter IrSlot.
//
// Algorithm (D-211): partition size B (power of two), FFT size 2B (fft.hpp),
// overlap-save with a frequency-domain delay line (FDL) of the input spectra.
//   - ZeroLatency = 1 (default): the first B taps of the IR run as a direct FIR
//     (vectorised, per sample), partitions 1.. run through the FDL. Latency 0.
//   - ZeroLatency = 0: every partition runs through the FDL; the effect's output
//     (wet AND dry) is delayed by B frames. EffectLatencyFrames() = B.
// The FFT steps happen at fixed input positions (every B frames since Init), so
// the output is bit-identical however a block is split into calls, and the cost
// per B frames is constant (no spikes from larger partitions).
//
// Parameters (EFPV / ParamBlock order):
//   0 IrSlot          0..1024 (int)   0     registry slot of the prepared IR; 0 = none (wet silent)
//   1 DryMix          0..1            0     dry gain (linear)
//   2 WetMix          0..1            1     wet gain (linear)
//   3 WetGainDb       -48..24         0     extra wet gain in dB (IR level compensation)
//   4 PartitionFrames 0..4096 (int)   0     [init] B; 0 = auto: max_frames rounded up to a
//                                            power of two, clamped to 64..1024; other values are
//                                            rounded up to a power of two, clamped to 64..4096
//   5 ZeroLatency     0..1 (int)      1     [init] see above
//   6 MaxIrMs         10..10000      2000   [init] longest IR the instance can hold, in ms at the
//                                            runtime rate (sizes the FDL; longer IRs are cut)
// Dry/wet: out = DryMix * dry + WetMix * 10^(WetGainDb/20) * wet, ramped per frame
// (the gains are derived per frame from the interpolated parameters).
// IrSlot is not ramped: a call uses the slot of its END value. Changing IrSlot
// swaps the IR at the call boundary; the input history (FDL) is kept, so the
// new IR applies to the recent input at once (no fade; crossfade by running two
// instances if needed).
//
// Channel routing (prepared IR with I channels, instance with C channels):
//   - I == C:                 per channel, output c = input c * IR c (stereo IR on stereo)
//   - I == C * C, C >= 2:     matrix ("true stereo" for C = 2): output o = sum_i input i *
//                              IR (i * C + o); for stereo the IR order is LL, LR, RL, RR
//   - otherwise:              output c = input c * IR (c mod I) (mono IR on every channel)
// A call with fewer channels than the instance treats the missing inputs as silence.
//
// The IR is usable by an instance only if its partition size equals the
// instance's B and its sample rate equals the instance's runtime rate;
// otherwise the instance treats it as absent (wet silent). Use
// ConvolutionPartitionFrames(desc) to prepare with the right B.
// IR frames beyond the instance capacity (MaxIrMs) are ignored.
//
// State (DSP pool): about 8 bytes per input channel per frame of capacity
// (FDL) + O(C B); see EffectStateBytes. The state never holds a pointer to
// the IR; the registry is read at the start of every call.
//
// Tail: done once the input has been silent (< -100 dBFS) for capacity + 2B
// frames (+ B with ZeroLatency 0). Then the delay line is invalidated without
// clearing it (O(1)), silent input costs nothing and gives exact zeros.
//
// Real time: EffectProcess, the registry reads and ConvolutionRegisteredIr are
// RT safe. Preparing, registering and unregistering are for normal workers.
#ifndef SND_DSP_CONVOLUTION_HPP
#define SND_DSP_CONVOLUTION_HPP

#include <cstddef>
#include <cstdint>

#include "snd/dsp/effects.hpp"

namespace snd::dsp {

// Parameter indices of snd.fx.convolution.
enum ConvolutionParam : std::uint32_t {
  kConvIrSlot = 0,
  kConvDryMix = 1,
  kConvWetMix = 2,
  kConvWetGainDb = 3,
  kConvPartitionFrames = 4,
  kConvZeroLatency = 5,
  kConvMaxIrMs = 6,
  kConvParamCount = 7,
};

inline constexpr std::uint32_t kConvolutionMaxIrSlots = 1024;
// Largest registry number (D-219): 24 bits, exact in a float parameter.
inline constexpr std::uint32_t kConvolutionMaxIrNumber = (1u << 24) - 1u;
inline constexpr std::uint32_t kConvolutionMaxIrChannels = 16;
inline constexpr std::uint32_t kConvolutionMinPartition = 64;
inline constexpr std::uint32_t kConvolutionMaxPartition = 4096;
inline constexpr std::uint32_t kConvolutionMaxIrFrames = 1u << 21;  // prepared IR, at the target rate
inline constexpr std::uint32_t kConvolutionMaxRate = 384000;

struct ConvolutionIr;  // opaque, lives in caller memory

// IR samples to prepare: `channels[c][0..frames)` float32. sampleRate 0 means the
// target rate. The IR is resampled to the target rate (Kaiser-windowed sinc,
// 64 zero crossings) when the rates differ and multiplied by `gain`.
struct ConvolutionIrDesc {
  const float* const* channels;
  std::uint32_t channelCount;  // 1..kConvolutionMaxIrChannels
  std::uint32_t frames;        // 1.. ; at the target rate <= kConvolutionMaxIrFrames
  std::uint32_t sampleRate;
  float gain;
};

// Frames of the IR at `targetRate` (0 if invalid).
std::uint32_t ConvolutionIrTargetFrames(std::uint32_t frames, std::uint32_t sampleRate,
                                        std::uint32_t targetRate) noexcept;
// Bytes of the prepared IR (multiple of 64), 0 if the arguments are invalid
// (partition not a power of two in [64, 4096], rate 0 or > kConvolutionMaxRate, ...).
std::size_t ConvolutionIrBytes(const ConvolutionIrDesc& desc, std::uint32_t targetRate,
                               std::uint32_t partitionFrames) noexcept;
// Prepares into `mem` (64-byte aligned, >= ConvolutionIrBytes). Not RT (FFTs,
// resampling); no allocation. Returns `mem` as the IR, or nullptr on failure.
const ConvolutionIr* ConvolutionPrepareIr(void* mem, std::size_t bytes,
                                          const ConvolutionIrDesc& desc, std::uint32_t targetRate,
                                          std::uint32_t partitionFrames) noexcept;

// --- IR blob (bank EFFX blob of a convolution effect, D-213) ---
// Little endian, any alignment:
//   u32 magic 'SCIR' (0x52494353) | u32 version (1) | u32 channels (1..16) | u32 frames (>= 1)
//   u32 sampleRate (1..384000)     | f32 gain       | u32 reserved[2] (0)
//   f32 samples[channels][frames]   (channel after channel)
// Total bytes = 32 + 4 * channels * frames exactly.
inline constexpr std::uint32_t kConvolutionBlobMagic = 0x52494353u;  // "SCIR"
inline constexpr std::uint32_t kConvolutionBlobVersion = 1;
inline constexpr std::uint32_t kConvolutionBlobHeaderBytes = 32;
struct ConvolutionBlobInfo {
  std::uint32_t channels;
  std::uint32_t frames;
  std::uint32_t sampleRate;
  float gain;
  const unsigned char* samples;  // little-endian f32, possibly unaligned
};
// Validates the header and the size. Pure.
bool ConvolutionParseIrBlob(const void* blob, std::uint64_t blobBytes,
                            ConvolutionBlobInfo* out) noexcept;
std::size_t ConvolutionIrBytesFromBlob(const void* blob, std::uint64_t blobBytes,
                                       std::uint32_t targetRate,
                                       std::uint32_t partitionFrames) noexcept;
const ConvolutionIr* ConvolutionPrepareIrFromBlob(void* mem, std::size_t bytes, const void* blob,
                                                  std::uint64_t blobBytes,
                                                  std::uint32_t targetRate,
                                                  std::uint32_t partitionFrames) noexcept;
// Writes a blob (tests, tools): bytes needed = 32 + 4 * channels * frames.
std::size_t ConvolutionWriteIrBlob(void* dst, std::size_t bytes, const float* const* channels,
                                   std::uint32_t channelCount, std::uint32_t frames,
                                   std::uint32_t sampleRate, float gain) noexcept;

// Prepared IR queries (nullptr / invalid -> 0).
std::uint32_t ConvolutionIrChannels(const ConvolutionIr* ir) noexcept;
std::uint32_t ConvolutionIrFrames(const ConvolutionIr* ir) noexcept;  // at the target rate
std::uint32_t ConvolutionIrPartition(const ConvolutionIr* ir) noexcept;
std::uint32_t ConvolutionIrSampleRate(const ConvolutionIr* ir) noexcept;

// --- Registry (process-wide, fixed capacity, lock-free) ---
// Register: normal worker. Returns the slot 1..kConvolutionMaxIrSlots (the
// lowest free one), 0 if the registry is full or `ir` is not a prepared IR.
std::uint32_t ConvolutionRegisterIr(const ConvolutionIr* ir) noexcept;
// Unregister: normal worker. The caller guarantees that no EffectProcess of an
// instance whose IrSlot is `slot` runs concurrently or later while the memory
// is being freed or reused (core: after the bank's epoch, D-213). Returns false
// if the slot was empty / out of range.
bool ConvolutionUnregisterIr(std::uint32_t slot) noexcept;
// RT safe (acquire load).
const ConvolutionIr* ConvolutionRegisteredIr(std::uint32_t slot) noexcept;

// --- Instance helpers ---
// Partition size an instance made from `desc` uses (pure; 0 if desc is not a
// convolution effect or invalid).
std::uint32_t ConvolutionPartitionFrames(const EffectDesc& desc) noexcept;
// Of an initialised instance.
std::uint32_t ConvolutionStatePartition(const void* state) noexcept;
std::uint32_t ConvolutionStateCapacityFrames(const void* state) noexcept;
// IR frames the instance used in its last call (0 = no usable IR).
std::uint32_t ConvolutionStateActiveIrFrames(const void* state) noexcept;

}  // namespace snd::dsp

#endif  // SND_DSP_CONVOLUTION_HPP
