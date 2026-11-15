// Audio -> game notifications and published snapshots (design.md 2.1).
// FROZEN CONTRACT.
//
// Three channels, all written by the Publish system (audio thread) and
// consumed on the game side:
//
// 1. Lossless flag arrays (never overflow, independent of volume):
//    - EndFlags: per Playback slot. Publish writes EndRecord[slot] and then
//      sets bit `slot` with fetch_or(release). Update() takes each word with
//      exchange(0, acquire), delivers on_end (unless the playback was a
//      one-shot) and returns the slot to the game-side free list. A slot ends
//      at most once per generation, and the game side reissues it only after
//      consuming the bit, so a record is never overwritten before it is read.
//    - ReleaseFlags: the same protocol for Emitter / Transport / UserBuffer
//      slots: the bit means "the audio side destroyed the row; the slot (and,
//      for user buffers, the game memory) may be reused".
//    - BankSlots: per bank slot state + result (atomic), for GetBankState and
//      on_bank_loaded (delivered once on the transition to Loaded/Failed).
// 2. EventRing: fixed-capacity SPSC ring of EventRecord (beats, markers,
//    logs, warnings). When full, new records are dropped and `dropped` is
//    incremented (stats.dropped_events).
// 3. Snapshot triple buffer: per-block state for the query API. Publish
//    fills the back buffer, then exchanges it with the middle index (dirty
//    bit set). Update() exchanges front and middle when dirty, so queries see
//    the state "as of the latest Update()". Readers on any thread use the
//    per-buffer seqlock (even = stable) and copy with relaxed atomic 8-byte
//    loads; every snapshot struct is therefore a multiple of 8 bytes.
//    1.1: writes into a frame go through base::notify::SnapshotStore /
//    SetFrameHeader (relaxed atomic 8-byte stores that pair with the readers'
//    loads; a plain store would be a data race). SnapshotFrame::reserved is
//    used by base: it holds the seq the frame had when it became front, so a
//    reader can tell a frame that was recycled while it copied.
#ifndef SND_INTERNAL_NOTIFY_HPP
#define SND_INTERNAL_NOTIFY_HPP

#include <atomic>
#include <cstdint>

#include "snd/internal/common.hpp"
#include "snd/types.h"

namespace snd::notify {

// --- 1. Flag arrays ---------------------------------------------------------
struct EndRecord {
  std::uint32_t generation;  // generation of the playback that ended
  std::uint16_t reason;      // snd_end_reason
  std::uint16_t detail;      // snd_end_detail
};
static_assert(sizeof(EndRecord) == 8);

struct EndFlags {
  EndRecord* records = nullptr;             // [capacity]
  std::atomic<std::uint64_t>* bits = nullptr;  // [(capacity + 63) / 64]
  std::uint32_t capacity = 0;               // Config.maxPlaybacks
};

struct ReleaseFlags {
  std::atomic<std::uint64_t>* bits = nullptr;
  std::uint32_t* generation = nullptr;  // generation that was released, per slot
  std::uint32_t capacity = 0;
};

struct BankSlotState {
  std::atomic<std::uint32_t> state;    // snd_bank_state
  std::atomic<std::int32_t> result;    // snd_result of the last load
  std::atomic<std::uint32_t> generation;
  std::atomic<std::uint32_t> notified; // set by Update after on_bank_loaded
};

// --- 2. Event ring ------------------------------------------------------------
enum class EventType : std::uint16_t {
  Beat = 1,         // object = transport handle, a = bar, b = beat (tick 0), clock = beat time
  Marker = 2,       // object = playback handle, a = marker id, clock = crossing time
  Log = 3,          // object = (uint64) static format string, a = level, arg0/arg1 = args
  Error = 4,        // a = snd_result, detail = snd_end_detail or 0, object = handle/id
  Warning = 5,      // same layout as Error (e.g. reservation deferred, transport gone)
  StreamStall = 6,  // object = playback handle, a = stream slot
  Underrun = 7,     // a = frames of silence inserted
  LodChange = 8,    // a = new LOD level, b = old level
  ReservationLate = 9,  // object = target handle, a = ReservationLateRule applied, b = deferrals
};

struct EventRecord {
  std::uint16_t type;    // EventType
  std::uint16_t detail;
  std::uint32_t a;
  std::uint64_t object;
  ClockSample clock;
  std::uint64_t arg0;
  std::uint64_t arg1;
  std::uint32_t b;
  std::uint32_t c;
};
static_assert(sizeof(EventRecord) == 48);

struct EventRing {
  EventRecord* items = nullptr;
  std::uint32_t capacity = 0;  // power of two (Config.eventRingCapacity rounded up)
  std::uint32_t reserved = 0;
  alignas(64) std::atomic<std::uint64_t> write{0};   // producer: Publish
  alignas(64) std::atomic<std::uint64_t> read{0};    // consumer: Update
  alignas(64) std::atomic<std::uint32_t> dropped{0};
};

// --- 3. Snapshots ---------------------------------------------------------------
struct PlaybackSnapshot {
  std::uint32_t generation;  // 0 = slot not live
  std::uint8_t state;        // snd_playback_state
  std::uint8_t hasMusical;
  std::uint16_t reserved;
  std::uint64_t frames;      // GetPosition
  snd_musical_time musical;
  std::uint32_t transportSlot;
};
static_assert(sizeof(PlaybackSnapshot) == 32 && sizeof(PlaybackSnapshot) % 8 == 0);

struct TransportSnapshot {
  std::uint32_t generation;
  std::uint32_t state;  // snd_transport_state
  snd_musical_time position;
  std::uint32_t beatsPerBar;
  double bpm;
  ClockSample clock;
};
static_assert(sizeof(TransportSnapshot) == 40 && sizeof(TransportSnapshot) % 8 == 0);

struct BusSnapshot {
  Id busId;
  std::uint32_t channels;
  float peak[kMaxChannels];
  float rms[kMaxChannels];
  float loudnessShort;
  float loudnessIntegrated;
};
static_assert(sizeof(BusSnapshot) == 80 && sizeof(BusSnapshot) % 8 == 0);

struct SnapshotFrame {
  std::atomic<std::uint32_t> seq{0};  // seqlock: odd while Publish writes
  std::uint32_t reserved = 0;
  BlockIndex block = 0;
  ClockSample clock = 0;
  snd_stats stats{};
  PlaybackSnapshot* playbacks = nullptr;    // [Config.maxPlaybacks], by slot
  TransportSnapshot* transports = nullptr;  // [Config.maxTransports], by slot
  BusSnapshot* buses = nullptr;             // [busCapacity], master first
  std::uint32_t busCount = 0;
  std::uint32_t busCapacity = 0;
};
static_assert(sizeof(snd_stats) % 8 == 0);

struct SnapshotTripleBuffer {
  SnapshotFrame frames[3];
  // bits 0-1: middle index, bit 2: dirty (new data since the reader's last swap)
  std::atomic<std::uint32_t> middle{1u};
  std::uint32_t back = 0;   // writer private (Publish)
  std::uint32_t front = 2;  // reader private (Update) - queries load `frontPublished`
  std::atomic<std::uint32_t> frontPublished{2u};
};
inline constexpr std::uint32_t kTripleDirtyBit = 4u;

}  // namespace snd::notify

#endif  // SND_INTERNAL_NOTIFY_HPP
