// The music system (design 8; IMusicSystem of snd/internal/music_if.hpp).
//
// Owns UpdateTransport (system 2) and the music logic inside RunEventLogic:
//   - transports: tempo map (segment tempo points + SetTempo overrides), time
//     signatures, position, Stopped / Playing / Paused, seek, beat notices;
//   - reservations (8.2): resolves each When into a musical tick (or a clock),
//     fires it sample-accurately when it falls inside the block, keeps the
//     musical target across tempo changes and seeks, applies the late rule
//     ("next grid" / "start late" / "cancel") when the target is closer than
//     the material's prime time;
//   - music playbacks (8.3-8.6): states, playlists (sequential / weighted
//     random / shuffle, repeats, loop counts), segments with pickup and tail,
//     stems (separate files; volume curves on parameters), the transition rule
//     table and its state machine (Idle / Pending / Transitioning, interrupt
//     rules, bridge segments, fades, start positions: head / same bar /
//     marker / last position), stingers (replace / stack), sync groups and
//     the sound-priority stall of the transport (8.4).
//
// Construction: a MusicSystem object is small; core gives it its memory pool
// through Init (PoolBytes from MusicLimits). The api's composition root keeps
// one MusicSystem per snd_system and passes it as CoreDesc::music.
// Threads: every IMusicSystem call comes from core on the audio thread (Init /
// Shutdown on the owner thread). The introspection calls below read the
// audio-thread state: call them only while no block is rendering (tests, or
// the audio thread itself).
#ifndef SND_MUSIC_MUSIC_SYSTEM_HPP
#define SND_MUSIC_MUSIC_SYSTEM_HPP

#include <cstddef>
#include <cstdint>

#include "snd/internal/music_if.hpp"

namespace snd::music {

namespace detail {
struct State;
}  // namespace detail

// What a music playback is doing (tests, the tool's timeline view 8.7).
struct MusicPlaybackInfo {
  Id state = 0;               // current state (the target of the last executed transition)
  Id pendingState = 0;        // Pending: the target waiting for its timing
  Id queuedState = 0;         // Transitioning with "after bridge": the one queued request
  std::uint32_t phase = 0;    // 0 idle, 1 pending, 2 transitioning
  Id segment = 0;             // MSEG id of the main segment at the current position (0 = none)
  MusicalTicks segmentStart = 0;  // its bar 0 on the transport
  MusicalTicks pendingTick = 0;
  std::uint32_t transitions = 0;  // executed transitions
  std::uint32_t deferrals = 0;    // late rule "next grid" moves (transitions and reservations)
  std::uint32_t cancels = 0;      // late rule "cancel"
  std::uint32_t started = 0;      // 1 once PlaybackStarted was seen
};

// One executed transition (8.7: "which beat a transition happened on").
struct MusicTransitionRecord {
  SlotIndex playback;
  Id from;
  Id to;
  std::uint32_t lateRule;   // 0xFF none, else the ReservationLateRule that moved / started it late
  MusicalTicks tick;        // transport tick of the switch
  ClockSample clock;        // common clock of the switch
  std::uint32_t deferrals;
  std::uint32_t reserved;
};

class MusicSystem final : public IMusicSystem {
 public:
  MusicSystem() noexcept = default;
  ~MusicSystem() override = default;
  MusicSystem(const MusicSystem&) = delete;
  MusicSystem& operator=(const MusicSystem&) = delete;

  std::size_t PoolBytes(const MusicLimits& limits) const noexcept override;
  bool Init(void* pool, std::size_t bytes, const MusicLimits& limits, const MusicServices& services) noexcept override;
  void Shutdown() noexcept override;
  void UpdateTransport(const MusicFrame& frame, schema::Tables& tables, FireList& fire) noexcept override;
  void RunMusicLogic(const MusicFrame& frame, const schema::Tables& tables, const MusicInput* inputs,
                     std::uint32_t count, MusicVoiceRequestList& out) noexcept override;
  void OnVoiceResults(const MusicVoiceResult* results, std::uint32_t count) noexcept override;
  std::uint32_t CollectNotifications(notify::EventRecord* out, std::uint32_t capacity) noexcept override;
  bool TicksToClock(SlotIndex transport, MusicalTicks ticks, ClockSample* out) const noexcept override;
  bool ClockToTicks(SlotIndex transport, ClockSample clock, MusicalTicks* out) const noexcept override;

  // --- introspection (no block rendering concurrently) ---
  bool GetPlaybackInfo(SlotIndex playback, MusicPlaybackInfo* out) const noexcept;
  // Copies up to `capacity` of the most recent transitions, oldest first. Returns the count.
  std::uint32_t TransitionLog(MusicTransitionRecord* out, std::uint32_t capacity) const noexcept;
  std::uint32_t DroppedNotifications() const noexcept;

 private:
  detail::State* s_ = nullptr;
};

}  // namespace snd::music

#endif  // SND_MUSIC_MUSIC_SYSTEM_HPP
