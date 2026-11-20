// Analysis of the mix (design 4.6 "ラウドネス計測", 7 "解析"; stage 3 group 4, D-147, D-148).
//
// Meters: meter 0 is the output (what snd_get_meter(SND_ID_NONE) reports), meters
// 1..maxMeteredBuses go to the buses whose BUSS.flags has kBusMeter, in the order Render
// first sees them (a bus without a free meter has none). Peak and RMS stay per bus for
// every bus (bus_graph.cpp); a meter adds:
//   - loudness (BS.1770-4 / R128, dsp LoudnessMeter*): short-term and integrated,
//     published with the bus's BusSnapshot. Buses without a meter publish NaN ("not
//     measured"; measured silence is dsp::kLoudnessSilence = -200 LUFS);
//   - the last fftSize frames of every channel (ring), for the spectrum.
// Spectrum: Render only keeps the rings. Publish copies the rings of every meter into a
// core-owned triple buffer while a reader asked within the last ~1 s (readers renew the
// interest), so the audio thread never runs an FFT and copies nothing when no one reads.
// Update swaps the buffer with the other snapshots. snd_get_spectrum (any thread) copies
// one channel of the front frame under a seqlock (the protocol of notify.hpp's triple
// buffer) and runs the FFT (dsp Spectrum*) on the reader's thread in scratch memory
// allocated at Create, serialised by a reader mutex (never taken by the audio thread).
// The first read after a period without readers finds no frame yet: SND_ERR_NOT_READY
// until the next Update.
#ifndef SND_CORE_SRC_ANALYSIS_HPP
#define SND_CORE_SRC_ANALYSIS_HPP

#include <atomic>
#include <cstdint>

#include "core_impl.hpp"
#include "snd/base/mutex.hpp"

namespace snd::core {

struct MeterSlot {
  SlotIndex bus = kNoSlot;      // meter 0: the output; others: the bus slot, kNoSlot = free
  std::uint32_t busGen = 0;
  Id busId = 0;
  std::uint32_t channels = 0;   // channels measured (<= AnalysisShared::ringChannels)
  dsp::LoudnessMeterState* loudness = nullptr;
  float* ring = nullptr;        // [ringChannels * fftSize], channel-major
  std::uint32_t write = 0;      // next ring position
  std::uint32_t filled = 0;     // frames written (saturates at fftSize)
};

// Published per meter (16 bytes, then ringChannels * fftSize floats).
struct MeterHeader {
  Id busId;
  std::uint32_t channels;  // 0 = no meter in this slot
  std::uint32_t write;     // oldest frame of the ring
  std::uint32_t filled;
};
static_assert(sizeof(MeterHeader) == 16);

struct AnalysisFrame {
  std::atomic<std::uint32_t> seq{0};       // odd while Publish writes
  std::atomic<std::uint32_t> frontSeq{0};  // seq when Update made it the front
  std::uint8_t* data = nullptr;            // [meterCount * meterBytes]
};

struct AnalysisShared {
  std::uint32_t fftSize = 0;
  std::uint32_t ringChannels = 0;
  std::uint32_t meterCount = 0;
  std::size_t meterBytes = 0;  // sizeof(MeterHeader) + ringChannels * fftSize * 4
  AnalysisFrame frames[3];
  std::atomic<std::uint32_t> middle{1u};  // bits 0-1 index, bit 2 dirty
  std::uint32_t back = 0;                 // Publish
  std::uint32_t front = 2;                // Update
  std::atomic<std::uint32_t> frontPublished{2u};
  std::atomic<std::uint32_t> wanted{0};  // set by readers; Publish takes it (interest renewed)
  std::uint32_t interestLeft = 0;        // Publish: blocks to keep publishing the rings
  std::uint32_t deadPublished = 1;       // Publish: the frame without data is out
  // Reader side (non-RT, serialised by readerMutex).
  base::Mutex readerMutex;
  void* spectrum = nullptr;  // dsp spectrum state (fftSize)
  float* raw = nullptr;      // [fftSize] one channel as published (ring order)
  float* samples = nullptr;  // [fftSize] one channel, oldest first
  float* magnitudes = nullptr;  // [fftSize / 2 + 1]
};

inline constexpr std::uint32_t kAnalysisDirty = 4u;
inline constexpr std::uint32_t kSpectrumInterestBlocks = 256;  // ~1.4 s at 48 kHz, 256 frames

}  // namespace snd::core

#endif  // SND_CORE_SRC_ANALYSIS_HPP
