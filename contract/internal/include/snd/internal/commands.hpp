// Command types and their binary representation (design.md 2.1, 2.5, 12.2-4).
// FROZEN CONTRACT.
//
// Recording (game side, per registered thread):
//   A producer owns Config.commands.arenasPerThread arenas. Each arena is
//
//     [ArenaHeader 64B][normal region arenaBytes][reserve region reserveBytes]
//
//   Commands are appended as [CommandHeader][payload], 8-byte aligned, size a
//   multiple of 8. Every command first goes to the normal region. When it is
//   full (and, so that the application order equals the recording order, for
//   every later command of the same arena once the reserve or the fallback
//   section is in use):
//     - Delivery::MustDeliver commands (stop, parameters, release, ...) go to
//       the reserve region;
//     - Play-family commands go to the reserve only if the event priority is
//       >= Config.commands.criticalPriority (Delivery::PlayCritical), otherwise
//       they are rejected at record time: the handle is still returned and the
//       game side ends it with SND_END_REJECTED/COMMAND_OVERFLOW at the next
//       Update (2.1, 13.1);
//     - Delivery::Droppable commands (transform/listener updates, auditions)
//       are dropped and counted (stats.rejected_commands).
//   If the reserve is also full, must-deliver commands are coalesced into the
//   arena's OverflowFallback section (stop/destroy bitsets, last-value table
//   for parameters/switches; CONTRACTS.md D-11). A must-deliver command the
//   fallback cannot represent is refused synchronously instead
//   (SND_ERR_COMMAND_BUFFER_FULL / invalid handle). Losing a command is then
//   impossible except for parameter values beyond overflowParamSlots, which
//   is counted in stats.reserve_overflows (coalesced commands are not counted).
//
// Applying (audio thread, ApplyCommands, block start):
//   Producers are drained in registration order (2.1). Within an arena the
//   order is: normal region, reserve region, fallback section. Because the
//   reserve is used only after the normal region filled up, this equals the
//   recording order. The audio thread stamps ArenaHeader::appliedBlock with
//   the index of the block whose ApplyCommands consumed the arena; the replay
//   recorder (11.3) stores (producer, appliedBlock, arena bytes). All commands
//   of an arena share that block number.
//
// Commands are in-process data: they may contain pointers (user data,
// callbacks, prepared banks). Replay files therefore store them per platform
// and map pointers through ids (bankId, ...). Sizes are asserted for 64-bit
// targets.
#ifndef SND_INTERNAL_COMMANDS_HPP
#define SND_INTERNAL_COMMANDS_HPP

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <type_traits>

#include "snd/internal/common.hpp"
#include "snd/types.h"

namespace snd::cmd {

inline constexpr std::uint32_t kAlign = 8;
inline constexpr std::uint32_t kMaxCommandBytes = 0xFFF8u;

struct CommandHeader {
  std::uint16_t type;       // CommandType
  std::uint16_t sizeBytes;  // header + payload, multiple of 8
  std::uint32_t seq;        // per-producer sequence number (wraps)
};
static_assert(sizeof(CommandHeader) == 8);

enum class Delivery : std::uint8_t {
  Droppable = 0,     // dropped when the normal region is full
  Play = 1,          // rejected when the normal region is full (handle ends REJECTED)
  PlayCritical = 2,  // Play with priority >= criticalPriority: may use the reserve
  MustDeliver = 3,   // reserve region, then OverflowFallback; never silently lost
};

enum class Origin : std::uint8_t {
  Game = 0,      // public API on a registered thread
  LiveLink = 1,  // live link thread (dev builds)
  Loader = 2,    // bank loading path on normal workers (10.3)
};

// ---------------------------------------------------------------------------
// Payloads (follow the header directly)
// ---------------------------------------------------------------------------
struct Suspend {};
struct Resume {};

struct BankRegister {  // loader -> audio thread: swap reference tables at block boundary (10.3)
  HandleRef bank;
  Id bankId;
  std::uint32_t languageId;  // 0 = not a language bank
  const void* prepared;      // bank module's prepared registration data (opaque to others)
};
struct BankUnload {
  HandleRef bank;
  std::uint32_t mode;  // snd_unload_mode
  std::uint32_t reserved;
};
struct SetLanguage {
  Id language;
  std::uint32_t reserved;
};

enum PlayFlags : std::uint32_t {
  kPlayOneShot = 1u << 0,      // no handle returned; no on_end callback
  kPlayHasPosition = 1u << 1,  // PlayAt: use `position`, no emitter
};
struct Play {
  HandleRef playback;  // allocated by the game side (one-shots too)
  HandleRef emitter;
  Id event;
  std::uint32_t flags;  // PlayFlags
  snd_when when;
  Vec3 position;
  std::uint32_t priorityHint;  // event priority known at record time (D-10), 0 if unknown
};
struct PlaySynth {
  HandleRef playback;
  HandleRef emitter;
  snd_when when;
  snd_synth_desc desc;
};
struct PlayUserBuffer {
  HandleRef playback;
  HandleRef emitter;
  HandleRef buffer;
  Id bus;
  std::uint32_t reserved;
  snd_when when;
};
struct PlayInput {
  HandleRef playback;
  HandleRef emitter;
  snd_input_desc desc;
  snd_when when;
};
struct Stop {
  HandleRef playback;
  float fadeSeconds;  // < 0 = event default
  std::uint32_t reserved;
  snd_when when;
};
struct PauseResume {
  HandleRef playback;
  float fadeSeconds;
  std::uint32_t pause;  // 1 = pause, 0 = resume
};
struct SetScheduledStop {
  HandleRef playback;
  float fadeSeconds;
  std::uint32_t reserved;
  snd_when when;
};

struct CreateEmitter {
  HandleRef emitter;
};
struct DestroyEmitter {
  HandleRef emitter;
};
enum TransformMask : std::uint32_t {
  kTransformPosition = 1u << 0,
  kTransformForward = 1u << 1,
  kTransformVelocity = 1u << 2,
};
struct SetTransform {
  HandleRef emitter;
  Vec3 position;
  Vec3 forward;
  Vec3 velocity;
  std::uint32_t mask;  // TransformMask: which fields are valid
};
struct SetListener {
  std::uint32_t index;
  std::uint32_t reserved;
  snd_listener_desc listener;
};

struct SetParam {
  std::uint8_t scope;  // ParamScope
  std::uint8_t reserved[3];
  Id param;
  HandleRef target;  // emitter / playback; none for Global
  float value;
  std::uint32_t reserved2;
};
struct SetSwitch {
  std::uint8_t scope;  // ParamScope
  std::uint8_t reserved[3];
  Id group;
  HandleRef target;
  Id value;
  std::uint32_t reserved2;
};

struct SetBusVolume {
  Id bus;
  float volume;
  float fadeSeconds;
  std::uint32_t reserved;
};
struct ApplySnapshot {
  Id snapshot;
  float weight;
  float timeSeconds;
  std::uint32_t reserved;
};
struct SetCategoryVolume {
  Id category;  // SND_ID_NONE = all
  float volume;
  float fadeSeconds;
  std::uint32_t reserved;
};
struct PauseCategory {
  Id category;
  std::uint32_t paused;
};
struct MuteCategory {
  Id category;
  std::uint32_t muted;
};

// 1.2 (D-89): `variant` selects what the command asks for. 1.1 recorders wrote 0 there.
//   kMusicStateVariantState   SetMusicState: `state` is the MSTA id (snd_set_music_state)
//   kMusicStateVariantStinger snd_play_stinger: `state` is the MSTG id of the music event
//                             of `playback`; IMMEDIATE = MSTG.timing on the playback's
//                             transport, another kind overrides it. Core makes a
//                             ReservationAction::Stinger reservation (music_if.hpp).
// Other values are ignored by core (and refused by the replay's shape rule).
enum MusicStateVariant : std::uint32_t {
  kMusicStateVariantState = 0,
  kMusicStateVariantStinger = 1,
};
struct SetMusicState {
  HandleRef playback;
  Id state;
  std::uint32_t variant;  // MusicStateVariant (1.1: reserved, 0)
  snd_when when;  // IMMEDIATE = rule timing (8.5)
};
struct PrepareMusicState {
  HandleRef playback;
  Id state;
  std::uint32_t reserved;
};
struct CreateTransport {
  HandleRef transport;
  snd_transport_desc desc;
};
struct DestroyTransport {
  HandleRef transport;
};
struct SetTransportState {
  HandleRef transport;
  std::uint32_t state;  // snd_transport_state
  std::uint32_t reserved;
  snd_when when;
};
// 1.2 (D-88): `variant` selects the transport rate that changes. 1.1 recorders wrote 0.
//   kTempoVariantBpm    snd_set_tempo: `bpm` is the tempo (quarter notes per minute)
//   kTempoVariantPitch  snd_set_transport_pitch: `bpm` holds the pitch ratio (> 0, the
//                       same entry rule as a tempo; core clamps it to [1/16, 16]).
//                       Core makes a ReservationAction::SetPitch reservation.
// Other values are ignored by core (and refused by the replay's shape rule).
enum TempoVariant : std::uint32_t {
  kTempoVariantBpm = 0,
  kTempoVariantPitch = 1,
};
struct SetTempo {
  HandleRef transport;
  float bpm;              // or the pitch ratio (kTempoVariantPitch)
  std::uint32_t variant;  // TempoVariant (1.1: reserved, 0)
  snd_when when;
};
struct SeekTransport {
  HandleRef transport;
  snd_musical_time position;
  std::uint32_t reserved;
  snd_when when;
};

struct CreateUserBuffer {
  HandleRef buffer;
  snd_user_buffer_desc desc;
};
struct DestroyUserBuffer {
  HandleRef buffer;
};

// Live link (11.2), dev builds. Values are raw bits interpreted by valueType.
enum class LiveValueType : std::uint32_t { F32 = 0, U32 = 1, I32 = 2, Id = 3 };
struct LiveSetValue {
  Id objectId;          // runtime id of the edited object (event, bus, param, ...)
  std::uint32_t field;  // field id per spec/interface/livelink-protocol.md
  std::uint32_t valueType;  // LiveValueType
  std::uint32_t index;      // element index for array fields, else 0
  std::uint64_t value;
};
struct LiveReplaceChunk {
  Id bankId;
  std::uint32_t fourcc;
  const void* prepared;  // loader-prepared replacement (epoch-released afterwards)
};
struct LiveAudition {
  HandleRef playback;
  Id event;
  std::uint32_t flags;  // PlayFlags
  Vec3 position;
  std::uint32_t reserved;
};
enum class LiveOverrideKind : std::uint32_t { Param = 0, MusicState = 1, Switch = 2 };
struct LiveOverride {
  std::uint32_t kind;  // LiveOverrideKind
  Id id;               // param / switch group / music playback event
  HandleRef target;    // playback for MusicState, none for global overrides
  std::uint32_t value; // float bits for Param, Id otherwise
  std::uint32_t enable;  // 0 = remove the override
};
struct LiveClearOverrides {};

// 1.1: snd_declare_language (10.4). The name is copied into the command
// (NUL-terminated, < SND_MAX_LANGUAGE_NAME_BYTES); language = snd_hash_id(name).
struct DeclareLanguage {
  Id language;
  std::uint32_t nameBytes;  // strlen(name)
  char name[SND_MAX_LANGUAGE_NAME_BYTES];
};

// ---------------------------------------------------------------------------
// Command table: X(Type, PayloadStruct, Delivery, Origin)
// Append-only (minor version). Values are wire-stable for replay files.
// ---------------------------------------------------------------------------
#define SND_COMMANDS(X)                                          \
  X(Suspend, Suspend, MustDeliver, Game)                         \
  X(Resume, Resume, MustDeliver, Game)                           \
  X(BankRegister, BankRegister, MustDeliver, Loader)             \
  X(BankUnload, BankUnload, MustDeliver, Game)                   \
  X(SetLanguage, SetLanguage, MustDeliver, Game)                 \
  X(Play, Play, Play, Game)                                      \
  X(PlaySynth, PlaySynth, Play, Game)                            \
  X(PlayUserBuffer, PlayUserBuffer, Play, Game)                  \
  X(PlayInput, PlayInput, Play, Game)                            \
  X(Stop, Stop, MustDeliver, Game)                               \
  X(PauseResume, PauseResume, MustDeliver, Game)                 \
  X(SetScheduledStop, SetScheduledStop, MustDeliver, Game)       \
  X(CreateEmitter, CreateEmitter, MustDeliver, Game)             \
  X(DestroyEmitter, DestroyEmitter, MustDeliver, Game)           \
  X(SetTransform, SetTransform, Droppable, Game)                 \
  X(SetListener, SetListener, Droppable, Game)                   \
  X(SetParam, SetParam, MustDeliver, Game)                       \
  X(SetSwitch, SetSwitch, MustDeliver, Game)                     \
  X(SetBusVolume, SetBusVolume, MustDeliver, Game)               \
  X(ApplySnapshot, ApplySnapshot, MustDeliver, Game)             \
  X(SetCategoryVolume, SetCategoryVolume, MustDeliver, Game)     \
  X(PauseCategory, PauseCategory, MustDeliver, Game)             \
  X(MuteCategory, MuteCategory, MustDeliver, Game)               \
  X(SetMusicState, SetMusicState, MustDeliver, Game)             \
  X(PrepareMusicState, PrepareMusicState, Droppable, Game)       \
  X(CreateTransport, CreateTransport, MustDeliver, Game)         \
  X(DestroyTransport, DestroyTransport, MustDeliver, Game)       \
  X(SetTransportState, SetTransportState, MustDeliver, Game)     \
  X(SetTempo, SetTempo, MustDeliver, Game)                       \
  X(SeekTransport, SeekTransport, MustDeliver, Game)             \
  X(CreateUserBuffer, CreateUserBuffer, MustDeliver, Game)       \
  X(DestroyUserBuffer, DestroyUserBuffer, MustDeliver, Game)     \
  X(LiveSetValue, LiveSetValue, MustDeliver, LiveLink)           \
  X(LiveReplaceChunk, LiveReplaceChunk, MustDeliver, LiveLink)   \
  X(LiveAudition, LiveAudition, Droppable, LiveLink)             \
  X(LiveOverride, LiveOverride, MustDeliver, LiveLink)           \
  X(LiveClearOverrides, LiveClearOverrides, MustDeliver, LiveLink) \
  /* contract 1.1 */                                             \
  X(DeclareLanguage, DeclareLanguage, MustDeliver, Game)

enum class CommandType : std::uint16_t {
  Invalid = 0,
#define SND_CMD_ENUM(T, P, D, O) T,
  SND_COMMANDS(SND_CMD_ENUM)
#undef SND_CMD_ENUM
  Count
};

constexpr std::uint32_t AlignUp8(std::size_t n) noexcept {
  return static_cast<std::uint32_t>((n + 7u) & ~static_cast<std::size_t>(7u));
}

// Size of header + payload. Empty payload structs occupy 0 bytes on the wire.
template <class P>
inline constexpr std::uint32_t kWireSize =
    std::is_empty_v<P> ? static_cast<std::uint32_t>(sizeof(CommandHeader))
                       : AlignUp8(sizeof(CommandHeader) + sizeof(P));

template <class P>
struct CommandTraits;

#define SND_CMD_TRAITS(T, P, D, O)                                          \
  static_assert(std::is_trivially_copyable_v<P>, #P " must be POD");        \
  static_assert(alignof(P) <= kAlign, #P " alignment exceeds 8");           \
  template <>                                                               \
  struct CommandTraits<P> {                                                 \
    static constexpr CommandType kType = CommandType::T;                    \
    static constexpr Delivery kDelivery = Delivery::D;                      \
    static constexpr Origin kOrigin = Origin::O;                            \
    static constexpr std::uint32_t kSize = kWireSize<P>;                    \
  };
SND_COMMANDS(SND_CMD_TRAITS)
#undef SND_CMD_TRAITS

struct CommandInfo {
  CommandType type;
  const char* name;
  std::uint32_t wireSize;
  Delivery delivery;
  Origin origin;
};
inline constexpr CommandInfo kCommandInfo[] = {
    CommandInfo{CommandType::Invalid, "Invalid", 0u, Delivery::Droppable, Origin::Game},
#define SND_CMD_INFO(T, P, D, O) \
  CommandInfo{CommandType::T, #T, kWireSize<P>, Delivery::D, Origin::O},
    SND_COMMANDS(SND_CMD_INFO)
#undef SND_CMD_INFO
};
static_assert(sizeof(kCommandInfo) / sizeof(kCommandInfo[0]) ==
              static_cast<std::size_t>(CommandType::Count));

constexpr const CommandInfo* FindCommandInfo(std::uint16_t type) noexcept {
  return (type > 0u && type < static_cast<std::uint16_t>(CommandType::Count)) ? &kCommandInfo[type]
                                                                              : nullptr;
}

// Wire sizes on 64-bit targets (frozen; changing one is a major contract change).
#if UINTPTR_MAX == 0xFFFFFFFFFFFFFFFFu
static_assert(kWireSize<Suspend> == 8);
static_assert(kWireSize<BankRegister> == 32);
static_assert(kWireSize<BankUnload> == 24);
static_assert(kWireSize<Play> == 80);
static_assert(kWireSize<PlaySynth> == 112);
static_assert(kWireSize<PlayUserBuffer> == 72);
static_assert(kWireSize<PlayInput> == 72);
static_assert(kWireSize<Stop> == 56);
static_assert(kWireSize<PauseResume> == 24);
static_assert(kWireSize<SetTransform> == 56);
static_assert(kWireSize<SetListener> == 72);
static_assert(kWireSize<SetParam> == 32);
static_assert(kWireSize<SetSwitch> == 32);
static_assert(kWireSize<SetMusicState> == 56);
static_assert(kWireSize<CreateTransport> == 32);
static_assert(kWireSize<SeekTransport> == 64);
static_assert(kWireSize<CreateUserBuffer> == 48);
static_assert(kWireSize<LiveSetValue> == 32);
static_assert(kWireSize<LiveOverride> == 32);
static_assert(kWireSize<DeclareLanguage> == 48);  // 1.1
#endif

// ---------------------------------------------------------------------------
// Arena layout
// ---------------------------------------------------------------------------
enum ArenaFlags : std::uint32_t {
  kArenaHasFallback = 1u << 0,  // OverflowFallback section is in use
  kArenaDroppedCommands = 1u << 1,
};

struct alignas(64) ArenaHeader {
  std::uint32_t producer;      // registration index of the recording thread
  std::uint32_t arenaIndex;
  std::uint64_t publishSeq;    // per producer, increments on every publish
  std::uint32_t normalBytes;   // used bytes in the normal region
  std::uint32_t reserveBytes;  // used bytes in the reserve region
  std::uint32_t commandCount;
  std::uint32_t flags;         // ArenaFlags
  BlockIndex appliedBlock;     // written by the audio thread when applied
  std::uint32_t droppedCount;  // Droppable / rejected commands (stats)
  std::uint32_t reserved[5];
};
static_assert(sizeof(ArenaHeader) == 64);

// Coalesced must-deliver commands when both regions are full (D-11). Bitsets
// are indexed by slot; the param table keeps the last value per key.
struct OverflowParam {
  std::uint8_t scope;  // ParamScope
  std::uint8_t isSwitch;
  std::uint16_t reserved;
  Id id;               // param or switch group
  HandleRef target;
  std::uint32_t value; // float bits or switch value id
  std::uint32_t used;
};
static_assert(sizeof(OverflowParam) == 24);

struct OverflowFallback {
  std::uint64_t* stopPlayback;       // Config.maxPlaybacks bits: Stop with event default fade
  std::uint64_t* destroyEmitter;     // maxEmitters bits
  std::uint64_t* destroyTransport;   // maxTransports bits
  std::uint64_t* destroyUserBuffer;  // maxUserBuffers bits
  std::uint64_t* unloadBank;         // maxBanks bits (StopAndUnload)
  OverflowParam* params;             // Config.commands.overflowParamSlots entries
  std::uint32_t paramCapacity;
  std::uint32_t lostParams;          // values that did not fit (stats.reserve_overflows)
};

// Iterates commands in [begin, end). Returns false on a malformed stream
// (unknown type, bad size) - used by ApplyCommands asserts and fuzz tests.
template <class Fn>
inline bool ForEachCommand(const std::uint8_t* begin, const std::uint8_t* end, Fn&& fn) noexcept {
  const std::uint8_t* p = begin;
  while (p < end) {
    if (static_cast<std::size_t>(end - p) < sizeof(CommandHeader)) return false;
    CommandHeader h;
    std::memcpy(&h, p, sizeof(h));
    const CommandInfo* info = FindCommandInfo(h.type);
    if (info == nullptr || h.sizeBytes != info->wireSize ||
        static_cast<std::size_t>(end - p) < h.sizeBytes) {
      return false;
    }
    fn(h, p + sizeof(CommandHeader));
    p += h.sizeBytes;
  }
  return true;
}

// ---------------------------------------------------------------------------
// Stage 3 (coverage, group 2; D-130, D-132): variants carried in reserved fields.
// No new command type and no layout change. A variant of 0 is the command's original
// meaning (bit for bit); appliers drop variants they do not know.
//  - SetBusVolume.reserved: bits 0-7 = variant. Variant 1 (kBusVolumeVariantEffectParam)
//    = bus effect parameter (snd_set_effect_param): bits 8-15 effect index in the bus's
//    chain, bits 16-23 parameter index; `volume` = the value (any finite value, may be
//    negative), `fadeSeconds` = the fade (>= 0). `bus` as for the volume (0 = master).
//  - SetSwitch.reserved[0] = 1 (kSwitchVariantVoiceEffect): voice effect kind of an Event
//    (snd_set_event_voice_effect): scope Global, `group` = event id, `value` = effect kind
//    (0 = remove the stage), reserved[1] = stage, reserved[2] = number of SetParam
//    variant-1 values recorded right after it.
//  - SetParam.reserved[0] = 1 (kParamVariantVoiceEffect): voice effect parameter:
//    reserved[1] = stage, reserved[2] = parameter index, `value` (finite). Scope Global:
//    `param` = event id (the Event's configuration). Scope Playback: `target` = the playback
//    (snd_set_playback_effect_param).
// Variant commands never go to the coalescing fallback (it keeps no variant).
// ---------------------------------------------------------------------------
inline constexpr std::uint32_t kBusVolumeVariantMask = 0xFFu;
inline constexpr std::uint32_t kBusVolumeVariantEffectParam = 1u;
inline constexpr std::uint32_t kBusVolumeEffectIndexShift = 8u;
inline constexpr std::uint32_t kBusVolumeParamIndexShift = 16u;
inline constexpr std::uint32_t kBusVolumeIndexMask = 0xFFu;
inline constexpr std::uint8_t kSwitchVariantVoiceEffect = 1u;
inline constexpr std::uint8_t kParamVariantVoiceEffect = 1u;
constexpr std::uint32_t BusEffectParamReserved(std::uint32_t effect, std::uint32_t param) noexcept {
  return kBusVolumeVariantEffectParam | ((effect & kBusVolumeIndexMask) << kBusVolumeEffectIndexShift) |
         ((param & kBusVolumeIndexMask) << kBusVolumeParamIndexShift);
}

// Emitter SetParam variant 2: kind in reserved[1], x/y/z float bits
// in value/param/reserved2. Variant 3 holds zone amount/value and fade/reserved2.
inline constexpr std::uint8_t kParamVariantShape = 2u;
inline constexpr std::uint8_t kParamVariantZone = 3u;
// Emitter SetSwitch variant 2: value = zone bus. Zero removes in one record;
// a nonzero value is immediately followed by SetParam variant 3.
inline constexpr std::uint8_t kSwitchVariantZone = 2u;

}  // namespace snd::cmd

#endif  // SND_INTERNAL_COMMANDS_HPP
