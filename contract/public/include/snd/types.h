/*
 * Sonus public C types: handles, results, enums and POD descriptors.
 *
 * FROZEN CONTRACT (spec/interface/CONTRACTS.md).
 *
 * Rules for every type in this header:
 *  - POD only, fixed-width fields, explicit padding, no bitfields.
 *  - Enumerations are plain integer typedefs plus #define constants so that
 *    their size is fixed in C. The C++ API mirrors them as `enum class` with
 *    identical values (snd/sonus.hpp).
 *  - New enum values may only be appended (minor version bump).
 */
#ifndef SND_TYPES_H
#define SND_TYPES_H

#include <stddef.h>
#include <stdint.h>

#include "snd/id.h"
#include "snd/version.h"

SND_EXTERN_C_BEGIN

/* ------------------------------------------------------------------------- */
/* Limits that are part of the ABI                                            */
/* ------------------------------------------------------------------------- */
#define SND_MAX_CHANNELS 8u        /* max channels of a voice, bus or device */
#define SND_MAX_LISTENERS 4u       /* 3.1: Listener table max 4 */
#define SND_TICKS_PER_BEAT 960u    /* 8.1 */
#define SND_MAX_LANGUAGE_NAME_BYTES 32u /* 1.1: snd_declare_language, incl. the NUL */
#define SND_MAX_LANGUAGES 32u           /* 1.1: declared language names per system */

/* ------------------------------------------------------------------------- */
/* Results                                                                    */
/* ------------------------------------------------------------------------- */
typedef int32_t snd_result;
#define SND_OK                         0
#define SND_ERR_INVALID_ARGUMENT      -1  /* null pointer, out-of-range value */
#define SND_ERR_OUT_OF_BUDGET         -2  /* Create: budget cannot hold the configured limits */
#define SND_ERR_NO_DEVICE             -3  /* Create/Resume: no output device could be opened */
#define SND_ERR_WRONG_THREAD          -4  /* owner-thread-only function called elsewhere */
#define SND_ERR_THREAD_NOT_REGISTERED -5  /* recording function from an unregistered thread */
#define SND_ERR_COMMAND_BUFFER_FULL   -6  /* recording rejected: arena (and reserve) full */
#define SND_ERR_INVALID_HANDLE        -7  /* handle is 0 or already known to be stale */
#define SND_ERR_NOT_FOUND             -8  /* unknown id (bus, chunk, ...) */
#define SND_ERR_BANK_IN_USE           -9  /* UnloadBank(IF_UNUSED) while referenced, or a dependant is loaded */
#define SND_ERR_BANK_DEPENDENCY      -10  /* dependency bank not loaded */
#define SND_ERR_IO                   -11
#define SND_ERR_CORRUPT              -12  /* checksum / structure validation failed */
#define SND_ERR_VERSION_MISMATCH     -13  /* bank format / platform / contract mismatch */
#define SND_ERR_LIMIT_REACHED        -14  /* no free slot (emitters, transports, threads, ...) */
#define SND_ERR_UNSUPPORTED          -15
#define SND_ERR_NOT_IMPLEMENTED      -16  /* stage-0 stubs */
#define SND_ERR_NOT_READY            -17  /* no published snapshot yet (before the first Update) */
#define SND_ERR_BANK_ALREADY_LOADED  -18  /* 1.1: the same bank (bankId + language) is loaded or loading from another path */

/* ------------------------------------------------------------------------- */
/* Handles (3.1): 64 bit = generation (high 32) | slot (low 32).              */
/* Generation 0 is never issued, so the all-zero value is "invalid".          */
/*                                                                            */
/* Issue and return (2.1, CONTRACTS.md D-05 / D-46):                          */
/*  - Playback, Emitter, Transport (standalone), UserBuffer and Bank slots    */
/*    are issued by a lock-free slot allocator (base::HandleAllocator) on the */
/*    calling registered thread; any registered thread may issue at the same  */
/*    time and gets the handle at once.                                       */
/*  - A slot is returned only by the owner thread's snd_update(), when it     */
/*    consumes the audio side's end / release flag of that slot (notify).     */
/*  - Generations stay below 2^31 (bit 31 of the generation is never set in   */
/*    an issued handle); after 2^31 - 1 reuses of a slot they wrap to 1.      */
/* ------------------------------------------------------------------------- */
typedef struct snd_playback_handle { uint64_t value; } snd_playback_handle;
typedef struct snd_emitter_handle { uint64_t value; } snd_emitter_handle;
typedef struct snd_transport_handle { uint64_t value; } snd_transport_handle;
typedef struct snd_bank_handle { uint64_t value; } snd_bank_handle;
typedef struct snd_user_buffer_handle { uint64_t value; } snd_user_buffer_handle;

#define SND_HANDLE_MAKE(slot, gen) ((((uint64_t)(uint32_t)(gen)) << 32) | (uint64_t)(uint32_t)(slot))
#define SND_HANDLE_SLOT(v) ((uint32_t)((v) & 0xFFFFFFFFu))
#define SND_HANDLE_GENERATION(v) ((uint32_t)((v) >> 32))
#define SND_HANDLE_INVALID_VALUE ((uint64_t)0u)

/*
 * Transport owned by a music Playback (8.1). Its handle is derived from the
 * playback handle without a round trip: slot = SND_TRANSPORT_PLAYBACK_BIT |
 * playback slot, generation = playback generation. Created/destroyed together
 * with the playback. Transports from CreateTransport never set this bit.
 */
#define SND_TRANSPORT_PLAYBACK_BIT 0x80000000u

/* ------------------------------------------------------------------------- */
/* Enumerations                                                               */
/* ------------------------------------------------------------------------- */
typedef uint32_t snd_playback_state; /* 4.1 */
#define SND_PLAYBACK_STATE_NONE     0u /* invalid / stale handle / not yet known */
#define SND_PLAYBACK_STATE_PENDING  1u
#define SND_PLAYBACK_STATE_PRIMING  2u
#define SND_PLAYBACK_STATE_PLAYING  3u
#define SND_PLAYBACK_STATE_PAUSED   4u
#define SND_PLAYBACK_STATE_STOPPING 5u
#define SND_PLAYBACK_STATE_ENDED    6u

typedef uint32_t snd_end_reason; /* 4.1, 13.1 */
#define SND_END_COMPLETED  1u
#define SND_END_STOPPED    2u
#define SND_END_STOLEN     3u
#define SND_END_REJECTED   4u
#define SND_END_DISCARDED  5u /* bank unloaded, system destroyed, owner transport destroyed */
#define SND_END_ERROR      6u
#define SND_END_NOT_LOADED 7u /* event id not registered at apply time (13.1) */

typedef uint32_t snd_end_detail; /* refinement of snd_end_reason, 0 = none */
#define SND_END_DETAIL_NONE                0u
#define SND_END_DETAIL_VOICE_LIMIT         1u  /* global real/virtual voice limit, steal mode "never" */
#define SND_END_DETAIL_LIMIT_GROUP         2u  /* voice limit group */
#define SND_END_DETAIL_EVENT_LIMIT         3u  /* per-event instance limit */
#define SND_END_DETAIL_RETRIGGER_INTERVAL  4u
#define SND_END_DETAIL_COMMAND_OVERFLOW    5u  /* arena full at record time */
#define SND_END_DETAIL_NO_STREAM_SLOT      6u
#define SND_END_DETAIL_DECODER_POOL_FULL   7u
#define SND_END_DETAIL_PCM_CACHE_FULL      8u
#define SND_END_DETAIL_PLAYBACK_TABLE_FULL 9u
#define SND_END_DETAIL_BANK_UNLOADED      10u
#define SND_END_DETAIL_STREAM_IO          11u
#define SND_END_DETAIL_DECODE             12u
#define SND_END_DETAIL_RESERVATION_MISSED 13u  /* "間に合わない時" = cancel */
#define SND_END_DETAIL_TRANSPORT_GONE     14u
#define SND_END_DETAIL_CALLBACK_DEADLINE  15u
#define SND_END_DETAIL_SYSTEM_DESTROYED   16u
#define SND_END_DETAIL_WRONG_EVENT_KIND   17u  /* e.g. music API on a non-music playback */
#define SND_END_DETAIL_RESERVATION_FULL   18u

typedef uint32_t snd_bank_state;
#define SND_BANK_STATE_UNLOADED  0u
#define SND_BANK_STATE_LOADING   1u
#define SND_BANK_STATE_LOADED    2u
#define SND_BANK_STATE_FAILED    3u
#define SND_BANK_STATE_UNLOADING 4u

typedef uint32_t snd_unload_mode; /* 10.3 */
#define SND_UNLOAD_STOP_AND_UNLOAD 0u
#define SND_UNLOAD_IF_UNUSED       1u

typedef uint32_t snd_transport_state; /* 8.1 */
#define SND_TRANSPORT_STOPPED 0u
#define SND_TRANSPORT_PLAYING 1u
#define SND_TRANSPORT_PAUSED  2u

typedef uint32_t snd_synth_mode; /* 5.2 */
#define SND_SYNTH_BUFFERED    0u /* default: normal worker renders ahead into a ring */
#define SND_SYNTH_LOW_LATENCY 1u /* opt-in: called synchronously on an RT worker */

typedef uint32_t snd_user_buffer_mode; /* 5.1 */
#define SND_USER_BUFFER_STREAM 0u /* ring: game keeps writing ahead of the read position */
#define SND_USER_BUFFER_STATIC 1u /* written once (or rewritten while stopped), played like a clip */

typedef uint32_t snd_listener_mode; /* 4.5 */
#define SND_LISTENER_NEAREST  0u
#define SND_LISTENER_WEIGHTED 1u
#define SND_LISTENER_SPLIT    2u /* per-listener outputs (split screen) */

typedef uint32_t snd_focus_mode; /* 2.6 */
#define SND_FOCUS_CONTINUE     0u
#define SND_FOCUS_SILENCE      1u
#define SND_FOCUS_PER_CATEGORY 2u /* categories flagged "mute on focus loss" in the bank */

typedef uint32_t snd_log_level;
#define SND_LOG_DEBUG 0u
#define SND_LOG_INFO  1u
#define SND_LOG_WARN  2u
#define SND_LOG_ERROR 3u

/* Memory tags (3.3 区分). Used by IAllocator and by snd_stats. */
typedef uint32_t snd_mem_tag;
#define SND_MEM_TABLES        0u
#define SND_MEM_FRAME_ARENA   1u
#define SND_MEM_THREAD_ARENA  2u
#define SND_MEM_STREAM_POOL   3u
#define SND_MEM_DECODER_POOL  4u
#define SND_MEM_DSP_POOL      5u
#define SND_MEM_PCM_CACHE     6u
#define SND_MEM_BANK          7u
#define SND_MEM_COMMAND_ARENA 8u  /* per-thread command arenas + reserves */
#define SND_MEM_NOTIFY        9u  /* end flags, event ring, triple buffers */
#define SND_MEM_LIVELINK     10u
#define SND_MEM_MISC         11u
#define SND_MEM_TAG_COUNT    12u

/* ------------------------------------------------------------------------- */
/* Time and scheduling (8.1, 8.2)                                             */
/* ------------------------------------------------------------------------- */
/* Musical position. All fields 0-based: bar 0 beat 0 tick 0 is the start. */
typedef struct snd_musical_time {
  int32_t bar;
  int32_t beat;
  int32_t tick; /* 0 .. SND_TICKS_PER_BEAT-1 */
} snd_musical_time;

typedef uint32_t snd_when_kind;
#define SND_WHEN_IMMEDIATE     0u /* at the block in which the command is applied */
#define SND_WHEN_NEXT_BEAT     1u
#define SND_WHEN_NEXT_BAR      2u
#define SND_WHEN_BARS_AFTER    3u /* count = N: bar line N bars after the current bar's start (N>=1) */
#define SND_WHEN_GRID          4u /* count = note division (4 = quarter, 8 = 1/8, 16 = 1/16, 3*k triplets allowed) */
#define SND_WHEN_NEXT_MARKER   5u /* value = marker id (0 = any marker) of the transport's current segment */
#define SND_WHEN_SEGMENT_END   6u
#define SND_WHEN_CLOCK_SAMPLE  7u /* value = absolute common-clock sample (PlayScheduled) */
#define SND_WHEN_DELAY_US      8u /* value = microseconds after the apply block start (PlayDelayed) */

#define SND_WHEN_FLAG_PICKUP_ALIGN 0x01u /* align the material's pickup marker (auftakt) to the target (8.2) */

/*
 * "When" for every reservable request (8.2). 32 bytes, POD.
 * Musical kinds (NEXT_BEAT..SEGMENT_END) need a valid transport. If the
 * transport is invalid/destroyed at apply time the request is treated as
 * IMMEDIATE and an SND_EVENT_WARNING with detail SND_END_DETAIL_TRANSPORT_GONE
 * is posted.
 */
typedef struct snd_when {
  snd_transport_handle transport; /* musical kinds only */
  uint64_t value;                 /* CLOCK_SAMPLE / DELAY_US / NEXT_MARKER */
  int32_t offset_ticks;           /* signed offset after quantisation (negative = earlier) */
  uint32_t count;                 /* BARS_AFTER / GRID */
  uint8_t kind;                   /* snd_when_kind */
  uint8_t flags;                  /* SND_WHEN_FLAG_* */
  uint8_t reserved[6];            /* must be 0 */
} snd_when;
SND_STATIC_ASSERT(sizeof(snd_when) == 32, "snd_when must be 32 bytes");

typedef struct snd_vec3 {
  float x, y, z;
} snd_vec3;

typedef struct snd_listener_desc {
  snd_vec3 position;
  snd_vec3 forward; /* unit */
  snd_vec3 up;      /* unit */
  snd_vec3 velocity;
  float weight;     /* SND_LISTENER_WEIGHTED only */
  uint32_t enabled; /* 0/1 */
} snd_listener_desc;

typedef struct snd_transport_desc {
  float bpm;              /* > 0 */
  uint16_t beats_per_bar; /* time signature numerator, >= 1 */
  uint16_t beat_unit;     /* denominator: 2, 4, 8, 16 */
  uint32_t start_playing; /* 0/1: start immediately (otherwise Stopped) */
  uint32_t reserved;
} snd_transport_desc;

/* Read-only snapshot values (2.1: consistent as of the latest Update()) */
typedef struct snd_position {
  uint64_t frames;           /* playback-local frames rendered since start (internal rate) */
  double seconds;            /* frames / internal sample rate */
  snd_musical_time musical;  /* valid if has_musical */
  uint32_t has_musical;      /* playback owns or follows a transport / query on a transport */
  uint32_t reserved;
  uint64_t clock_sample;     /* common clock at the snapshot */
} snd_position;

typedef struct snd_meter {
  uint32_t channels;
  uint32_t reserved;
  float peak[SND_MAX_CHANNELS]; /* linear, max over the last published period */
  float rms[SND_MAX_CHANNELS];  /* linear */
  float loudness_short;         /* LUFS, short-term (method fixed in stage 3) */
  float loudness_integrated;    /* LUFS */
  uint64_t clock_sample;
} snd_meter;

typedef struct snd_stats {
  uint64_t block_index;      /* number of blocks rendered */
  uint64_t clock_sample;     /* common clock */
  uint32_t playbacks;
  uint32_t real_voices;
  uint32_t virtual_voices;
  uint32_t streams;
  uint32_t lod_level;        /* 0 = full quality (2.4) */
  uint32_t underruns;        /* cumulative (2.2) */
  uint32_t stream_stalls;    /* cumulative (6.3) */
  uint32_t late_chunks;      /* cumulative chunks dropped at the deadline (2.3) */
  uint32_t dropped_events;   /* event ring overflow (2.1) */
  uint32_t rejected_commands;/* recording rejected (2.1) */
  uint32_t reserve_overflows;/* reserve region exhausted; coalesced fallbacks used (CONTRACTS.md D-11) */
  uint32_t reserved0;
  float block_time_avg_us;
  float block_time_max_us;
  float block_budget_us;     /* block duration * target utilisation */
  float output_latency_s;    /* IOutputDevice::OutputLatencySeconds + render ahead */
  uint64_t memory_used[SND_MEM_TAG_COUNT];
  uint64_t memory_budget[SND_MEM_TAG_COUNT];
} snd_stats;

/* ------------------------------------------------------------------------- */
/* Dynamic generation (5)                                                     */
/* ------------------------------------------------------------------------- */
#define SND_RENDER_FLAG_LOW_LATENCY 0x01u
#define SND_RENDER_FLAG_FIRST_BLOCK 0x02u
#define SND_RENDER_FLAG_RELEASING   0x04u /* playback is Stopping */

/* Passed to synthesis callbacks (5.2) and plugin effects. */
typedef struct snd_render_context {
  uint64_t clock_sample;        /* common clock of the first frame to render */
  snd_playback_handle playback; /* owner playback (0 for bus effects) */
  const float* param_start;     /* parameter values at block start (per SynthDesc/Event param list) */
  const float* param_end;       /* values at block end; interpolate linearly per frame */
  uint32_t param_count;
  uint32_t sample_rate;         /* internal rate */
  uint32_t block_frames;        /* configured block size */
  uint32_t flags;               /* SND_RENDER_FLAG_* */
  double transport_beats;       /* beats since transport start, or -1 if none */
} snd_render_context;

typedef void (SND_CALL *snd_synth_callback)(void* user, float* const* out, uint32_t channels,
                                            uint32_t frames, const snd_render_context* ctx);
/* Generic function pointer, used to carry a C++ callback through the C ABI. */
typedef void (SND_CALL *snd_fnptr)(void);

typedef struct snd_synth_desc {
  snd_synth_callback callback; /* C callback, or NULL when cpp_callback is set */
  snd_fnptr cpp_callback;      /* reserved for the C++ wrapper (snd::SynthCallback, cast back by api) */
  void* user;
  uint32_t channels;           /* 1..SND_MAX_CHANNELS */
  snd_synth_mode mode;
  uint32_t ahead_blocks;       /* BUFFERED ring depth; 0 = Config.tuning.callbackAheadBlocks */
  uint32_t priority;           /* voice priority (0 = event template / default) */
  snd_id bus;                  /* output bus; SND_ID_NONE = master */
  snd_id event_template;       /* optional event providing routing, 3D and limits; NONE = defaults */
  float volume;                /* linear */
  uint32_t reserved;
} snd_synth_desc;

typedef struct snd_user_buffer_desc {
  float* const* channels;      /* game-owned memory, non-interleaved, valid until the buffer is released */
  uint32_t channel_count;      /* 1..SND_MAX_CHANNELS */
  uint32_t frames;             /* capacity per channel */
  uint32_t sample_rate;        /* 0 = internal rate */
  snd_user_buffer_mode mode;
  uint32_t loop;               /* STATIC: loop the written region */
  uint32_t reserved;
} snd_user_buffer_desc;

typedef struct snd_input_desc {
  uint32_t device_index;       /* platform input index (0 = default) */
  uint32_t channels;
  snd_id bus;                  /* NONE = master */
  uint32_t reserved;
} snd_input_desc;

/* ------------------------------------------------------------------------- */
/* Notifications delivered inside Update() on the owner thread (2.1, 13.1)    */
/* ------------------------------------------------------------------------- */
typedef struct snd_timing {
  uint64_t clock_sample;        /* when the moment happens on the common clock */
  double audible_delay_seconds; /* estimated time from callback delivery until it leaves the speaker (may be < 0) */
} snd_timing;

typedef struct snd_callbacks {
  void* user;
  /* Playback ended (any reason). Delivered exactly once per playback, never lost (2.1). */
  void (SND_CALL *on_end)(void* user, snd_playback_handle playback, snd_end_reason reason,
                          snd_end_detail detail);
  /* Marker crossed in a playing clip / segment. May be dropped on event ring overflow. */
  void (SND_CALL *on_marker)(void* user, snd_playback_handle playback, snd_id marker,
                             const snd_timing* timing);
  /* Beat of a transport (bar start when position->beat == 0). May be dropped on overflow. */
  void (SND_CALL *on_beat)(void* user, snd_transport_handle transport,
                           const snd_musical_time* position, const snd_timing* timing);
  /* Asynchronous error or warning not tied to a playback end. object = handle value or id. */
  void (SND_CALL *on_error)(void* user, snd_result code, uint32_t detail, uint64_t object);
  /* LoadBankAsync finished (result SND_OK or the failure). Never lost. */
  void (SND_CALL *on_bank_loaded)(void* user, snd_bank_handle bank, snd_result result);
  /* Log record; `format` is a static printf-style string, args are raw 64-bit values. */
  void (SND_CALL *on_log)(void* user, snd_log_level level, const char* format, uint64_t arg0,
                          uint64_t arg1);
} snd_callbacks;

SND_EXTERN_C_END

#endif /* SND_TYPES_H */
