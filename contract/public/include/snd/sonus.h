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
 *    1.1 (D-46): slots come from a lock-free allocator shared by all
 *    registered threads (base::HandleAllocator); a slot goes back to it only
 *    in the owner's snd_update(), when the end / release flag of the slot is
 *    consumed. Issued generations are below 2^31 (see snd/types.h).
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
 * Errors: SND_ERR_INVALID_ARGUMENT (null out, bad cfg/version/struct_size),
 * SND_ERR_OUT_OF_BUDGET, SND_ERR_NO_DEVICE. On error *out is set to NULL.
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
 * Declaring the same name again is SND_OK. Thread: registered. Recorded as a
 * must-deliver command (the name is copied).
 * Errors: SND_ERR_INVALID_ARGUMENT (null / empty / too long / bad character),
 * SND_ERR_LIMIT_REACHED (SND_MAX_LANGUAGES names already declared). */
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
/* bus SND_ID_NONE = master. */
SND_API snd_result SND_CALL snd_get_meter(const snd_system* sys, snd_id bus, snd_meter* out);
SND_API snd_result SND_CALL snd_get_stats(const snd_system* sys, snd_stats* out);

SND_EXTERN_C_END

#endif /* SND_SONUS_H */
