/*
 * System configuration (design.md 13.2 + every limit and provisional "仮"
 * value in the design). FROZEN CONTRACT.
 *
 * Every scalar field is declared exactly once in the X-macro lists below.
 * The lists generate: the C structs (snake_case), snd_config_init(), the C++
 * snd::Config (camelCase, default member initialisers) and the C++ -> C
 * conversion in snd/sonus.hpp. A field therefore cannot drift between the two
 * APIs. Entry format:
 *
 *   X(c_name, cppName, c_type, cpp_type, default)
 *
 * Values marked (仮) are provisional defaults from the design; changing a
 * default is NOT a contract change (CONTRACTS.md), changing a name/type is.
 */
#ifndef SND_CONFIG_H
#define SND_CONFIG_H

#include <stdint.h>

#include "snd/ext.h"
#include "snd/types.h"
#include "snd/version.h"

SND_EXTERN_C_BEGIN

/* 1.2 (D-102): the output device snd_create opens when output.device is NULL. */
typedef uint32_t snd_output_kind;
#define SND_OUTPUT_PLATFORM 0u /* the OS device (WASAPI / Core Audio / ALSA, else the silent timer) */
#define SND_OUTPUT_SILENT   1u /* built-in silent device driven by its own timer thread in real time */
#define SND_OUTPUT_MANUAL   2u /* built-in silent device the game advances with snd_advance_output */

/* Top level ---------------------------------------------------------------- */
#define SND_CONFIG_TOP(X)                                                                         \
  /* internal sample rate (1.1) */                                                                \
  X(sample_rate, sampleRate, uint32_t, uint32_t, 48000u)                                          \
  /* frames per block (1.3), power of two, 64..1024 */                                            \
  X(block_size, blockSize, uint32_t, uint32_t, 256u)                                              \
  /* render-ahead target = device period + N blocks (2.2) */                                     \
  X(render_ahead_blocks, renderAheadBlocks, uint32_t, uint32_t, 2u)                               \
  /* requested output channels, 0 = device default */                                            \
  X(output_channels, outputChannels, uint32_t, uint32_t, 2u)                                      \
  X(max_real_voices, maxRealVoices, uint32_t, uint32_t, 128u)                                     \
  X(max_virtual_voices, maxVirtualVoices, uint32_t, uint32_t, 1024u)                              \
  /* extra real-voice slots used only for steal fades (4.4, 仮) */                                \
  X(fade_reserve_voices, fadeReserveVoices, uint32_t, uint32_t, 8u)                               \
  X(max_playbacks, maxPlaybacks, uint32_t, uint32_t, 1024u)                                       \
  X(max_emitters, maxEmitters, uint32_t, uint32_t, 512u)                                          \
  /* 1..SND_MAX_LISTENERS */                                                                      \
  X(max_listeners, maxListeners, uint32_t, uint32_t, 4u)                                          \
  /* standalone transports + transports owned by music playbacks (8.1) */                         \
  X(max_transports, maxTransports, uint32_t, uint32_t, 8u)                                        \
  X(max_reservations, maxReservations, uint32_t, uint32_t, 256u)                                  \
  X(max_streams, maxStreams, uint32_t, uint32_t, 32u)                                             \
  X(max_banks, maxBanks, uint32_t, uint32_t, 64u)                                                 \
  X(max_user_buffers, maxUserBuffers, uint32_t, uint32_t, 32u)                                    \
  /* Global-scope ParamValue rows (emitter/playback scopes: 8 inline + overflow, 3.1) */          \
  X(max_global_params, maxGlobalParams, uint32_t, uint32_t, 256u)                                 \
  /* overflow blocks of 16 ParamValue entries shared by emitter/playback scopes (3.1) */          \
  X(param_overflow_blocks, paramOverflowBlocks, uint32_t, uint32_t, 256u)                         \
  /* audio -> game event ring entries (2.1) */                                                    \
  X(event_ring_capacity, eventRingCapacity, uint32_t, uint32_t, 1024u)                            \
  /* owner thread included */                                                                     \
  X(max_registered_threads, maxRegisteredThreads, uint32_t, uint32_t, 8u)                         \
  X(listener_mode, listenerMode, snd_listener_mode, ::snd::ListenerMode, SND_LISTENER_NEAREST)    \
  X(focus_mode, focusMode, snd_focus_mode, ::snd::FocusMode, SND_FOCUS_CONTINUE)                  \
  /* base seed; playback seeds derive from it and the playback sequence (2.5) */                  \
  X(seed, seed, uint32_t, uint32_t, 0x5EED5EEDu)

/* Top level, added in contract 1.1. Stored at the END of snd_config (after
 * `plugins`) so that the 1.0 layout is a prefix (struct_size, CONTRACTS.md §2):
 * snd_create treats a config whose struct_size ends before these fields as
 * having the defaults. In the C++ snd::Config they sit with the other
 * top-level fields. Out-of-range values make snd_create fail with
 * SND_ERR_INVALID_ARGUMENT. ------------------------------------------------ */
#define SND_CONFIG_TOP_1_1(X)                                                                     \
  /* largest channel count of a voice, 1..SND_MAX_CHANNELS; sizes core's resampler pool (D-44) */ \
  X(max_voice_channels, maxVoiceChannels, uint32_t, uint32_t, 8u)                                 \
  /* largest rate ratio a voice is resampled at (source frames per output frame, including the */ \
  /* source/internal rate ratio), 1.0..SND_MAX_PITCH_RATIO; larger ratios are clamped to it.    */ \
  /* NaN, infinities, values below 1 or above the bound: snd_create SND_ERR_INVALID_ARGUMENT.  */ \
  /* Sizes core's resampler pool (D-44); ratios above 4 use ~32 KiB of stack (D-47).           */ \
  X(max_pitch_ratio, maxPitchRatio, float, float, 4.0f)

/* Top level, added in contract 1.2 (D-92). Stored at the END of snd_config (after the
 * 1.1 fields): a config whose struct_size ends before them (a 1.0 / 1.1 caller) gets the
 * defaults. Capacities of the objects core unifies by id across loaded banks (A1-16):
 * distinct ids of the whole project loaded at once. 1..65535 each (0 or above 65535:
 * snd_create SND_ERR_INVALID_ARGUMENT). max_buses does not count dev_reserve.buses. */
#define SND_CONFIG_TOP_1_2(X)                                                                     \
  X(max_buses, maxBuses, uint32_t, uint32_t, 128u)                                                \
  /* + 1 row for "all categories" (D-30) */                                                       \
  X(max_categories, maxCategories, uint32_t, uint32_t, 64u)                                       \
  X(max_snapshots, maxSnapshots, uint32_t, uint32_t, 64u)                                         \
  /* voice limit groups (VLGR ids) */                                                             \
  X(max_limit_groups, maxLimitGroups, uint32_t, uint32_t, 64u)                                    \
  /* parameter definitions (PARM ids) */                                                          \
  X(max_param_defs, maxParamDefs, uint32_t, uint32_t, 512u)                                       \
  X(max_duck_rules, maxDuckRules, uint32_t, uint32_t, 64u)

/* Top level, added in contract 1.2 for hosts (D-102, D-103). Stored at the END of snd_config
 * (after the D-92 capacities): a config whose struct_size ends before them (a caller built
 * against the earlier 1.2 header, or 1.0 / 1.1) gets the defaults.
 *  - file_system_root: root directory of the built-in file system (snd_create's file_system
 *    = NULL). UTF-8; absolute, or relative to the working directory at snd_create (resolved
 *    once there). NULL or "" = the working directory. Bank and stream paths stay relative to
 *    it: absolute paths, '\\', ':' and ".." that would leave the root are refused, and links
 *    whose target lies outside the root are not followed. A root that is not an existing
 *    directory makes snd_create fail with SND_ERR_NOT_FOUND; a root given together with a
 *    game file system is SND_ERR_INVALID_ARGUMENT. Copied by snd_create.
 *  - output_kind: SND_OUTPUT_* (above). Used only when output.device is NULL; another value
 *    than SND_OUTPUT_PLATFORM together with a game device, or an unknown value, is
 *    SND_ERR_INVALID_ARGUMENT. The built-in silent devices run at sample_rate with
 *    output_channels (0 = 2) channels and periods of block_size frames. */
#define SND_CONFIG_TOP_1_2_HOST(X)                                                                \
  X(file_system_root, fileSystemRoot, const char*, const char*, NULL)                             \
  X(output_kind, outputKind, snd_output_kind, ::snd::OutputKind, SND_OUTPUT_PLATFORM)

/* Top level, added in stage 3 (coverage, D-132; contract minor, version not bumped yet).
 * Stored at the END of snd_config (after the 1.2 host fields): a shorter struct gets the
 * defaults. Heavy voice effects (7 "再生単位のフィルタ"): a fixed pool allocated at
 * snd_create (MemTag::DspPool) of max_voice_effects slots of voice_effect_bytes each.
 *  - max_voice_effects: real voices that can run heavy voice effects at once (clamped to
 *    max_real_voices + fade_reserve_voices). A voice without a free slot plays dry.
 *  - voice_effect_chain: stages per voice, 0..4 (above 4: SND_ERR_INVALID_ARGUMENT).
 *    0 disables the configuration API (snd_set_event_voice_effect: INVALID_ARGUMENT).
 *  - voice_effect_bytes: effect state per voice (all its stages, 64-byte granularity),
 *    at most 64 MiB. The default holds a 48 kHz reverb (about 132 KiB) and a filter.
 *  - voice_effect_settings: entries of the Event configuration table and of the
 *    per-playback value table, 1..65535. */
#define SND_CONFIG_TOP_S3_VOICE_FX(X)                                                             \
  X(max_voice_effects, maxVoiceEffects, uint32_t, uint32_t, 4u)                                   \
  X(voice_effect_chain, voiceEffectChain, uint32_t, uint32_t, 2u)                                 \
  X(voice_effect_bytes, voiceEffectBytes, uint32_t, uint32_t, (160u << 10))                       \
  X(voice_effect_settings, voiceEffectSettings, uint32_t, uint32_t, 64u)

/* Memory budgets (3.3) ----------------------------------------------------- */
#define SND_CONFIG_MEMORY(X)                                                                      \
  X(stream_pool_bytes, streamPoolBytes, uint64_t, uint64_t, (8ull << 20))                         \
  /* fixed chunk size of the stream rings (6) */                                                  \
  X(stream_chunk_bytes, streamChunkBytes, uint32_t, uint32_t, (32u << 10))                        \
  X(pcm_cache_bytes, pcmCacheBytes, uint64_t, uint64_t, (16ull << 20))                            \
  /* PCM cache page (6.2, 仮) */                                                                  \
  X(pcm_cache_page_bytes, pcmCachePageBytes, uint32_t, uint32_t, (64u << 10))                     \
  /* serial-section scratch (logic) */                                                            \
  X(frame_arena_bytes, frameArenaBytes, uint32_t, uint32_t, (256u << 10))                         \
  /* per RT thread (RT workers + audio thread) scratch for the parallel render */                 \
  X(thread_arena_bytes, threadArenaBytes, uint32_t, uint32_t, (256u << 10))                       \
  /* decoder pool = max_decoders slots of decoder_slot_bytes (CONTRACTS.md D-14). 64 KiB (D-58):   \
     Opus is about 40 KiB at 1ch and 52 KiB at 2ch; Opus with 3ch or more needs 192 KiB or more.  \
     PCM / ADPCM (<= 1024 B/ch blocks) fit 32 KiB. */                                             \
  X(max_decoders, maxDecoders, uint32_t, uint32_t, 64u)                                           \
  X(decoder_slot_bytes, decoderSlotBytes, uint32_t, uint32_t, (64u << 10))                        \
  /* per-voice PCM ring for compressed-resident samples and Opus frame slack (6.2, 6.4) */        \
  X(voice_ring_frames, voiceRingFrames, uint32_t, uint32_t, 4096u)                                \
  /* DSP instance pool spare on top of what banks declare (3.3) */                                \
  X(dsp_pool_spare_bytes, dspPoolSpareBytes, uint64_t, uint64_t, (256ull << 10))

/* Command recording (2.1) ---------------------------------------------------- */
#define SND_CONFIG_COMMANDS(X)                                                                    \
  /* bytes per arena, normal region (8-byte granularity) */                                       \
  X(arena_bytes, arenaBytes, uint32_t, uint32_t, (64u << 10))                                     \
  /* arenas per registered thread (2.1: 3) */                                                     \
  X(arenas_per_thread, arenasPerThread, uint32_t, uint32_t, 3u)                                   \
  /* reserved region per arena for must-deliver commands */                                      \
  X(reserve_bytes, reserveBytes, uint32_t, uint32_t, (8u << 10))                                  \
  /* plays of events with priority >= this may use the reserve (CONTRACTS.md D-10) */             \
  X(critical_priority, criticalPriority, uint32_t, uint32_t, 224u)                                \
  /* coalescing table for parameter commands when the reserve is exhausted (D-11) */              \
  X(overflow_param_slots, overflowParamSlots, uint32_t, uint32_t, 256u)

/* Threads and jobs (2.3) ----------------------------------------------------- */
#define SND_CONFIG_JOBS(X)                                                                        \
  X(realtime_workers, realtimeWorkers, uint32_t, uint32_t, 2u)                                    \
  /* ignored when an external job system is given */                                              \
  X(normal_workers, normalWorkers, uint32_t, uint32_t, 2u)                                        \
  /* render-list chunk granularity in voices (2.3) */                                             \
  X(voices_per_chunk, voicesPerChunk, uint32_t, uint32_t, 16u)

/* Tuning: provisional (仮) values from the design ---------------------------- */
#define SND_CONFIG_TUNING(X)                                                                      \
  /* spin wait for in-flight chunks at the deadline (2.3) */                                      \
  X(rt_spin_microseconds, rtSpinMicroseconds, uint32_t, uint32_t, 50u)                            \
  /* block budget = block duration * this percent (2.4) */                                        \
  X(load_target_percent, loadTargetPercent, uint32_t, uint32_t, 60u)                              \
  X(lod_down_blocks, lodDownBlocks, uint32_t, uint32_t, 8u)                                       \
  X(lod_up_blocks, lodUpBlocks, uint32_t, uint32_t, 64u)                                          \
  /* emitter extrapolation when Update() stalls (2.1) */                                          \
  X(emitter_extrapolate_ms, emitterExtrapolateMs, uint32_t, uint32_t, 100u)                       \
  /* max catch-up blocks after an underrun (2.2) */                                               \
  X(underrun_catchup_blocks, underrunCatchupBlocks, uint32_t, uint32_t, 2u)                       \
  /* Buffered synth callback ring depth (5.2) */                                                  \
  X(callback_ahead_blocks, callbackAheadBlocks, uint32_t, uint32_t, 4u)                           \
  /* stream preload chunk length embedded in banks (6.1) */                                       \
  X(stream_preload_ms, streamPreloadMs, uint32_t, uint32_t, 100u)                                 \
  /* decode-ahead target at pitch 1.0 (6.1) */                                                    \
  X(stream_target_ahead_ms, streamTargetAheadMs, uint32_t, uint32_t, 200u)                        \
  /* IO bandwidth budget per 100 ms (6.1) */                                                      \
  X(io_bytes_per_100ms, ioBytesPer100ms, uint32_t, uint32_t, (4u << 20))                          \
  /* real-voice replacement hysteresis (4.4) */                                                   \
  X(steal_hysteresis_db, stealHysteresisDb, float, float, 3.0f)                                   \
  /* "short fade" used by steal, stall, late chunk, unload (CONTRACTS.md D-15) */                 \
  X(short_fade_ms, shortFadeMs, float, float, 5.0f)

/* Development reserve for live structural edits (11.2) ------------------------ */
#define SND_CONFIG_DEV_RESERVE(X)                                                                 \
  X(buses, buses, uint32_t, uint32_t, 16u)                                                        \
  X(effects, effects, uint32_t, uint32_t, 32u)                                                    \
  X(events, events, uint32_t, uint32_t, 64u)                                                      \
  /* writable override copies of bank tables + extra DSP state */                                 \
  X(bytes, bytes, uint64_t, uint64_t, (1ull << 20))

/* Live link (11.2); ignored when built with SND_LIVELINK=0 -------------------- */
#define SND_CONFIG_LIVELINK(X)                                                                    \
  X(enabled, enabled, uint32_t, uint32_t, 0u)                                                     \
  X(port, port, uint32_t, uint32_t, 27460u)                                                       \
  /* profiler meter rate (about 30 Hz) */                                                         \
  X(meter_hz, meterHz, uint32_t, uint32_t, 30u)                                                   \
  X(send_buffer_bytes, sendBufferBytes, uint32_t, uint32_t, (256u << 10))                         \
  /* record commands for replay (11.3) */                                                         \
  X(record_commands, recordCommands, uint32_t, uint32_t, 0u)

#define SND_CONFIG_C_FIELD(c, cpp, ct, cppt, d) ct c;

typedef struct snd_memory_config { SND_CONFIG_MEMORY(SND_CONFIG_C_FIELD) } snd_memory_config;
typedef struct snd_commands_config { SND_CONFIG_COMMANDS(SND_CONFIG_C_FIELD) } snd_commands_config;
typedef struct snd_jobs_config {
  SND_CONFIG_JOBS(SND_CONFIG_C_FIELD)
  /* Optional host job system for NORMAL worker jobs only (2.3). NULL = own workers. */
  const snd_job_system* external;
} snd_jobs_config;
typedef struct snd_tuning_config { SND_CONFIG_TUNING(SND_CONFIG_C_FIELD) } snd_tuning_config;
typedef struct snd_dev_reserve_config { SND_CONFIG_DEV_RESERVE(SND_CONFIG_C_FIELD) } snd_dev_reserve_config;
typedef struct snd_livelink_config { SND_CONFIG_LIVELINK(SND_CONFIG_C_FIELD) } snd_livelink_config;

typedef struct snd_output_config {
  /* NULL = the built-in device chosen by output_kind (1.2, D-102; default: the platform's).
   * Tests pass the null device mock. */
  const snd_output_device* device;
  uint32_t device_index;
  uint32_t reserved;
} snd_output_config;

typedef struct snd_plugin_config {
  const snd_codec* codecs; /* custom codecs; override built-ins with the same fourcc */
  uint32_t codec_count;
  uint32_t effect_count;
  const snd_dsp_effect* effects; /* plugin effects (12.3 IDspEffect) */
} snd_plugin_config;

typedef struct snd_config {
  uint32_t struct_size;      /* = sizeof(snd_config); set by snd_config_init */
  uint32_t contract_version; /* = SND_CONTRACT_VERSION; set by snd_config_init */
  SND_CONFIG_TOP(SND_CONFIG_C_FIELD)
  snd_memory_config memory;
  snd_commands_config commands;
  snd_jobs_config jobs;
  snd_tuning_config tuning;
  snd_dev_reserve_config dev_reserve;
  snd_livelink_config livelink;
  snd_output_config output;
  snd_plugin_config plugins;
  /* --- contract 1.1 --- */
  SND_CONFIG_TOP_1_1(SND_CONFIG_C_FIELD)
  /* --- contract 1.2 --- */
  SND_CONFIG_TOP_1_2(SND_CONFIG_C_FIELD)
  /* --- contract 1.2, host additions (D-102, D-103) --- */
  SND_CONFIG_TOP_1_2_HOST(SND_CONFIG_C_FIELD)
  /* --- stage 3 (coverage): voice effects (D-132) --- */
  SND_CONFIG_TOP_S3_VOICE_FX(SND_CONFIG_C_FIELD)
} snd_config;

/* Upper bound of max_pitch_ratio (= dsp kResamplerMaxRatio). */
#define SND_MAX_PITCH_RATIO 32.0f

/* Fill `cfg` with defaults. Thread: any. Always call before setting fields. */
static inline void snd_config_init(snd_config* cfg) {
  if (cfg == NULL) return;
#define SND_CONFIG_INIT_TOP(c, cpp, ct, cppt, d) cfg->c = (ct)(d);
#define SND_CONFIG_INIT_MEMORY(c, cpp, ct, cppt, d) cfg->memory.c = (ct)(d);
#define SND_CONFIG_INIT_COMMANDS(c, cpp, ct, cppt, d) cfg->commands.c = (ct)(d);
#define SND_CONFIG_INIT_JOBS(c, cpp, ct, cppt, d) cfg->jobs.c = (ct)(d);
#define SND_CONFIG_INIT_TUNING(c, cpp, ct, cppt, d) cfg->tuning.c = (ct)(d);
#define SND_CONFIG_INIT_DEV(c, cpp, ct, cppt, d) cfg->dev_reserve.c = (ct)(d);
#define SND_CONFIG_INIT_LL(c, cpp, ct, cppt, d) cfg->livelink.c = (ct)(d);
  cfg->struct_size = (uint32_t)sizeof(snd_config);
  cfg->contract_version = (uint32_t)SND_CONTRACT_VERSION;
  SND_CONFIG_TOP(SND_CONFIG_INIT_TOP)
  SND_CONFIG_TOP_1_1(SND_CONFIG_INIT_TOP)
  SND_CONFIG_TOP_1_2(SND_CONFIG_INIT_TOP)
  SND_CONFIG_TOP_1_2_HOST(SND_CONFIG_INIT_TOP)
  SND_CONFIG_TOP_S3_VOICE_FX(SND_CONFIG_INIT_TOP)
  SND_CONFIG_MEMORY(SND_CONFIG_INIT_MEMORY)
  SND_CONFIG_COMMANDS(SND_CONFIG_INIT_COMMANDS)
  SND_CONFIG_JOBS(SND_CONFIG_INIT_JOBS)
  SND_CONFIG_TUNING(SND_CONFIG_INIT_TUNING)
  SND_CONFIG_DEV_RESERVE(SND_CONFIG_INIT_DEV)
  SND_CONFIG_LIVELINK(SND_CONFIG_INIT_LL)
  cfg->jobs.external = NULL;
  cfg->output.device = NULL;
  cfg->output.device_index = 0u;
  cfg->output.reserved = 0u;
  cfg->plugins.codecs = NULL;
  cfg->plugins.codec_count = 0u;
  cfg->plugins.effect_count = 0u;
  cfg->plugins.effects = NULL;
#undef SND_CONFIG_INIT_TOP
#undef SND_CONFIG_INIT_MEMORY
#undef SND_CONFIG_INIT_COMMANDS
#undef SND_CONFIG_INIT_JOBS
#undef SND_CONFIG_INIT_TUNING
#undef SND_CONFIG_INIT_DEV
#undef SND_CONFIG_INIT_LL
}

SND_EXTERN_C_END

#endif /* SND_CONFIG_H */
