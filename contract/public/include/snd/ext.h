/*
 * Extension points as C function tables (design.md 12.3).
 *
 * FROZEN CONTRACT (spec/interface/CONTRACTS.md).
 *
 * Each table mirrors one C++ interface in snd/ext/<name>.hpp (IAllocator,
 * IJobSystem, IFileSystem, ICodec, IDspEffect, IOutputDevice). The POD types
 * defined here are the single definition of the payloads; the C++ headers
 * alias them (e.g. snd::CodecHeader == snd_codec_header).
 *
 * snd_create() copies every table by value. The `user` pointers and anything
 * they reference must stay valid until snd_destroy() returns.
 */
#ifndef SND_EXT_H
#define SND_EXT_H

#include <stddef.h>
#include <stdint.h>

#include "snd/types.h"
#include "snd/version.h"

SND_EXTERN_C_BEGIN

/* Four character code, first character in the lowest byte (little-endian file order). */
#define SND_FOURCC(a, b, c, d)                                                    \
  ((uint32_t)(uint8_t)(a) | ((uint32_t)(uint8_t)(b) << 8) | ((uint32_t)(uint8_t)(c) << 16) | \
   ((uint32_t)(uint8_t)(d) << 24))

/* ========================================================================= */
/* IAllocator: called at init and from normal workers (bank loading) only.   */
/* Never from the audio thread or RT workers.                                */
/* ========================================================================= */
typedef struct snd_allocator {
  void* user;
  /* Returns memory aligned to `align` (power of two, >= 16) or NULL. Thread-safe. */
  void* (SND_CALL *allocate)(void* user, size_t bytes, size_t align, snd_mem_tag tag);
  /* p may be NULL. `tag` equals the tag passed at allocation. Thread-safe. */
  void (SND_CALL *deallocate)(void* user, void* p, snd_mem_tag tag);
} snd_allocator;

/* ========================================================================= */
/* IJobSystem: normal-worker jobs only (2.3). RT work is never submitted.    */
/* ========================================================================= */
typedef uint32_t snd_job_lane;
#define SND_JOB_LANE_PREFETCH   0u /* 先読み: stream / compressed-resident decode */
#define SND_JOB_LANE_BACKGROUND 1u /* bank validation & registration prep, IR conversion, analysis */
#define SND_JOB_LANE_COUNT      2u

typedef void (SND_CALL *snd_job_fn)(void* data);

typedef struct snd_job_desc {
  snd_job_fn fn;
  void* data;
  /* Decremented by 1 with release semantics after fn returned or was skipped.
   * Accessed atomically (std::atomic_ref / C11 atomic). May be NULL. */
  uint32_t* done_counter;
  /* If non-NULL and it atomically reads non-zero when the job is about to
   * start, fn is not called (cooperative cancel); done_counter is still
   * decremented. A running job may poll it itself. */
  const uint32_t* cancel_flag;
  uint32_t priority; /* within a lane: higher runs first, FIFO among equals */
  uint32_t reserved;
} snd_job_desc;

typedef struct snd_job_system {
  void* user;
  /* Any thread EXCEPT the audio thread and RT workers (CONTRACTS.md D-08).
   * Copies `jobs[0..count)`. Must not run a job synchronously inside submit
   * unless the implementation documents it (the deterministic mock does). */
  void (SND_CALL *submit)(void* user, const snd_job_desc* jobs, uint32_t count, snd_job_lane lane);
} snd_job_system;

/* ========================================================================= */
/* IFileSystem: called from the IO thread only (6.5).                         */
/* ========================================================================= */
typedef uint32_t snd_file_id;
#define SND_FILE_INVALID 0u

/* Chosen by the caller (IO scheduler); unique among in-flight reads of one file system. */
typedef struct snd_io_token {
  uint64_t value;
} snd_io_token;

typedef uint32_t snd_io_state;
#define SND_IO_PENDING       0u
#define SND_IO_DONE          1u /* `bytes` valid (< requested only at end of file) */
#define SND_IO_ERROR         2u
#define SND_IO_UNKNOWN_TOKEN 3u /* never issued or already retired */

typedef struct snd_io_status {
  snd_io_state state;
  uint32_t bytes;
} snd_io_status;

typedef struct snd_file_system {
  void* user;
  /* SND_FILE_INVALID on failure. Paths are UTF-8, '/' separated, relative to the FS root. */
  snd_file_id (SND_CALL *open)(void* user, const char* path);
  /* File size in bytes, UINT64_MAX if the id is invalid. */
  uint64_t (SND_CALL *size)(void* user, snd_file_id file);
  /* Start an async read into dst. Returns 1 if accepted, 0 if rejected (invalid id,
   * too many reads in flight); a rejected read must be retried later by the caller. */
  int32_t (SND_CALL *read_async)(void* user, snd_file_id file, uint64_t offset, void* dst,
                                 uint32_t bytes, snd_io_token token);
  /* DONE/ERROR are reported once; the token is retired afterwards. */
  snd_io_status (SND_CALL *poll)(void* user, snd_io_token token);
  /* No reads may be in flight on `file`. */
  void (SND_CALL *close)(void* user, snd_file_id file);
} snd_file_system;

/* ========================================================================= */
/* ICodec: normal workers (prefetch lane). State comes from the decoder pool. */
/* ========================================================================= */
#define SND_CODEC_PCM_F32 SND_FOURCC('P', 'C', 'M', 'F')
#define SND_CODEC_PCM_S16 SND_FOURCC('P', 'C', '1', '6')
#define SND_CODEC_ADPCM   SND_FOURCC('A', 'D', 'P', 'C') /* IMA ADPCM, block-seekable */
#define SND_CODEC_OPUS    SND_FOURCC('O', 'P', 'U', 'S')

#define SND_DECODE_ERROR 0xFFFFFFFFu

typedef struct snd_codec_header {
  uint32_t codec;             /* SND_CODEC_* or a custom fourcc */
  uint32_t channels;
  uint32_t sample_rate;
  uint32_t frames_per_packet; /* 0 = variable */
  uint64_t total_frames;      /* playable frames (after pre-skip) */
  uint32_t pre_skip;          /* frames to drop at the very start (encoder delay, 6.4) */
  uint32_t pre_roll;          /* frames to decode before a seek target for convergence */
  uint32_t block_align;       /* bytes per packet for fixed-size codecs, else 0 */
  uint32_t extra_bytes;
  const uint8_t* extra;       /* codec setup data inside bank memory (read-only) */
} snd_codec_header;

typedef struct snd_codec {
  void* user;
  uint32_t codec;    /* fourcc this table decodes */
  uint32_t reserved;
  /* Bytes of decoder state for this stream (state is 64-byte aligned). Thread-safe, pure. */
  uint32_t (SND_CALL *state_bytes)(void* user, const snd_codec_header* header);
  void (SND_CALL *init)(void* user, void* state, const snd_codec_header* header);
  /* Consumes ALL src_bytes (a partial trailing packet is kept in state) and writes
   * at most max_frames frames per channel into dst (non-interleaved). Frames that
   * do not fit are kept in state and returned by later calls; call with
   * src == NULL, src_bytes == 0 to drain. Returns frames written or SND_DECODE_ERROR.
   * pre_skip is applied by the codec on the first packet after init.
   *
   * Contract 1.1 (A1-8):
   *  - Draining (src == NULL) only returns what the state holds. It does NOT mean
   *    end of stream: the end is fixed by header.total_frames (input past the end
   *    is consumed and dropped).
   *  - The unconsumed data a state may hold is bounded. The built-in codecs hold
   *    at most 16 KiB + one packet of undecoded input; a call that would leave
   *    more returns SND_DECODE_ERROR (until the next init / seek). Other codecs
   *    define their own bound, which state_bytes() covers.
   *  - Caller's duty: when decode returns max_frames (the output was full), drain
   *    with src == NULL until it returns fewer than max_frames before passing the
   *    next input. With the built-in codecs, pass at most 16 KiB per call unless
   *    the output has room for all of it (then nothing is held). */
  uint32_t (SND_CALL *decode)(void* user, void* state, const uint8_t* src, uint32_t src_bytes,
                              float* const* dst, uint32_t max_frames);
  /* Reset for decoding from a packet boundary whose first frame is `frame`.
   * The caller picks that packet from the seek table (frame <= target - pre_roll)
   * and discards the leading (target - frame) frames itself. */
  void (SND_CALL *seek)(void* user, void* state, uint64_t frame);
} snd_codec;

/* ========================================================================= */
/* IDspEffect: plugin effects, called from RT workers and the audio thread.   */
/* Must follow principle 1 (no allocation, locks, IO, formatted logging).     */
/* Stack (CONTRACTS.md D-47): the audio thread and RT workers have at least   */
/* 256 KiB of stack. Plugin code called there - process / is_tail_done and   */
/* the synthesis callbacks (snd_synth_callback, 5.2) - should use at most     */
/* about 64 KiB of it; larger scratch belongs in the state.                   */
/* ========================================================================= */
typedef struct snd_effect_desc {
  uint32_t effect_type;        /* id of the effect kind (matches snd_dsp_effect.effect_type) */
  uint32_t channels;
  uint32_t max_frames;         /* = Config.blockSize */
  uint32_t param_count;
  const float* initial_params; /* param_count values */
  const void* blob;            /* bank data, read-only (e.g. impulse response) */
  uint64_t blob_bytes;
  /* 1.1 (A1-4): rate the instance runs at (Config.sampleRate at runtime, the
   * build's DSP sizing rate in the build tool). Filled in by the runtime / build
   * tool before state_bytes and init are called; init's sample_rate argument
   * always equals this value. Plugins built against 1.0 never read it. */
  uint32_t sample_rate;
  uint32_t reserved;           /* 0 */
} snd_effect_desc;

typedef struct snd_param_block {
  const float* start;  /* values at the first frame */
  const float* end;    /* values after the last frame; ramp linearly */
  uint32_t count;
  uint32_t changed_mask; /* bit i (i<31): param i changed this block; bit 31: any param >= 31 changed */
  const snd_render_context* ctx;
} snd_param_block;

typedef struct snd_dsp_effect {
  void* user;
  uint32_t effect_type;
  uint32_t reserved;
  /* Called on a normal worker at bank load (DSP instance pool sizing) and by the
   * build tool. Pure. desc->sample_rate is set (1.1). */
  uint32_t (SND_CALL *state_bytes)(void* user, const snd_effect_desc* desc);
  /* Called on a normal worker at bank load; state is 64-byte aligned.
   * sample_rate == desc->sample_rate (1.1). */
  void (SND_CALL *init)(void* user, void* state, const snd_effect_desc* desc, uint32_t sample_rate);
  /* in and out may alias (in-place). frames <= max_frames. */
  void (SND_CALL *process)(void* user, void* state, const float* const* in, float* const* out,
                           uint32_t channels, uint32_t frames, const snd_param_block* params);
  /* Non-zero when the effect produces only silence for silent input (bypass/stop is safe, 2.4). */
  int32_t (SND_CALL *is_tail_done)(void* user, const void* state);
} snd_dsp_effect;

/* ========================================================================= */
/* IOutputDevice (2.2, 2.6)                                                   */
/* ========================================================================= */
typedef struct snd_device_desc {
  uint32_t sample_rate;   /* requested; 0 = device default */
  uint32_t channels;      /* requested; 0 = device default */
  uint32_t period_frames; /* requested; 0 = device default */
  uint32_t device_index;  /* 0 = system default */
} snd_device_desc;

/* Device thread: copy `frames` interleaved frames into `interleaved`, then wake
 * the audio thread. Must not compute (2 / thread table). */
typedef void (SND_CALL *snd_device_callback)(void* user, float* interleaved, uint32_t frames,
                                             uint32_t channels);

typedef uint32_t snd_device_event_kind;
#define SND_DEVICE_EVENT_CHANGED      1u /* route/format change (headphones), new values in the event */
#define SND_DEVICE_EVENT_SUSPENDED    2u
#define SND_DEVICE_EVENT_RESUMED      3u
#define SND_DEVICE_EVENT_LOST         4u
#define SND_DEVICE_EVENT_FOCUS_LOST   5u
#define SND_DEVICE_EVENT_FOCUS_GAINED 6u

typedef struct snd_device_event {
  snd_device_event_kind kind;
  uint32_t channels;
  uint32_t sample_rate;
  uint32_t period_frames;
} snd_device_event;

/* Any thread (device / OS notification thread); must only post, never block. */
typedef void (SND_CALL *snd_device_event_fn)(void* user, const snd_device_event* event);

typedef struct snd_output_device {
  void* user;
  /* Owner thread. Returns 1 on success. The callback starts after start(). Devices
   * without a native callback drive `cb` from their own timer thread. */
  int32_t (SND_CALL *open)(void* user, const snd_device_desc* desc, snd_device_callback cb,
                           void* cb_user);
  int32_t (SND_CALL *start)(void* user); /* 1 on success */
  void (SND_CALL *stop)(void* user);     /* returns after the last callback finished */
  void (SND_CALL *close)(void* user);
  uint32_t (SND_CALL *period_frames)(void* user); /* valid after open */
  uint32_t (SND_CALL *sample_rate)(void* user);
  uint32_t (SND_CALL *channels)(void* user);
  double (SND_CALL *output_latency_seconds)(void* user); /* 8.1 beat-notification correction */
  void (SND_CALL *set_event_listener)(void* user, snd_device_event_fn fn, void* fn_user);
} snd_output_device;

SND_EXTERN_C_END

#endif /* SND_EXT_H */
