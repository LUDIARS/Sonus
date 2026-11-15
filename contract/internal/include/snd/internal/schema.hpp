// Runtime table schema and column ownership (design.md 3.1, 3.2). FROZEN CONTRACT.
//
// Every table is a fixed-capacity SoA: one array per column, indexed by DENSE
// position; a sparse index maps stable slots to dense positions (3.1).
// Cross-table references always store slots, never dense positions.
//
// Ownership rules encoded here (checked at compile time):
//  1. Each column has exactly one writer system (by construction: the writer
//     is part of the column declaration). Systems obtain writable column
//     pointers only through WriteColumn<System, Column>(), which
//     static_asserts the ownership.
//  2. Each system may read only the columns it owns or lists in
//     SND_SCHEMA_READS, through ReadColumn<System, Column>().
//  3. Every declared read states its timing:
//       Current  - written earlier in the same block (writer order < reader order)
//       Previous - value of the previous block (writer order >= reader order)
//       Async    - written by a non-audio thread; the column type is std::atomic
//     ValidateReads() rejects inconsistent declarations (static_assert below).
//  4. Rows are created / destroyed only by the systems in SND_SCHEMA_TABLES
//     (RowOwner checks).
// System-private state that no other system reads (voice-limit counters, the
// transition state machine, ...) is not part of this schema.
#ifndef SND_INTERNAL_SCHEMA_HPP
#define SND_INTERNAL_SCHEMA_HPP

#include <atomic>
#include <cstddef>
#include <cstdint>

#include "snd/ext/types.hpp"
#include "snd/internal/common.hpp"

namespace snd::schema {

// ---------------------------------------------------------------------------
// Systems (3.2) in execution order, plus the asynchronous writers.
// ---------------------------------------------------------------------------
enum class SystemId : std::uint8_t {
  None = 0,
  ApplyCommands = 1,
  UpdateTransport = 2,     // implemented by the music module (music_if.hpp)
  EvaluateLogicParams = 3,
  RunEventLogic = 4,       // core; music logic runs inside it (music_if.hpp)
  Compute3D = 5,
  EvaluateDspParams = 6,
  SelectVoices = 7,
  BuildRenderList = 8,
  Render = 9,              // core drives; source kinds render batches (source_if.hpp)
  Publish = 10,
  ExternalIo = 100,        // IO thread (stream reads)
  ExternalDecode = 101,    // decode jobs on the prefetch lane
};

constexpr bool IsAudioThreadSystem(SystemId s) noexcept {
  return static_cast<std::uint8_t>(s) >= 1u && static_cast<std::uint8_t>(s) <= 10u;
}
constexpr bool IsExternal(SystemId s) noexcept {
  return s == SystemId::ExternalIo || s == SystemId::ExternalDecode;
}
constexpr int Order(SystemId s) noexcept { return static_cast<int>(s); }

constexpr const char* SystemName(SystemId s) noexcept {
  switch (s) {
    case SystemId::None: return "None";
    case SystemId::ApplyCommands: return "ApplyCommands";
    case SystemId::UpdateTransport: return "UpdateTransport";
    case SystemId::EvaluateLogicParams: return "EvaluateLogicParams";
    case SystemId::RunEventLogic: return "RunEventLogic";
    case SystemId::Compute3D: return "Compute3D";
    case SystemId::EvaluateDspParams: return "EvaluateDspParams";
    case SystemId::SelectVoices: return "SelectVoices";
    case SystemId::BuildRenderList: return "BuildRenderList";
    case SystemId::Render: return "Render";
    case SystemId::Publish: return "Publish";
    case SystemId::ExternalIo: return "ExternalIo";
    case SystemId::ExternalDecode: return "ExternalDecode";
  }
  return "?";
}

enum class Temperature : std::uint8_t { Hot = 0, Cold = 1 };
enum class Indexing : std::uint8_t {
  Sparse = 0,  // dense array + sparse slot index, swap-remove (3.1)
  Direct = 1,  // slot == position (fixed-shape tables: Listener, Spatial; Stream and RealVoice since 1.1)
};
enum class ReadTiming : std::uint8_t { Current = 0, Previous = 1, Async = 2 };

using AtomicU64 = std::atomic<std::uint64_t>;
using U8 = std::uint8_t;
using U32 = std::uint32_t;
using U64 = std::uint64_t;
using F32 = float;

// ---------------------------------------------------------------------------
// Tables: T(Table, Indexing, RowCreator, RowDestroyer, capacity)
// ---------------------------------------------------------------------------
#define SND_SCHEMA_TABLES(T)                                                                    \
  T(Playback, Sparse, ApplyCommands, RunEventLogic, "Config.maxPlaybacks")                      \
  T(Voice, Sparse, RunEventLogic, RunEventLogic, "Config.maxVirtualVoices")                     \
  T(RealVoice, Direct, SelectVoices, SelectVoices, "maxRealVoices + fadeReserveVoices; slot = position (D-43)") \
  T(Emitter, Sparse, ApplyCommands, ApplyCommands, "Config.maxEmitters")                        \
  T(Listener, Direct, ApplyCommands, ApplyCommands, "Config.maxListeners (<= 4)")               \
  T(Spatial, Direct, Compute3D, Compute3D, "maxEmitters * maxListeners; slot = e*L + l")        \
  T(ParamGlobal, Sparse, ApplyCommands, ApplyCommands, "Config.maxGlobalParams")                \
  T(ParamEmitter, Sparse, ApplyCommands, ApplyCommands,                                         \
    "maxEmitters*8 inline + paramOverflowBlocks*16 shared")                                     \
  T(ParamPlayback, Sparse, ApplyCommands, RunEventLogic,                                        \
    "maxPlaybacks*8 inline + paramOverflowBlocks*16 shared")                                    \
  T(Bus, Sparse, ApplyCommands, ApplyCommands, "sum over loaded banks + devReserve.buses")      \
  T(Stream, Direct, RunEventLogic, RunEventLogic, "Config.maxStreams; slot = position (D-45)")  \
  T(Transport, Sparse, ApplyCommands, UpdateTransport, "Config.maxTransports")                  \
  T(Reservation, Sparse, ApplyCommands, UpdateTransport, "Config.maxReservations")              \
  T(Category, Sparse, ApplyCommands, ApplyCommands, "sum over loaded banks")                    \
  T(Snapshot, Sparse, ApplyCommands, ApplyCommands, "sum over loaded banks")

// ---------------------------------------------------------------------------
// Columns: C(Table, column, Type, Temperature, Writer)
// ---------------------------------------------------------------------------
#define SND_COLS_Playback(C)                                                   \
  C(Playback, eventRef, EventRef, Hot, ApplyCommands)                          \
  C(Playback, seed, U32, Hot, ApplyCommands)                                   \
  C(Playback, emitterSlot, SlotIndex, Hot, ApplyCommands)                      \
  C(Playback, transportSlot, SlotIndex, Hot, ApplyCommands)                    \
  C(Playback, request, PlaybackRequest, Hot, ApplyCommands)                    \
  C(Playback, state, PlaybackStateId, Hot, RunEventLogic)                      \
  C(Playback, schedule, PlaybackSchedule, Hot, RunEventLogic)                  \
  C(Playback, logic, PlaybackLogic, Hot, RunEventLogic)                        \
  C(Playback, eventId, Id, Cold, ApplyCommands)                                \
  C(Playback, dynamicSource, DynamicSourceRef, Cold, ApplyCommands)            \
  C(Playback, endInfo, PlaybackEnd, Cold, RunEventLogic)

#define SND_COLS_Voice(C)                                                      \
  C(Voice, playbackSlot, SlotIndex, Hot, RunEventLogic)                        \
  C(Voice, sourceKind, SourceKind, Hot, RunEventLogic)                         \
  C(Voice, sourceSlot, SlotIndex, Hot, RunEventLogic)                          \
  C(Voice, busSlot, SlotIndex, Hot, RunEventLogic)                             \
  C(Voice, emitterSlot, SlotIndex, Hot, RunEventLogic)                         \
  C(Voice, priority, U32, Hot, RunEventLogic)                                  \
  C(Voice, limitGroup, U32, Hot, RunEventLogic)                                \
  C(Voice, lifecycle, VoiceLifecycle, Hot, RunEventLogic)                      \
  C(Voice, pitch, Ramp, Hot, EvaluateDspParams)                                \
  C(Voice, gain, Ramp, Hot, EvaluateDspParams)                                 \
  C(Voice, filterTarget, FilterTarget, Hot, EvaluateDspParams)                 \
  C(Voice, audibility, F32, Hot, EvaluateDspParams)                            \
  C(Voice, realSlot, SlotIndex, Hot, SelectVoices)                             \
  C(Voice, playhead, Playhead, Hot, Render)                                    \
  C(Voice, renderStatus, VoiceRenderStatus, Hot, Render)                       \
  C(Voice, renderHold, VoiceRenderHold, Hot, Render)                           \
  C(Voice, clipRef, ClipRef, Cold, RunEventLogic)                              \
  C(Voice, createdBlock, BlockIndex, Cold, RunEventLogic)

// RealVoice (1.1, D-43 / D-44): Indexing::Direct, slot == row position, rows
// never move (the real-voice limit is fixed). It holds the render state a
// chunk writes: a chunk writes ONLY RealVoice columns of its voices' real slots
// (playhead, renderStatus, filterState, panMatrix, appliedGain) and the pools
// indexed by the real slot (resampler state, D-44; per-voice source / effect
// state). RenderItem refers to the voice by RealVoice slot. Voice rows (Sparse,
// they move) are never written from a chunk: Voice.playhead / renderStatus are
// written by Render's serial part only (copied from RealVoice once the chunk
// is done, or held voices once IsQuiescent(); virtual voices advanced there).
#define SND_COLS_RealVoice(C)                                                  \
  C(RealVoice, voiceSlot, SlotIndex, Hot, SelectVoices)                        \
  C(RealVoice, assign, RealVoiceAssign, Hot, SelectVoices)                     \
  C(RealVoice, filterState, FilterState, Hot, Render)                          \
  C(RealVoice, panMatrix, PanMatrix, Hot, Render)                              \
  C(RealVoice, appliedGain, F32, Hot, Render)                                  \
  C(RealVoice, playhead, Playhead, Hot, Render)    /* 1.1: written by chunks */ \
  C(RealVoice, renderStatus, VoiceRenderStatus, Hot, Render) /* 1.1: by chunks */

#define SND_COLS_Emitter(C)                                                    \
  C(Emitter, position, Vec3, Hot, ApplyCommands)                               \
  C(Emitter, velocity, Vec3, Hot, ApplyCommands)                               \
  C(Emitter, forward, Vec3, Hot, ApplyCommands)                                \
  C(Emitter, lastUpdateBlock, BlockIndex, Hot, ApplyCommands)                  \
  C(Emitter, occlusion, Occlusion, Hot, EvaluateLogicParams)                   \
  C(Emitter, renderPosition, Vec3, Hot, Compute3D)                             \
  C(Emitter, createdBlock, BlockIndex, Cold, ApplyCommands)

#define SND_COLS_Listener(C)                                                   \
  C(Listener, position, Vec3, Hot, ApplyCommands)                              \
  C(Listener, velocity, Vec3, Hot, ApplyCommands)                              \
  C(Listener, forward, Vec3, Hot, ApplyCommands)                               \
  C(Listener, up, Vec3, Hot, ApplyCommands)                                    \
  C(Listener, weight, F32, Hot, ApplyCommands)                                 \
  C(Listener, enabled, U32, Hot, ApplyCommands)

#define SND_COLS_Spatial(C)                                                    \
  C(Spatial, distance, F32, Hot, Compute3D)                                    \
  C(Spatial, direction, Vec3, Hot, Compute3D)                                  \
  C(Spatial, dopplerRatio, F32, Hot, Compute3D)                                \
  C(Spatial, coneCos, F32, Hot, Compute3D)                                     \
  C(Spatial, valid, U32, Hot, Compute3D)

#define SND_PARAM_COLS(C, T)                                                   \
  C(T, paramId, Id, Hot, ApplyCommands)                                        \
  C(T, ownerSlot, SlotIndex, Hot, ApplyCommands)                               \
  C(T, kind, ParamKind, Hot, ApplyCommands)                                    \
  C(T, target, F32, Hot, ApplyCommands)                                        \
  C(T, rampRate, F32, Hot, ApplyCommands)                                      \
  C(T, nextSlot, SlotIndex, Hot, ApplyCommands)                                \
  C(T, current, F32, Hot, EvaluateLogicParams)
#define SND_COLS_ParamGlobal(C) SND_PARAM_COLS(C, ParamGlobal)
#define SND_COLS_ParamEmitter(C) SND_PARAM_COLS(C, ParamEmitter)
#define SND_COLS_ParamPlayback(C) SND_PARAM_COLS(C, ParamPlayback)

#define SND_COLS_Bus(C)                                                        \
  C(Bus, parentSlot, SlotIndex, Hot, ApplyCommands)                            \
  C(Bus, sendRange, RangeU32, Hot, ApplyCommands)                              \
  C(Bus, effectRange, RangeU32, Hot, ApplyCommands)                            \
  C(Bus, graphOrder, U32, Hot, ApplyCommands)                                  \
  C(Bus, volume, TimedRamp, Hot, ApplyCommands)                                \
  C(Bus, gainTarget, F32, Hot, EvaluateDspParams)                              \
  C(Bus, effectBypass, U32, Hot, EvaluateDspParams)                            \
  C(Bus, gain, F32, Hot, Render)                                               \
  C(Bus, peak, BusPeak, Hot, Render)                                           \
  C(Bus, busId, Id, Cold, ApplyCommands)

// Stream (1.1, D-45): Indexing::Direct, slot == row position, rows never move.
// The columns with non-audio-thread writers (readPos: ExternalIo, ringWrite:
// ExternalDecode) are indexed by the stream SLOT directly (the IO thread and the
// decode jobs hold the slot, never a dense position). Direct rows are never
// referred to by handle (base/sparse_set.hpp): liveness is per slot only.
// How io's per-slot StreamChannel atomics (ReadPos() ...) relate to these
// columns is the Stream source's choice (stage 2): either the source copies the
// IO-side value into the column, or the column's storage IS the channel's value.
#define SND_COLS_Stream(C)                                                     \
  C(Stream, fileId, FileId, Hot, RunEventLogic)                                \
  C(Stream, syncGroup, U32, Hot, RunEventLogic)                                \
  C(Stream, control, StreamControl, Hot, RunEventLogic)                        \
  C(Stream, ringRange, RangeU32, Hot, RunEventLogic)                           \
  C(Stream, readPos, AtomicU64, Hot, ExternalIo)                               \
  C(Stream, ringWrite, AtomicU64, Hot, ExternalDecode)                         \
  C(Stream, ringRead, AtomicU64, Hot, Render)                                  \
  C(Stream, stallCount, U32, Cold, Render)

#define SND_COLS_Transport(C)                                                  \
  C(Transport, ownerPlayback, SlotIndex, Hot, ApplyCommands)                   \
  C(Transport, lifetime, TransportLifetime, Hot, ApplyCommands)                \
  C(Transport, tempoMap, TempoMapRef, Hot, UpdateTransport)                    \
  C(Transport, signature, TimeSignature, Hot, UpdateTransport)                 \
  C(Transport, position, TransportPosition, Hot, UpdateTransport)              \
  C(Transport, state, U32, Hot, UpdateTransport)                               \
  C(Transport, pitch, F32, Hot, UpdateTransport)                               \
  C(Transport, loopRange, MusicalRange, Hot, UpdateTransport)                  \
  C(Transport, initial, TransportInit, Cold, ApplyCommands)

#define SND_COLS_Reservation(C)                                                \
  C(Reservation, transportSlot, SlotIndex, Hot, ApplyCommands)                 \
  C(Reservation, when, ReservationWhen, Hot, ApplyCommands)                    \
  C(Reservation, action, ReservationAction, Hot, ApplyCommands)                \
  C(Reservation, target, HandleRef, Hot, ApplyCommands)                        \
  C(Reservation, payload, ReservationPayload, Hot, ApplyCommands)              \
  C(Reservation, acceptedBlock, BlockIndex, Hot, ApplyCommands)                \
  C(Reservation, resolved, ResolvedTime, Hot, UpdateTransport)                 \
  C(Reservation, sequence, U64, Cold, ApplyCommands)

#define SND_COLS_Category(C)                                                   \
  C(Category, categoryId, Id, Hot, ApplyCommands)                              \
  C(Category, request, CategoryRequest, Hot, ApplyCommands)                    \
  C(Category, flags, U32, Hot, ApplyCommands)                                  \
  C(Category, gain, F32, Hot, EvaluateDspParams)

#define SND_COLS_Snapshot(C)                                                   \
  C(Snapshot, snapshotId, Id, Hot, ApplyCommands)                              \
  C(Snapshot, request, SnapshotRequest, Hot, ApplyCommands)                    \
  C(Snapshot, weight, F32, Hot, EvaluateDspParams)

#define SND_SCHEMA_ALL_COLUMNS(C)                                                                \
  SND_COLS_Playback(C) SND_COLS_Voice(C) SND_COLS_RealVoice(C) SND_COLS_Emitter(C)              \
  SND_COLS_Listener(C) SND_COLS_Spatial(C) SND_COLS_ParamGlobal(C) SND_COLS_ParamEmitter(C)     \
  SND_COLS_ParamPlayback(C) SND_COLS_Bus(C) SND_COLS_Stream(C) SND_COLS_Transport(C)            \
  SND_COLS_Reservation(C) SND_COLS_Category(C) SND_COLS_Snapshot(C)

// ---------------------------------------------------------------------------
// Reads (3.2 "読む" column): R(System, Table_column, Timing)
// Owners implicitly read their own columns and are not listed.
// ---------------------------------------------------------------------------
#define SND_SCHEMA_READS(R)                                                    \
  /* 2 UpdateTransport: Transport, Reservation, owner playback state */        \
  R(UpdateTransport, Transport_ownerPlayback, Current)                         \
  R(UpdateTransport, Transport_lifetime, Current)                              \
  R(UpdateTransport, Transport_initial, Current)                               \
  R(UpdateTransport, Reservation_transportSlot, Current)                       \
  R(UpdateTransport, Reservation_when, Current)                                \
  R(UpdateTransport, Reservation_action, Current)                              \
  R(UpdateTransport, Reservation_target, Current)                              \
  R(UpdateTransport, Reservation_payload, Current)                             \
  R(UpdateTransport, Reservation_acceptedBlock, Current)                       \
  R(UpdateTransport, Reservation_sequence, Current)                            \
  R(UpdateTransport, Playback_state, Previous)                                 \
  R(UpdateTransport, Playback_eventRef, Current)                               \
  /* 3 EvaluateLogicParams: ParamValue */                                      \
  R(EvaluateLogicParams, ParamGlobal_paramId, Current)                         \
  R(EvaluateLogicParams, ParamGlobal_kind, Current)                            \
  R(EvaluateLogicParams, ParamGlobal_target, Current)                          \
  R(EvaluateLogicParams, ParamGlobal_rampRate, Current)                        \
  R(EvaluateLogicParams, ParamEmitter_paramId, Current)                        \
  R(EvaluateLogicParams, ParamEmitter_ownerSlot, Current)                      \
  R(EvaluateLogicParams, ParamEmitter_kind, Current)                           \
  R(EvaluateLogicParams, ParamEmitter_target, Current)                         \
  R(EvaluateLogicParams, ParamEmitter_rampRate, Current)                       \
  R(EvaluateLogicParams, ParamPlayback_paramId, Current)                       \
  R(EvaluateLogicParams, ParamPlayback_kind, Current)                          \
  R(EvaluateLogicParams, ParamPlayback_target, Current)                        \
  R(EvaluateLogicParams, ParamPlayback_rampRate, Current)                      \
  /* 4 RunEventLogic: Playback, switch values, fire list (music_if) */         \
  R(RunEventLogic, Playback_eventRef, Current)                                 \
  R(RunEventLogic, Playback_seed, Current)                                     \
  R(RunEventLogic, Playback_emitterSlot, Current)                              \
  R(RunEventLogic, Playback_transportSlot, Current)                            \
  R(RunEventLogic, Playback_request, Current)                                  \
  R(RunEventLogic, Playback_eventId, Current)                                  \
  R(RunEventLogic, Playback_dynamicSource, Current)                            \
  R(RunEventLogic, ParamGlobal_paramId, Current)                               \
  R(RunEventLogic, ParamGlobal_current, Current)                               \
  R(RunEventLogic, ParamEmitter_paramId, Current)                              \
  R(RunEventLogic, ParamEmitter_nextSlot, Current)                             \
  R(RunEventLogic, ParamEmitter_current, Current)                              \
  R(RunEventLogic, ParamPlayback_paramId, Current)                             \
  R(RunEventLogic, ParamPlayback_nextSlot, Current)                            \
  R(RunEventLogic, ParamPlayback_current, Current)                             \
  R(RunEventLogic, Transport_position, Current)                                \
  R(RunEventLogic, Transport_state, Current)                                   \
  R(RunEventLogic, Category_categoryId, Current)                               \
  R(RunEventLogic, Category_request, Current)                                  \
  R(RunEventLogic, Category_flags, Current)                                    \
  R(RunEventLogic, Voice_renderStatus, Previous)                               \
  R(RunEventLogic, Voice_playhead, Previous)                                   \
  R(RunEventLogic, Voice_realSlot, Previous)                                   \
  R(RunEventLogic, Voice_renderHold, Previous) /* 1.1: no destroy while held */ \
  R(RunEventLogic, Stream_readPos, Async)                                      \
  R(RunEventLogic, Stream_ringWrite, Async)                                    \
  /* 5 Compute3D: Emitter (extrapolated/interpolated), Listener */             \
  R(Compute3D, Emitter_position, Current)                                      \
  R(Compute3D, Emitter_velocity, Current)                                      \
  R(Compute3D, Emitter_forward, Current)                                       \
  R(Compute3D, Emitter_lastUpdateBlock, Current)                               \
  R(Compute3D, Listener_position, Current)                                     \
  R(Compute3D, Listener_velocity, Current)                                     \
  R(Compute3D, Listener_forward, Current)                                      \
  R(Compute3D, Listener_up, Current)                                           \
  R(Compute3D, Listener_weight, Current)                                       \
  R(Compute3D, Listener_enabled, Current)                                      \
  /* 6 EvaluateDspParams: ParamValue, Spatial, previous bus gain */            \
  R(EvaluateDspParams, ParamGlobal_paramId, Current)                           \
  R(EvaluateDspParams, ParamGlobal_current, Current)                           \
  R(EvaluateDspParams, ParamEmitter_paramId, Current)                          \
  R(EvaluateDspParams, ParamEmitter_nextSlot, Current)                         \
  R(EvaluateDspParams, ParamEmitter_current, Current)                          \
  R(EvaluateDspParams, ParamPlayback_paramId, Current)                         \
  R(EvaluateDspParams, ParamPlayback_nextSlot, Current)                        \
  R(EvaluateDspParams, ParamPlayback_current, Current)                         \
  R(EvaluateDspParams, Spatial_distance, Current)                              \
  R(EvaluateDspParams, Spatial_direction, Current)                             \
  R(EvaluateDspParams, Spatial_dopplerRatio, Current)                          \
  R(EvaluateDspParams, Spatial_coneCos, Current)                               \
  R(EvaluateDspParams, Spatial_valid, Current)                                 \
  R(EvaluateDspParams, Emitter_occlusion, Current)                             \
  R(EvaluateDspParams, Voice_playbackSlot, Current)                            \
  R(EvaluateDspParams, Voice_busSlot, Current)                                 \
  R(EvaluateDspParams, Voice_emitterSlot, Current)                             \
  R(EvaluateDspParams, Voice_lifecycle, Current)                               \
  R(EvaluateDspParams, Voice_clipRef, Current)                                 \
  R(EvaluateDspParams, Playback_state, Current)                                \
  R(EvaluateDspParams, Playback_logic, Current)                                \
  R(EvaluateDspParams, Playback_eventRef, Current)                             \
  R(EvaluateDspParams, Bus_parentSlot, Current)                                \
  R(EvaluateDspParams, Bus_graphOrder, Current)                                \
  R(EvaluateDspParams, Bus_volume, Current)                                    \
  R(EvaluateDspParams, Bus_gain, Previous)                                     \
  R(EvaluateDspParams, Bus_peak, Previous)                                     \
  R(EvaluateDspParams, Category_categoryId, Current)                           \
  R(EvaluateDspParams, Category_request, Current)                              \
  R(EvaluateDspParams, Snapshot_snapshotId, Current)                           \
  R(EvaluateDspParams, Snapshot_request, Current)                              \
  /* 7 SelectVoices: priority, audibility (uses previous bus gain/ducking) */  \
  R(SelectVoices, Voice_priority, Current)                                     \
  R(SelectVoices, Voice_audibility, Current)                                   \
  R(SelectVoices, Voice_limitGroup, Current)                                   \
  R(SelectVoices, Voice_lifecycle, Current)                                    \
  R(SelectVoices, Voice_playbackSlot, Current)                                 \
  R(SelectVoices, Voice_sourceKind, Current)                                   \
  R(SelectVoices, Voice_playhead, Previous)                                    \
  R(SelectVoices, Bus_gain, Previous)                                          \
  R(SelectVoices, Voice_renderHold, Previous)  /* 1.1 D-43 */                  \
  /* 8 BuildRenderList: RealVoice, Voice */                                    \
  R(BuildRenderList, RealVoice_voiceSlot, Current)                             \
  R(BuildRenderList, RealVoice_assign, Current)                                \
  R(BuildRenderList, Voice_sourceKind, Current)                                \
  R(BuildRenderList, Voice_sourceSlot, Current)                                \
  R(BuildRenderList, Voice_busSlot, Current)                                   \
  R(BuildRenderList, Voice_realSlot, Current)                                  \
  R(BuildRenderList, Voice_lifecycle, Current)                                 \
  R(BuildRenderList, Bus_graphOrder, Current)                                  \
  R(BuildRenderList, Voice_renderHold, Previous)  /* 1.1 D-43 */               \
  /* 9 Render: render list, sources, DSP state */                              \
  R(Render, Voice_sourceKind, Current)                                         \
  R(Render, Voice_sourceSlot, Current)                                         \
  R(Render, Voice_busSlot, Current)                                            \
  R(Render, Voice_emitterSlot, Current)                                        \
  R(Render, Voice_lifecycle, Current)                                          \
  R(Render, Voice_pitch, Current)                                              \
  R(Render, Voice_gain, Current)                                               \
  R(Render, Voice_filterTarget, Current)                                       \
  R(Render, Voice_realSlot, Current)                                           \
  R(Render, Voice_clipRef, Current)                                            \
  R(Render, RealVoice_voiceSlot, Current)                                      \
  R(Render, RealVoice_assign, Current)                                         \
  R(Render, Spatial_distance, Current)                                         \
  R(Render, Spatial_direction, Current)                                        \
  R(Render, Spatial_valid, Current)                                            \
  R(Render, Bus_parentSlot, Current)                                           \
  R(Render, Bus_sendRange, Current)                                            \
  R(Render, Bus_effectRange, Current)                                          \
  R(Render, Bus_graphOrder, Current)                                           \
  R(Render, Bus_gainTarget, Current)                                           \
  R(Render, Bus_effectBypass, Current)                                         \
  R(Render, Stream_control, Current)                                           \
  R(Render, Stream_ringRange, Current)                                         \
  R(Render, Stream_ringWrite, Async)                                           \
  /* 10 Publish: everything that is published */                               \
  R(Publish, Playback_state, Current)                                          \
  R(Publish, Playback_schedule, Current)                                       \
  R(Publish, Playback_logic, Current)                                          \
  R(Publish, Playback_endInfo, Current)                                        \
  R(Publish, Playback_eventId, Current)                                        \
  R(Publish, Playback_transportSlot, Current)                                  \
  R(Publish, Voice_realSlot, Current)                                          \
  R(Publish, Voice_lifecycle, Current)                                         \
  R(Publish, Voice_playbackSlot, Current)                                      \
  R(Publish, Bus_busId, Current)                                               \
  R(Publish, Bus_gain, Current)                                                \
  R(Publish, Bus_peak, Current)                                                \
  R(Publish, Transport_position, Current)                                      \
  R(Publish, Transport_state, Current)                                         \
  R(Publish, Transport_signature, Current)                                     \
  R(Publish, Stream_control, Current)                                          \
  R(Publish, Stream_stallCount, Current)

// Per-block data that is not a table (owner = only writer).
#define SND_SCHEMA_FRAME_DATA(F)                                                  \
  F(FireList, UpdateTransport, "reservations that fire this block (music_if.hpp)") \
  F(MusicVoiceRequests, RunEventLogic, "voice start/stop requests from music logic") \
  F(RenderList, BuildRenderList, "sorted by bus, then source kind (source_if.hpp); 2 sets, alternating per block (D-43)") \
  F(ChunkAccumulators, Render, "per-chunk per-bus sums, added in chunk order (3.4); 2 sets with the render list (D-43)") \
  F(TripleBuffer, Publish, "game-visible snapshot (notify.hpp)")                   \
  F(EndFlags, Publish, "per-playback end flags (notify.hpp)")                      \
  F(EventRing, Publish, "beats, markers, logs (notify.hpp)")

// ===========================================================================
// Generated definitions
// ===========================================================================
enum class TableId : std::uint8_t {
#define SND_TABLE_ENUM(T, idx, cr, de, cap) T,
  SND_SCHEMA_TABLES(SND_TABLE_ENUM)
#undef SND_TABLE_ENUM
  Count
};

enum class ColumnId : std::uint16_t {
#define SND_COLUMN_ENUM(T, name, type, temp, writer) T##_##name,
  SND_SCHEMA_ALL_COLUMNS(SND_COLUMN_ENUM)
#undef SND_COLUMN_ENUM
  Count
};

enum class FrameDataId : std::uint8_t {
#define SND_FRAME_ENUM(name, owner, doc) name,
  SND_SCHEMA_FRAME_DATA(SND_FRAME_ENUM)
#undef SND_FRAME_ENUM
  Count
};

struct TableInfo {
  TableId id;
  const char* name;
  Indexing indexing;
  SystemId rowCreator;
  SystemId rowDestroyer;
  const char* capacity;
};

inline constexpr TableInfo kTables[] = {
#define SND_TABLE_INFO(T, idx, cr, de, cap) \
  TableInfo{TableId::T, #T, Indexing::idx, SystemId::cr, SystemId::de, cap},
    SND_SCHEMA_TABLES(SND_TABLE_INFO)
#undef SND_TABLE_INFO
};
static_assert(sizeof(kTables) / sizeof(kTables[0]) == static_cast<std::size_t>(TableId::Count));

struct ColumnInfo {
  ColumnId id;
  TableId table;
  const char* name;  // "Table.column"
  std::uint32_t elemBytes;
  std::uint32_t elemAlign;
  Temperature temperature;
  SystemId writer;
  bool atomic;
};

template <class T>
inline constexpr bool kIsAtomic = false;
template <class T>
inline constexpr bool kIsAtomic<std::atomic<T>> = true;

inline constexpr ColumnInfo kColumns[] = {
#define SND_COLUMN_INFO(T, name, type, temp, writer)                                        \
  ColumnInfo{ColumnId::T##_##name, TableId::T,          #T "." #name,                       \
             static_cast<std::uint32_t>(sizeof(type)), static_cast<std::uint32_t>(alignof(type)), \
             Temperature::temp,      SystemId::writer,    kIsAtomic<type>},
    SND_SCHEMA_ALL_COLUMNS(SND_COLUMN_INFO)
#undef SND_COLUMN_INFO
};
static_assert(sizeof(kColumns) / sizeof(kColumns[0]) == static_cast<std::size_t>(ColumnId::Count));

struct ReadInfo {
  SystemId reader;
  ColumnId column;
  ReadTiming timing;
};

inline constexpr ReadInfo kReads[] = {
#define SND_READ_INFO(S, col, timing) ReadInfo{SystemId::S, ColumnId::col, ReadTiming::timing},
    SND_SCHEMA_READS(SND_READ_INFO)
#undef SND_READ_INFO
};

struct FrameDataInfo {
  FrameDataId id;
  const char* name;
  SystemId owner;
  const char* doc;
};
inline constexpr FrameDataInfo kFrameData[] = {
#define SND_FRAME_INFO(name, owner, doc) FrameDataInfo{FrameDataId::name, #name, SystemId::owner, doc},
    SND_SCHEMA_FRAME_DATA(SND_FRAME_INFO)
#undef SND_FRAME_INFO
};

constexpr const ColumnInfo& Column(ColumnId c) noexcept {
  return kColumns[static_cast<std::size_t>(c)];
}
constexpr const TableInfo& Table(TableId t) noexcept { return kTables[static_cast<std::size_t>(t)]; }

// Returns the index of the first invalid read declaration, or -1.
constexpr int ValidateReads() noexcept {
  const std::size_t n = sizeof(kReads) / sizeof(kReads[0]);
  for (std::size_t i = 0; i < n; ++i) {
    const ReadInfo& r = kReads[i];
    const ColumnInfo& c = Column(r.column);
    if (!IsAudioThreadSystem(r.reader)) return static_cast<int>(i);
    if (c.writer == r.reader) return static_cast<int>(i);  // owners must not be listed
    switch (r.timing) {
      case ReadTiming::Current:
        if (!IsAudioThreadSystem(c.writer) || Order(c.writer) >= Order(r.reader)) return static_cast<int>(i);
        break;
      case ReadTiming::Previous:
        if (!IsAudioThreadSystem(c.writer) || Order(c.writer) < Order(r.reader)) return static_cast<int>(i);
        break;
      case ReadTiming::Async:
        if (!c.atomic) return static_cast<int>(i);
        break;
    }
    for (std::size_t j = i + 1; j < n; ++j) {
      if (kReads[j].reader == r.reader && kReads[j].column == r.column) return static_cast<int>(j);
    }
  }
  return -1;
}
static_assert(ValidateReads() == -1, "SND_SCHEMA_READS: index ValidateReads() is inconsistent");

// Every column written outside the audio thread must be atomic; every atomic
// column must be read through Async declarations only (checked above).
constexpr int ValidateColumns() noexcept {
  for (std::size_t i = 0; i < sizeof(kColumns) / sizeof(kColumns[0]); ++i) {
    const ColumnInfo& c = kColumns[i];
    if (IsExternal(c.writer) && !c.atomic) return static_cast<int>(i);
    if (!IsExternal(c.writer) && !IsAudioThreadSystem(c.writer)) return static_cast<int>(i);
    if (c.elemAlign > kBufferAlignment) return static_cast<int>(i);  // columns are 64-byte aligned arrays
  }
  return -1;
}
static_assert(ValidateColumns() == -1, "column writer / atomicity mismatch");

constexpr bool IsDeclaredRead(SystemId s, ColumnId c) noexcept {
  for (const ReadInfo& r : kReads) {
    if (r.reader == s && r.column == c) return true;
  }
  return false;
}

// ---------------------------------------------------------------------------
// Storage
// ---------------------------------------------------------------------------
// Row bookkeeping shared by all tables. The algorithms (insert, swap-remove,
// generation bump) are implemented by base (stage 1); core allocates.
struct TableRows {
  std::uint32_t capacity = 0;
  std::uint32_t count = 0;           // live rows = dense [0, count)
  SlotIndex* denseToSlot = nullptr;  // Sparse only
  std::uint32_t* slotToDense = nullptr;  // Sparse only; kInvalidSlot = free slot
  std::uint32_t* generation = nullptr;   // per slot; never 0 for a live row
};

#define SND_DECL_COLUMN_MEMBER(T, name, type, temp, writer) type* name = nullptr;
#define SND_DECL_TABLE_STRUCT(T, idx, cr, de, cap)              \
  struct T##Columns {                                           \
    SND_COLS_##T(SND_DECL_COLUMN_MEMBER)                        \
  };                                                            \
  struct T##Table {                                             \
    TableRows rows;                                             \
    T##Columns col;                                             \
  };
SND_SCHEMA_TABLES(SND_DECL_TABLE_STRUCT)
#undef SND_DECL_TABLE_STRUCT
#undef SND_DECL_COLUMN_MEMBER

struct Tables {
#define SND_DECL_TABLES_MEMBER(T, idx, cr, de, cap) T##Table T;
  SND_SCHEMA_TABLES(SND_DECL_TABLES_MEMBER)
#undef SND_DECL_TABLES_MEMBER
};

// ---------------------------------------------------------------------------
// Typed, ownership-checked access
// ---------------------------------------------------------------------------
template <ColumnId C>
struct ColumnTraits;

#define SND_COLUMN_TRAITS(T, name, type, temp, writer)                              \
  template <>                                                                       \
  struct ColumnTraits<ColumnId::T##_##name> {                                       \
    using Type = type;                                                              \
    static constexpr TableId kTable = TableId::T;                                   \
    static constexpr SystemId kWriter = SystemId::writer;                           \
    static constexpr Temperature kTemperature = Temperature::temp;                  \
    static Type* Get(Tables& t) noexcept { return t.T.col.name; }                   \
    static const Type* Get(const Tables& t) noexcept { return t.T.col.name; }       \
    static Type*& Slot(Tables& t) noexcept { return t.T.col.name; }                 \
  };
SND_SCHEMA_ALL_COLUMNS(SND_COLUMN_TRAITS)
#undef SND_COLUMN_TRAITS

template <SystemId S, ColumnId C>
inline typename ColumnTraits<C>::Type* WriteColumn(Tables& t) noexcept {
  static_assert(ColumnTraits<C>::kWriter == S,
                "column ownership violation: this system is not the column's writer (3.2)");
  return ColumnTraits<C>::Get(t);
}

template <SystemId S, ColumnId C>
inline const typename ColumnTraits<C>::Type* ReadColumn(const Tables& t) noexcept {
  static_assert(ColumnTraits<C>::kWriter == S || IsDeclaredRead(S, C),
                "undeclared read: add the column to SND_SCHEMA_READS (contract change)");
  return ColumnTraits<C>::Get(t);
}

// Row creation/destruction rights (checked at compile time by callers).
template <SystemId S, TableId T>
inline constexpr bool kCanCreateRows = Table(T).rowCreator == S;
template <SystemId S, TableId T>
inline constexpr bool kCanDestroyRows = Table(T).rowDestroyer == S;

template <TableId T>
struct TableTraits;
#define SND_TABLE_TRAITS(T, idx, cr, de, cap)                          \
  template <>                                                          \
  struct TableTraits<TableId::T> {                                     \
    using Storage = T##Table;                                          \
    static Storage& Get(Tables& t) noexcept { return t.T; }            \
    static const Storage& Get(const Tables& t) noexcept { return t.T; } \
  };
SND_SCHEMA_TABLES(SND_TABLE_TRAITS)
#undef SND_TABLE_TRAITS

template <SystemId S, TableId T>
inline TableRows& CreatorRows(Tables& t) noexcept {
  static_assert(kCanCreateRows<S, T> || kCanDestroyRows<S, T>,
                "row lifecycle violation: this system may not create or destroy rows of this table");
  return TableTraits<T>::Get(t).rows;
}

}  // namespace snd::schema

#endif  // SND_INTERNAL_SCHEMA_HPP
