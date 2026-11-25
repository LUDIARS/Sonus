/*
 * Sonus public C API (design.md 13). FROZEN CONTRACT.
 *
 * General rules (apply to every function below unless stated otherwise):
 *
 * Threads
 *  - "owner": the thread that called snd_create(). It is registered implicitly.
 *  - "registered": the owner or any thread that called snd_register_thread().
 *    Each registered thread records into its own command arenas and SPSC
 *    queue (2.1). Order is kept per thread, not across threads.
 *  - "any": callable from any thread, including unregistered ones.
 *  A recording function called from an unregistered thread records nothing
 *  and returns SND_ERR_THREAD_NOT_REGISTERED (or an invalid handle).
 *  Owner-only functions called elsewhere return SND_ERR_WRONG_THREAD.
 *  No function may be called from the audio thread, RT workers or from inside
 *  a synth callback / plugin effect.
 *
 * Recording
 *  - Recording functions append a command and return immediately (13). The
 *    command is applied by the audio thread at a block boundary after the
 *    recording thread publishes its arena (snd_update() on the owner thread,
 *    snd_flush() on other registered threads).
 *  - Handle-returning functions allocate the slot on the calling thread and
 *    return the handle at once (2.1). They return an invalid handle (value 0)
 *    only when no slot is free or the thread is not registered.
 *    1.1 (D-46): playback / emitter / transport / user-buffer slots come from
 *    a lock-free allocator shared by all registered threads
 *    (base::HandleAllocator); a slot goes back to it only in the owner's
 *    snd_update(), when the end / release flag of the slot is consumed. Bank
 *    slots are issued by the bank module under a mutex. Issued generations
 *    are below 2^31 (see snd/types.h).
 *  - snd_play() of an event that is not loaded still returns a valid handle;
 *    the playback ends with SND_END_NOT_LOADED at a later Update (13.1).
 *  - Asynchronous outcomes (end, load complete, errors) arrive through
 *    snd_callbacks inside snd_update() on the owner thread.
 *
 * Invalid handles
 *  - Operations on a handle that is 0 or already known to be stale on the
 *    calling side record nothing and return SND_ERR_INVALID_HANDLE.
 *    Operations on a handle that becomes stale before the command is applied
 *    are ignored by the audio thread (generation mismatch, 3.1). In both cases
 *    nothing happens, as required by 13.
 *  - Query functions return SND_PLAYBACK_STATE_NONE / SND_ERR_INVALID_HANDLE.
 *
 * Errors
 *  - snd_result values are defined in snd/types.h. SND_ERR_COMMAND_BUFFER_FULL
 *    means the command was dropped at record time (2.1); play requests that
 *    are dropped this way still yield a handle which ends with
 *    SND_END_REJECTED / SND_END_DETAIL_COMMAND_OVERFLOW.
 */
#ifndef SND_SONUS_H
#define SND_SONUS_H

#include <stddef.h>
#include <stdint.h>

#include "snd/config.h"
#include "snd/ext.h"
#include "snd/id.h"
#include "snd/types.h"
#include "snd/version.h"

SND_EXTERN_C_BEGIN

typedef struct snd_system snd_system;

/* ========================================================================= */
/* System                                                                    */
/* ========================================================================= */

/* Contract version the library was built with (compare with SND_CONTRACT_VERSION). Thread: any. */
SND_API uint32_t SND_CALL snd_contract_version(void);

/*
 * Create the system. Thread: becomes the owner thread.
 * cfg: required, initialised with snd_config_init(). allocator/file_system:
 * NULL selects the built-in defaults (malloc-based allocator, OS file system).
 * All memory is allocated here (3.3).
 * 1.2 (D-102, D-103): with cfg->output.device NULL, cfg->output_kind picks the platform
 * device or a built-in silent one; with file_system NULL, cfg->file_system_root is the
 * root of the OS file system (NULL = the working directory).
 * Errors: SND_ERR_INVALID_ARGUMENT (null out, bad cfg/version/struct_size),
 * SND_ERR_OUT_OF_BUDGET, SND_ERR_NO_DEVICE, SND_ERR_NOT_FOUND (1.2: file_system_root is
 * not an existing directory). On error *out is set to NULL.
 */
SND_API snd_result SND_CALL snd_create(const snd_config* cfg, const snd_allocator* allocator,
                                       const snd_file_system* file_system, snd_system** out);

/* Stop audio, release all memory. Thread: owner. NULL is a no-op. No callbacks are delivered. */
SND_API void SND_CALL snd_destroy(snd_system* sys);

/*
 * Publish the owner thread's commands, reclaim returned arenas and slots, and
 * deliver callbacks (2.1). Thread: owner. Call once per game frame.
 * Errors: SND_ERR_WRONG_THREAD, SND_ERR_INVALID_ARGUMENT.
 */
SND_API snd_result SND_CALL snd_update(snd_system* sys);

/* Register the calling thread as a command producer. Thread: any (unregistered). Idempotent.
 * Errors: SND_ERR_LIMIT_REACHED (Config.maxRegisteredThreads). */
SND_API snd_result SND_CALL snd_register_thread(snd_system* sys);

/* Publish pending commands and unregister. Thread: registered (not the owner). */
SND_API snd_result SND_CALL snd_unregister_thread(snd_system* sys);

/* Publish the calling thread's arena without waiting for snd_update(). Thread: registered.
 * The owner normally relies on snd_update(); non-owner threads call this once per frame. */
SND_API snd_result SND_CALL snd_flush(snd_system* sys);

/* Platform lifecycle (2.6). Suspend stops the device and the common clock; resume refills
 * render-ahead before restarting. Thread: registered. Recorded like other commands. */
SND_API snd_result SND_CALL snd_suspend(snd_system* sys);
SND_API snd_result SND_CALL snd_resume(snd_system* sys);

/* 1.2 (D-102): advance the built-in manual device (Config.output_kind = SND_OUTPUT_MANUAL)
 * by `periods` device periods of Config.block_size frames, on the calling thread. Before
 * each period it waits until the audio thread has rendered its render-ahead target (at most
 * about 2 s, then the period plays what there is and the common clock catches up as after
 * an underrun, 2.2), so a test that advances slowly hears every rendered frame in order:
 * the first frame ever delivered is common-clock sample 0. Commands reach the audio thread
 * as usual (snd_update); a command recorded before the call is applied at a block that is
 * already in the render-ahead, Config.render_ahead_blocks + 1 periods later at most.
 * out: NULL, or room for periods * block_size interleaved frames of Config.output_channels
 * (0 = 2) channels, receiving what the device delivered; out_frames is its capacity in
 * frames. Thread: owner (one call at a time).
 * Errors: SND_ERR_INVALID_ARGUMENT (sys NULL, out_frames too small for a non-NULL out),
 * SND_ERR_WRONG_THREAD, SND_ERR_UNSUPPORTED (the system does not run the manual device). */
SND_API snd_result SND_CALL snd_advance_output(snd_system* sys, uint32_t periods, float* out,
                                               uint32_t out_frames);

/* Install notification callbacks (copied). NULL clears all. Thread: owner. */
SND_API snd_result SND_CALL snd_set_callbacks(snd_system* sys, const snd_callbacks* callbacks);

/* ========================================================================= */
/* Banks (10.3, 10.4)                                                        */
/* ========================================================================= */

/* Start loading; returns at once. Completion: on_bank_loaded. Loading an already loaded or
 * loading path returns the existing handle. Thread: registered. Invalid handle if no slot.
 * 1.1: the same bank (bankId + language) already loaded or loading from ANOTHER path fails
 * the new load with SND_ERR_BANK_ALREADY_LOADED (the first one stays). */
SND_API snd_bank_handle SND_CALL snd_load_bank_async(snd_system* sys, const char* path);

/* STOP_AND_UNLOAD fades out referencing playbacks first. IF_UNUSED fails with
 * SND_ERR_BANK_IN_USE (reported through on_error with object = bank handle) when referenced.
 * Thread: registered. Returns SND_OK when recorded. */
SND_API snd_result SND_CALL snd_unload_bank(snd_system* sys, snd_bank_handle bank,
                                            snd_unload_mode mode);

/* State as of the latest Update. Thread: any. Unknown handle -> SND_BANK_STATE_UNLOADED. */
SND_API snd_bank_state SND_CALL snd_get_bank_state(const snd_system* sys, snd_bank_handle bank);

/* 1.1: tell the runtime a language name (10.4). Language banks are files named
 * "<base>.<name>.sndb"; the id passed to snd_set_language is snd_hash_id(name).
 * name: 1..SND_MAX_LANGUAGE_NAME_BYTES-1 bytes of UTF-8, no '/', '.' or '\\'.
 * The system keeps one table of declared names: this call adds the name to it
 * synchronously on the calling thread (it may lock), then records a
 * must-deliver command (the name is copied). snd_set_language checks that
 * table, so it sees every name declared earlier on the same thread.
 * Declaring the same name again is SND_OK and does nothing. Thread: registered.
 * Errors: SND_ERR_INVALID_ARGUMENT (null / empty / too long / bad character, or
 * the name hashes to the id of another declared name), SND_ERR_LIMIT_REACHED
 * (SND_MAX_LANGUAGES names already declared). */
SND_API snd_result SND_CALL snd_declare_language(snd_system* sys, const char* name);

/* Switch the voice-language bank set (10.4). Playing voices keep their language. Thread: registered.
 * 1.1: `language` must be snd_hash_id() of a name given to snd_declare_language() (on any
 * thread, earlier); otherwise nothing is recorded and SND_ERR_NOT_FOUND is returned. */
SND_API snd_result SND_CALL snd_set_language(snd_system* sys, snd_id language);

/* Read-only pointer to an application chunk of a LOADED bank (e.g. 'PHYS', see
 * spec/interface/bank-format.md). Valid until the bank is unloaded. Thread: owner.
 * Errors: SND_ERR_INVALID_HANDLE, SND_ERR_NOT_FOUND, SND_ERR_NOT_READY (not loaded). */
SND_API snd_result SND_CALL snd_get_bank_chunk(const snd_system* sys, snd_bank_handle bank,
                                               uint32_t fourcc, const void** data,
                                               uint64_t* bytes);

/* ========================================================================= */
/* Playback (4, 8.2)                                                         */
/* `when` may be NULL (= immediate) for every function taking one.           */
/* ========================================================================= */

/* Thread: registered. emitter may be invalid (2D). */
SND_API snd_playback_handle SND_CALL snd_play(snd_system* sys, snd_id event,
                                              snd_emitter_handle emitter, const snd_when* when);

/* Play without a handle (4.2). Errors: THREAD_NOT_REGISTERED, COMMAND_BUFFER_FULL. */
SND_API snd_result SND_CALL snd_play_one_shot(snd_system* sys, snd_id event,
                                              snd_emitter_handle emitter, const snd_when* when);

/* One-shot at a fixed world position, no emitter (9.5). */
SND_API snd_result SND_CALL snd_play_at(snd_system* sys, snd_id event, const snd_vec3* position,
                                        const snd_when* when);

/* Stop with a fade (seconds, < 0 = the event's default). Thread: registered. Must-deliver. */
SND_API snd_result SND_CALL snd_stop(snd_system* sys, snd_playback_handle playback,
                                     float fade_seconds, const snd_when* when);

/* Pause / resume a playback, keeping its position (4.1). Thread: registered. Must-deliver. */
SND_API snd_result SND_CALL snd_pause(snd_system* sys, snd_playback_handle playback,
                                      float fade_seconds);
SND_API snd_result SND_CALL snd_resume_playback(snd_system* sys, snd_playback_handle playback,
                                                float fade_seconds);

/* Schedule the end (Unity SetScheduledEndTime). Replaces an earlier schedule. Must-deliver. */
SND_API snd_result SND_CALL snd_set_scheduled_stop(snd_system* sys, snd_playback_handle playback,
                                                   const snd_when* when, float fade_seconds);

/* 1.2 (D-104): volume and pitch of one playback, for any kind of playback (events, synths,
 * user buffers). volume is a linear gain (>= 0, at most 1000) that multiplies the gain of
 * every voice of the playback; pitch is a rate ratio (> 0, 1 = normal; clamped to
 * [1/16, 16], and with the other pitches of the voice to Config.max_pitch_ratio) that
 * multiplies their pitch. Synth voices ignore pitch (D-94). Both start at 1 for a new
 * playback, keep their value for its life and apply from the block that applies the
 * command, ramped over that block (no click). A pending playback (start `when` in the
 * future) starts with the value. Thread: registered. Must-deliver (recorded as a
 * playback-scope parameter with a reserved id, so the reserve and the coalescing fallback
 * keep the last value; D-11).
 * Errors: SND_ERR_INVALID_HANDLE, SND_ERR_INVALID_ARGUMENT (volume NaN / infinite / < 0,
 * pitch NaN / infinite / <= 0), SND_ERR_THREAD_NOT_REGISTERED, SND_ERR_COMMAND_BUFFER_FULL. */
SND_API snd_result SND_CALL snd_set_playback_volume(snd_system* sys, snd_playback_handle playback,
                                                    float volume);
SND_API snd_result SND_CALL snd_set_playback_pitch(snd_system* sys, snd_playback_handle playback,
                                                   float pitch);

/* Handle of the transport owned by a music playback (pure computation, 8.1). Thread: any.
 * The transport is valid while the playback plays a music event. */
SND_API snd_transport_handle SND_CALL snd_get_playback_transport(snd_playback_handle playback);

/* ========================================================================= */
/* 3D (4.5)                                                                  */
/* ========================================================================= */
SND_API snd_emitter_handle SND_CALL snd_create_emitter(snd_system* sys);  /* Thread: registered */
/* Playbacks on the emitter keep the last transform. Must-deliver. */
SND_API snd_result SND_CALL snd_destroy_emitter(snd_system* sys, snd_emitter_handle emitter);
/* Any pointer may be NULL to keep the previous value. Thread: registered. */
SND_API snd_result SND_CALL snd_set_transform(snd_system* sys, snd_emitter_handle emitter,
                                              const snd_vec3* position, const snd_vec3* forward,
                                              const snd_vec3* velocity);
/* index < Config.maxListeners. Thread: registered. */
SND_API snd_result SND_CALL snd_set_listener(snd_system* sys, uint32_t index,
                                             const snd_listener_desc* listener);

/* ========================================================================= */
/* Parameters and switches (4.3). All must-deliver.                          */
/* ========================================================================= */
SND_API snd_result SND_CALL snd_set_param_global(snd_system* sys, snd_id param, float value);
SND_API snd_result SND_CALL snd_set_param_emitter(snd_system* sys, snd_emitter_handle emitter,
                                                  snd_id param, float value);
SND_API snd_result SND_CALL snd_set_param_playback(snd_system* sys, snd_playback_handle playback,
                                                   snd_id param, float value);
SND_API snd_result SND_CALL snd_set_switch_global(snd_system* sys, snd_id group, snd_id value);
SND_API snd_result SND_CALL snd_set_switch_emitter(snd_system* sys, snd_emitter_handle emitter,
                                                   snd_id group, snd_id value);
SND_API snd_result SND_CALL snd_set_switch_playback(snd_system* sys, snd_playback_handle playback,
                                                    snd_id group, snd_id value);

/* ========================================================================= */
/* Mix (4.6). Volumes are linear gain. Thread: registered.                    */
/* ========================================================================= */
SND_API snd_result SND_CALL snd_set_bus_volume(snd_system* sys, snd_id bus, float volume,
                                               float fade_seconds);
SND_API snd_result SND_CALL snd_apply_snapshot(snd_system* sys, snd_id snapshot, float weight,
                                               float time_seconds);
/* category SND_ID_NONE = all categories (global pause/volume, 7章 ポーズ). */
SND_API snd_result SND_CALL snd_set_category_volume(snd_system* sys, snd_id category,
                                                    float volume, float fade_seconds);
SND_API snd_result SND_CALL snd_pause_category(snd_system* sys, snd_id category, int32_t paused);
SND_API snd_result SND_CALL snd_mute_category(snd_system* sys, snd_id category, int32_t muted);

/* ========================================================================= */
/* Music (8)                                                                 */
/* ========================================================================= */
/* `when`: NULL/IMMEDIATE = timing from the transition rule table (8.5); any
 * other kind overrides the rule's timing column. Thread: registered. */
SND_API snd_result SND_CALL snd_set_music_state(snd_system* sys, snd_playback_handle music,
                                                snd_id state, const snd_when* when);
SND_API snd_result SND_CALL snd_prepare_music_state(snd_system* sys, snd_playback_handle music,
                                                    snd_id state);
/* Standalone transport (8.1). Thread: registered. Invalid handle if none free. */
SND_API snd_transport_handle SND_CALL snd_create_transport(snd_system* sys,
                                                           const snd_transport_desc* desc);
/* Playbacks following it keep playing unquantised; pending reservations on it end with
 * SND_END_DETAIL_TRANSPORT_GONE. Must-deliver. Playback-owned transports cannot be destroyed. */
SND_API snd_result SND_CALL snd_destroy_transport(snd_system* sys, snd_transport_handle transport);
SND_API snd_result SND_CALL snd_set_transport_state(snd_system* sys, snd_transport_handle transport,
                                                    snd_transport_state state, const snd_when* when);
SND_API snd_result SND_CALL snd_set_tempo(snd_system* sys, snd_transport_handle transport,
                                          float bpm, const snd_when* when);
SND_API snd_result SND_CALL snd_seek_transport(snd_system* sys, snd_transport_handle transport,
                                               const snd_musical_time* position,
                                               const snd_when* when);
/* 1.2 (D-88): playback speed of a transport (8.1 "トランスポートのピッチ"). pitch is a rate
 * ratio (1 = normal): the transport's ticks advance pitch times faster (tempo x pitch),
 * and the voices of the music playback that owns the transport play at that pitch too
 * (tempo and pitch change together; time-stretching is stage 6). Standalone transports
 * only change their tick rate. Takes effect at `when` like snd_set_tempo; reservations
 * follow the new time line. Values are clamped to [1/16, 16]; the voice pitch is also
 * limited by Config.max_pitch_ratio. Thread: registered. Must-deliver.
 * Errors: SND_ERR_INVALID_HANDLE, SND_ERR_INVALID_ARGUMENT (pitch not finite or <= 0,
 * bad when). */
SND_API snd_result SND_CALL snd_set_transport_pitch(snd_system* sys, snd_transport_handle transport,
                                                    float pitch, const snd_when* when);
/* 1.2 (D-89): play a stinger (8.3, 8.6) of a music playback: `stinger` is the id of an entry
 * of the music event's "stingers" (bank MSTG). `when` NULL / IMMEDIATE = the stinger's own
 * timing (MSTG.timing / timingCount) on the playback's transport; any other kind overrides
 * it (like snd_set_music_state). The stinger event starts as a child of the music playback
 * at the sample of the timing point (D-76). With overlap "replace", a newer request of the
 * same stinger on the same playback cancels the older pending one. A stinger the music
 * event does not have plays nothing. No handle: it ends with the music playback.
 * Thread: registered. Must-deliver.
 * Errors: SND_ERR_INVALID_HANDLE (music), SND_ERR_INVALID_ARGUMENT (stinger 0, bad when). */
SND_API snd_result SND_CALL snd_play_stinger(snd_system* sys, snd_playback_handle music,
                                             snd_id stinger, const snd_when* when);

/* ========================================================================= */
/* Dynamic generation (5)                                                    */
/* ========================================================================= */
SND_API snd_playback_handle SND_CALL snd_play_synth(snd_system* sys, const snd_synth_desc* desc,
                                                    snd_emitter_handle emitter,
                                                    const snd_when* when);
/* Thread: registered. The sample memory in desc stays owned by the game; it must stay
 * valid until snd_get_user_buffer_released() returns 1 after snd_destroy_user_buffer(). */
SND_API snd_user_buffer_handle SND_CALL snd_create_user_buffer(snd_system* sys,
                                                               const snd_user_buffer_desc* desc);
/* Must-deliver. The game may free the memory after the next Update in which
 * snd_get_user_buffer_released() returns 1. */
SND_API snd_result SND_CALL snd_destroy_user_buffer(snd_system* sys, snd_user_buffer_handle buffer);
/* 1 once the audio side no longer references the buffer memory. Thread: any. */
SND_API int32_t SND_CALL snd_get_user_buffer_released(const snd_system* sys,
                                                      snd_user_buffer_handle buffer);
/* Copy up to `frames` frames (non-interleaved, channel_count pointers) after the write position.
 * Returns frames copied (limited by free space). Thread: ANY, but one writer per buffer at a time.
 * Lock-free; does not record a command. Invalid handle -> 0. */
SND_API uint32_t SND_CALL snd_write_user_buffer(snd_system* sys, snd_user_buffer_handle buffer,
                                                const float* const* channels, uint32_t frames);
SND_API snd_playback_handle SND_CALL snd_play_user_buffer(snd_system* sys,
                                                          snd_user_buffer_handle buffer,
                                                          snd_id bus, snd_emitter_handle emitter,
                                                          const snd_when* when);
/* 1.2 (D-104): snd_play_user_buffer with the playback's starting volume and pitch (as
 * snd_set_playback_volume / snd_set_playback_pitch; the same rules, recorded together with
 * the play so the first block already uses them). Several playbacks may read one STATIC
 * buffer at once, each with its own position, volume and pitch, without copying it (the
 * audio side only reads static memory); a STREAM buffer has one reader at a time (D-94).
 * Invalid handle also when volume or pitch is refused. Thread: registered. */
SND_API snd_playback_handle SND_CALL snd_play_user_buffer_ex(snd_system* sys,
                                                             snd_user_buffer_handle buffer,
                                                             snd_id bus, float volume, float pitch,
                                                             snd_emitter_handle emitter,
                                                             const snd_when* when);
SND_API snd_playback_handle SND_CALL snd_play_input(snd_system* sys, const snd_input_desc* desc,
                                                    snd_emitter_handle emitter,
                                                    const snd_when* when);

/* ========================================================================= */
/* Queries: values consistent as of the latest snd_update() (2.1). Thread: any. */
/* ========================================================================= */
SND_API snd_playback_state SND_CALL snd_get_playback_state(const snd_system* sys,
                                                           snd_playback_handle playback);
SND_API snd_result SND_CALL snd_get_position(const snd_system* sys, snd_playback_handle playback,
                                             snd_position* out);
SND_API snd_result SND_CALL snd_get_transport_position(const snd_system* sys,
                                                       snd_transport_handle transport,
                                                       snd_position* out);
/* bus SND_ID_NONE = master. Loudness (loudness_short / loudness_integrated) is measured for
 * the output (master) and for buses with BUSS.flags kBusMeter (a fixed number of them, in
 * the order of their bus rows); other buses report NaN there (not measured; measured
 * silence reads -200 LUFS). Peak and RMS are measured for every bus. */
SND_API snd_result SND_CALL snd_get_meter(const snd_system* sys, snd_id bus, snd_meter* out);
SND_API snd_result SND_CALL snd_get_stats(const snd_system* sys, snd_stats* out);

/* ========================================================================= */
/* Effects at run time (stage 3, coverage). Thread: registered.               */
/* ========================================================================= */
/* D-130: parameter `param_index` of effect `effect_index` (0-based, in the bus's chain)
 * of bus `bus` (SND_ID_NONE = master) moves to `value` linearly over `fade_seconds`
 * (< 0 = 0; seconds -> frames truncated, as snd_set_bus_volume), from the next block;
 * inside a block the dsp interpolates (D-26). The value is clamped to the parameter's
 * range (standard effects). Recorded as SetBusVolume variant 1. SND_ERR_INVALID_ARGUMENT:
 * effect_index >= 16, param_index >= 16, value or fade not finite. Reported when applied:
 * an unknown bus / effect / parameter (warning SND_ERR_NOT_FOUND, object = bus id); an
 * [init] parameter of a standard effect (read by its init only, e.g. MaxDelay; warning
 * SND_ERR_INVALID_ARGUMENT, object = bus id, dropped). Values set at run time belong to
 * the bus's effect instance: unloading the bank that defines the bus and loading it
 * again starts from the bank's values. */
SND_API snd_result SND_CALL snd_set_effect_param(snd_system* sys, snd_id bus, uint32_t effect_index,
                                                 uint32_t param_index, float value, float fade_seconds);
/* D-132: heavy voice effect `slot` (0 .. snd_config voice_effect_chain - 1) of the voices of
 * `event` that take a voice-effect slot after this is applied (at their start, or when
 * they come back from virtual); sounding voices keep what they have. A voice reads the
 * kind and the values together at that moment; a stage removed meanwhile is absent (no
 * warning). Voices of a nested Event use the nested Event's setting. effect_kind = a
 * standard effect id ("snd.fx.*", snd/dsp effects) or a plugin effect type
 * (Config.plugins); SND_ID_NONE removes the stage. params[0 .. param_count) override the
 * kind's defaults ([init] parameters included: read when the slot is taken; param_count
 * <= 16; params may be NULL when param_count is 0). The voice effects run per real voice
 * after the standard lowpass / highpass and before the pan, in the voice-effect pool
 * (D-133: without a free slot the voice plays dry; D-134: no tail past the voice's end).
 * SND_ERR_INVALID_ARGUMENT: event SND_ID_NONE, slot out of range, param_count > 16, a
 * non-finite value; SND_ERR_NOT_FOUND: unknown effect kind. Recorded as one SetSwitch
 * (variant 1) and param_count SetParam (variant 1), all or none
 * (SND_ERR_COMMAND_BUFFER_FULL records nothing). When applied with the Event setting
 * table (snd_config voice_effect_settings) full: warning SND_ERR_LIMIT_REACHED
 * (object = event id), dropped. */
SND_API snd_result SND_CALL snd_set_event_voice_effect(snd_system* sys, snd_id event, uint32_t slot,
                                                       snd_id effect_kind, const float* params,
                                                       uint32_t param_count);
/* D-132: parameter `param_index` of voice effect `slot` of the voices of `playback`: a
 * sounding voice from the next block (interpolated inside the block), a voice that takes
 * a slot later from its first block. Kept for the playback's life. SND_ERR_INVALID_HANDLE:
 * 0 or stale; SND_ERR_INVALID_ARGUMENT: slot out of range, param_index >= 16, value not
 * finite. Reported when applied: an [init] parameter of the kind the playback's Event
 * has set now in that stage (warning SND_ERR_INVALID_ARGUMENT, object = playback, dropped;
 * a sounding voice's slot also never takes [init] values of its own kind); the per-playback value table
 * (snd_config voice_effect_settings) full (warning SND_ERR_LIMIT_REACHED, object =
 * playback, dropped). */
SND_API snd_result SND_CALL snd_set_playback_effect_param(snd_system* sys, snd_playback_handle playback,
                                                          uint32_t slot, uint32_t param_index, float value);

/* Nonnegative local half-extents in metres. Point accepts a null extent. */
SND_API snd_result SND_CALL snd_set_emitter_shape(snd_system* sys, snd_emitter_handle emitter,
                                                  snd_emitter_shape kind, const snd_vec3* half_extents);
/* Four strongest listener zones. Amount [0,1], fade_distance >= 0 metres.
 * SND_ID_NONE removes the zone. */
SND_API snd_result SND_CALL snd_set_reverb_zone(snd_system* sys, snd_emitter_handle emitter, snd_id bus,
                                                float amount, float fade_distance);

/* Stage 3 additions                                                         */
/* ========================================================================= */
/* D-146: snd_play with a linear fade-in of the start over fade_in_seconds (rounded to
 * milliseconds, at most 65.535 s; 0 = none, the same as snd_play). Stopping during the
 * fade-in fades out from the level reached. NaN, infinite or negative fade: invalid handle.
 * Thread: registered. */
SND_API snd_playback_handle SND_CALL snd_play_fade(snd_system* sys, snd_id event,
                                                   snd_emitter_handle emitter, const snd_when* when,
                                                   float fade_in_seconds);
/* D-148: spectrum of one channel of a bus as of the latest snd_update(): linear magnitudes
 * of the last FFT-size frames (Hann window; a full-scale sine on a bin reads 1.0), bin k at
 * k * sample_rate / fft_size Hz (fft_size: 1024 by default, CoreLimits.spectrumFftSize).
 * Writes min(bin_count, fft_size / 2 + 1) bins and 0 to the rest of out_magnitudes.
 * bus SND_ID_NONE (or the master's id) = the output; other buses need BUSS.flags
 * kBusMeter (a fixed number of them is measured, 8 by default).
 * Returns SND_OK; SND_ERR_INVALID_ARGUMENT (NULL sys / out, bin_count 0);
 * SND_ERR_NOT_FOUND (unknown bus); SND_ERR_UNSUPPORTED (bus without a meter, or channel
 * not below the bus's channels); SND_ERR_NOT_READY (no data yet: the spectrum is kept up to
 * date only while it is read - the first call after ~1 s without one returns NOT_READY, and
 * the data is there from the first snd_update() that follows a rendered block). Thread: any. */
SND_API snd_result SND_CALL snd_get_spectrum(const snd_system* sys, snd_id bus, uint32_t channel,
                                             float* out_magnitudes, uint32_t bin_count);

SND_EXTERN_C_END

#endif /* SND_SONUS_H */
