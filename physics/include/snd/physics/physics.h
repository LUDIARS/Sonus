/*
 * Physics-material audio (design.md 9), the full C API of the physics module.
 *
 * Builds on the frozen snd/physics.h (snd_phys, snd_contact, snd_surface_weight,
 * snd_contact_kind, snd_motion and the four snd_phys_* functions) and adds the
 * snd_physics_* functions: a versioned configuration, the material table
 * (single events, pair overrides, defaults, footsteps, strength curves), the
 * game material -> sound material map with surface parameters, statistics and
 * an observer callback. One object type: snd_phys (alias snd_physics).
 *
 * The module is an upper layer on the public C API only (design.md 12.1). All
 * of it runs on ONE game thread: the thread that created the snd_phys object,
 * which must be a registered thread of the system (it records snd_play,
 * snd_set_param_playback, ...). Every function below returns
 * SND_ERR_WRONG_THREAD on another thread (except the init helpers, which are
 * pure). Functions that read a bank (snd_phys_create with a bank,
 * snd_physics_load_bank_table) also follow snd_get_bank_chunk: the system's
 * owner thread only.
 *
 * Memory: everything is allocated once at creation from the allocator (NULL =
 * malloc). After creation nothing is allocated: tables are replaced by copying
 * into the fixed-capacity storage taken at creation. Too small a budget at
 * creation: SND_ERR_OUT_OF_BUDGET; a table or map larger than the configured
 * capacity: SND_ERR_LIMIT_REACHED.
 *
 * The snd_phys object keeps the snd_system pointer: destroy it before the
 * system (snd_phys_destroy stops the tracked sliding / rolling playbacks).
 *
 * Behaviour (exact rules, CONTRACTS.md "段階 7-A" candidates D-110..):
 *
 * Time. dt_seconds of snd_phys_submit_contacts is clamped to
 *   config.max_dt_seconds, rounded to whole microseconds (round half away from
 *   zero) and added to a uint64 clock. Cooldowns and timeouts are compared on
 *   that clock in microseconds (the configured seconds are rounded the same way).
 *   Negative, NaN or infinite dt: SND_ERR_INVALID_ARGUMENT and nothing happens.
 *   count == 0 (contacts may be NULL) still advances the clock, the budget carry
 *   and the timeouts. Call once per game frame.
 *
 * Culling of Impact / Break (9.4), one pass in this order:
 *   1. Threshold: Impact with impulse < threshold is dropped (equal passes).
 *      threshold = config.impulse_threshold if > 0, else the table's.
 *      Break has no threshold. Non-finite or negative impulse, non-finite
 *      position, unknown kind: dropped as invalid.
 *   2. Grouping: key = (cell, unordered sound-material pair, kind); cell per axis
 *      = floor(position / grid_size) computed in float (a position of exactly
 *      k * grid_size is in cell k). Representative = the largest impulse (first
 *      in the array on ties); its position, masses, objects and material order
 *      are used; impulse = the maximum; count = the number of contacts merged.
 *   3. Cooldown: key = (unordered object pair, unordered sound-material pair,
 *      kind) of the representative; passes when no record exists or
 *      now - last_play >= cooldown (equal passes). Recorded only for groups that
 *      were actually requested in step 4.
 *   4. Budget: Break first, then by impulse descending (array order of the
 *      representative on ties); the first floor(carry + budget_per_second * dt)
 *      are requested, only the fraction is carried (an unused whole part is
 *      not). dt here is the clamped dt in whole microseconds.
 *   A group whose event resolves to nothing is dropped before step 3.
 *
 * Event resolution (9.3) for a contact of materials A, B and kind k:
 *   pair override pair[k][min(A,B) * N + max(A,B)] -> both single events
 *   single[A][k] and single[B][k] (each that is set; A == B plays once) ->
 *   default[k] -> nothing. A material that is unknown (not in the map, mapped to
 *   SND_PHYSICS_UNMAPPED, or >= N) skips the pair and single stages and goes to
 *   default[k].
 * Strength (9.3): a single event of A uses A's curves and mass_a (B: B's and
 *   mass_b); a pair override or a default event uses the material and mass of
 *   the lighter object (A when equal; non-finite or negative masses count as 0).
 *   Impact / Break: volume = impulse_to_volume(impulse), velocity parameter =
 *   impulse_to_velocity(impulse), pitch = mass_to_pitch(mass). Slide / Roll:
 *   volume = speed_to_volume(speed), pitch = speed_to_pitch(speed). A curve with
 *   0 points falls back to the table default curve, then to the built-in curve
 *   (SND_PHYSICS_BUILTIN_*, below). Volume is clamped to [0, 1000], pitch to
 *   [1/16, 16]; snd_set_playback_volume / _pitch are recorded only when the value
 *   is not exactly 1 (a playback starts at 1).
 * Surface parameters (9.1): a single event gets its material's surface
 *   parameters, a pair override or default event both materials' (the larger
 *   value when an id appears in both), as playback-scope parameters.
 *
 * Positioned one-shots: Impact / Break are played with snd_create_emitter ->
 *   snd_set_transform -> snd_play -> parameters, volume, pitch ->
 *   snd_destroy_emitter (the playback keeps the last transform). When no emitter
 *   slot is free (or the transform is refused) the request falls back to
 *   snd_play_at without parameters, volume or pitch (counted in
 *   play_at_fallbacks, flag SND_PHYSICS_RECORD_FLAG_POSITION_ONLY).
 *
 * Footsteps (9.2): surfaces with weight > 0 (finite) are candidates; the two
 *   largest (first on ties) play. volume = weight_to_volume(w / (w1 + w2))
 *   (one surface: weight_to_volume(1)). Event: footstep[M][motion] ->
 *   footstep_any[M] -> footstep_default -> nothing (unknown material: straight to
 *   footstep_default). Played on `foot` (2D when the handle is 0). The motion,
 *   the foot's sound material and the normalised weight are passed as parameters,
 *   with the surface's surface parameters.
 *
 * Sliding / rolling (9.5): key = (unordered object pair, unordered sound-material
 *   pair, kind). The first report creates an emitter and starts the resolved
 *   event(s); later reports (the largest relative speed when one call repeats a
 *   key) move the emitter and update the speed parameter, volume and pitch. An
 *   entry not reported for >= tracking_timeout is stopped with
 *   snd_stop(tracking_fade_seconds) and its emitter destroyed (a report in the
 *   same call keeps it alive). Table full: the entry with the oldest last report
 *   (then the oldest start) is evicted with the fade; entries reported in the
 *   current call are never evicted (the new contact is dropped instead,
 *   tracking_dropped). A playback that ended by itself (state ENDED, or the
 *   handle is refused as stale) is restarted with the report that finds it.
 *   Slide / Roll have no budget; relative_speed < min_slide_speed is dropped as
 *   below threshold; non-finite or negative relative_speed is invalid.
 */
#ifndef SND_PHYSICS_PHYSICS_H
#define SND_PHYSICS_PHYSICS_H

#include <stddef.h>
#include <stdint.h>

#include "snd/id.h"
#include "snd/physics.h"
#include "snd/sonus.h"
#include "snd/types.h"

SND_EXTERN_C_BEGIN

typedef snd_phys snd_physics; /* the one object type */

/* ------------------------------------------------------------------------- */
/* Limits                                                                     */
/* ------------------------------------------------------------------------- */
#define SND_PHYSICS_CONFIG_VERSION 1u
#define SND_PHYSICS_CONTACT_KIND_COUNT 4u /* Impact, Slide, Roll, Break */
#define SND_PHYSICS_MOTION_COUNT 6u       /* SND_MOTION_WALK .. SND_MOTION_SCUFF */
#define SND_PHYSICS_MAX_CURVE_POINTS 8u
#define SND_PHYSICS_MAX_SURFACE_PARAMS 4u
#define SND_PHYSICS_MAX_MATERIALS_LIMIT 1024u /* upper bound of config.max_materials */
#define SND_PHYSICS_MAX_CAPACITY (1u << 20)  /* upper bound of the other capacities */

#define SND_PHYSICS_UNMAPPED 0xFFFFu         /* map entry / record: no sound material */
#define SND_PHYSICS_DEFAULT_MATERIAL 0xFFFFu /* setters: the table-wide default row */
#define SND_PHYSICS_ALL_KINDS 0xFFFFFFFFu    /* setters: every contact kind */
#define SND_PHYSICS_ANY_MOTION 0xFFFFFFFFu   /* footstep setter: the motion-independent column */

/* ------------------------------------------------------------------------- */
/* Parameters physics passes to events (playback scope). Default ids are      */
/* snd_hash_id of these names; SND_ID_NONE in the config = not passed.        */
/* ------------------------------------------------------------------------- */
#define SND_PHYSICS_PARAM_IMPULSE_NAME "Phys/Impulse"               /* group's max impulse (N*s): Impact, Break */
#define SND_PHYSICS_PARAM_VELOCITY_NAME "Phys/Velocity"             /* impulse_to_velocity: Impact, Break */
#define SND_PHYSICS_PARAM_COUNT_NAME "Phys/Count"                   /* contacts merged (>= 1): Impact, Break */
#define SND_PHYSICS_PARAM_SPEED_NAME "Phys/Speed"                   /* relative speed (m/s): every contact */
#define SND_PHYSICS_PARAM_MASS_NAME "Phys/Mass"                     /* mass used for the curves (kg): every contact */
#define SND_PHYSICS_PARAM_MOTION_NAME "Phys/Motion"                 /* snd_motion as a number: footsteps */
#define SND_PHYSICS_PARAM_FOOT_MATERIAL_NAME "Phys/FootMaterial"    /* foot's sound material: footsteps */
#define SND_PHYSICS_PARAM_SURFACE_WEIGHT_NAME "Phys/SurfaceWeight"  /* w / (w1 + w2): footsteps */

typedef struct snd_physics_param_ids {
  snd_id impulse;
  snd_id velocity;
  snd_id count;
  snd_id speed;
  snd_id mass;
  snd_id motion;
  snd_id foot_material;
  snd_id surface_weight;
} snd_physics_param_ids;

/* A parameter id and value (surface parameters, observer records). */
typedef struct snd_physics_param {
  snd_id id;
  float value;
} snd_physics_param;

/* ------------------------------------------------------------------------- */
/* Observer (design 9.3 "代替が起きたことはツールで一覧できる")              */
/* ------------------------------------------------------------------------- */
typedef uint32_t snd_physics_record_kind;
#define SND_PHYSICS_RECORD_PLAY 0u     /* a one-shot was requested (contact, footstep, play_at) */
#define SND_PHYSICS_RECORD_START 1u    /* a sliding / rolling playback was started (or restarted) */
#define SND_PHYSICS_RECORD_UPDATE 2u   /* a sliding / rolling playback was updated */
#define SND_PHYSICS_RECORD_STOP 3u     /* a sliding / rolling playback was stopped */
#define SND_PHYSICS_RECORD_FALLBACK 4u /* event resolution did not use the first stage */

typedef uint32_t snd_physics_source;
#define SND_PHYSICS_SOURCE_CONTACT 0u
#define SND_PHYSICS_SOURCE_FOOTSTEP 1u
#define SND_PHYSICS_SOURCE_PLAY_AT 2u

/* The stage of the event resolution that chose the event. */
typedef uint32_t snd_physics_stage;
#define SND_PHYSICS_STAGE_PAIR 0u         /* contact: pair override */
#define SND_PHYSICS_STAGE_SINGLE 1u       /* contact: single event(s) of the materials */
#define SND_PHYSICS_STAGE_DEFAULT 2u      /* contact: table default of the kind */
#define SND_PHYSICS_STAGE_NOTHING 3u      /* nothing to play */
#define SND_PHYSICS_STAGE_FOOT_MOTION 4u  /* footstep: footstep[M][motion] */
#define SND_PHYSICS_STAGE_FOOT_ANY 5u     /* footstep: footstep_any[M] */
#define SND_PHYSICS_STAGE_FOOT_DEFAULT 6u /* footstep: footstep_default */
#define SND_PHYSICS_STAGE_DIRECT 7u       /* snd_physics_play_at: the event given */

typedef uint32_t snd_physics_stop_reason;
#define SND_PHYSICS_STOP_NONE 0u
#define SND_PHYSICS_STOP_TIMEOUT 1u   /* not reported for tracking_timeout */
#define SND_PHYSICS_STOP_EVICTED 2u   /* tracking table full */
#define SND_PHYSICS_STOP_DESTROYED 3u /* snd_phys_destroy */
#define SND_PHYSICS_STOP_ENDED 4u     /* the playback had ended by itself (restart follows) */

#define SND_PHYSICS_RECORD_FLAG_POSITION_ONLY 0x1u    /* played with snd_play_at: no parameters / volume / pitch */
#define SND_PHYSICS_RECORD_FLAG_UNKNOWN_MATERIAL 0x2u /* a material was unknown (resolution went to the default) */
#define SND_PHYSICS_RECORD_FLAG_RESTART 0x4u          /* START of a playback that had ended by itself */

typedef struct snd_physics_record {
  snd_physics_record_kind kind;
  snd_physics_source source;
  uint32_t contact_kind;       /* snd_contact_kind; snd_motion for footsteps */
  snd_physics_stage stage;
  snd_physics_stop_reason stop_reason;
  uint32_t flags;              /* SND_PHYSICS_RECORD_FLAG_* */
  snd_id event;                /* SND_ID_NONE in FALLBACK records of STAGE_NOTHING */
  uint32_t count;              /* contacts merged (one-shot contacts), else 1 */
  uint16_t material_a, material_b;           /* sound materials, SND_PHYSICS_UNMAPPED = unknown / none */
  uint16_t game_material_a, game_material_b; /* as given (footstep: surface, foot) */
  float impulse;  /* Impact / Break: the group's max impulse */
  float velocity; /* Impact / Break: impulse_to_velocity */
  float speed;    /* relative speed (contacts) */
  float mass;     /* mass used for the curves (contacts) */
  float volume;   /* linear, as recorded (1 = not recorded) */
  float pitch;    /* rate ratio, as recorded (1 = not recorded) */
  snd_vec3 position;
  uint64_t object_a, object_b;
  snd_playback_handle playback; /* 0 for position-only plays, failures and FALLBACK records */
  snd_emitter_handle emitter;   /* emitter used (0 = 2D / play_at) */
  snd_result result;            /* SND_OK, or the failure of the API call that recorded it */
  uint32_t param_count;
  const snd_physics_param* params; /* parameters recorded on the playback, in order; valid during the call */
} snd_physics_record;

/* Called on the physics thread, synchronously, one record at a time. It must not call
 * any snd_phys_* / snd_physics_* function (they return SND_ERR_UNSUPPORTED meanwhile). */
typedef void(SND_CALL* snd_physics_observer)(void* user, const snd_physics_record* record);

/* ------------------------------------------------------------------------- */
/* Configuration                                                              */
/* ------------------------------------------------------------------------- */
typedef struct snd_physics_config {
  uint32_t struct_size; /* = sizeof(snd_physics_config); a smaller size (an older header) gets
                           defaults for the fields it does not cover; must be a multiple of 4 */
  uint32_t version;     /* = SND_PHYSICS_CONFIG_VERSION (1..current) */
  float budget_per_second;         /* 9.4 step 4, default 300 (>= 0, finite) */
  float grid_size;                 /* 9.4 step 2 (m), default 0.5 (> 0, finite) */
  float cooldown_seconds;          /* 9.4 step 3, default 0.05 (>= 0, finite) */
  float impulse_threshold;         /* 9.4 step 1 (N*s), 0 = the table's (default 0) */
  float min_slide_speed;           /* Slide / Roll lower bound (m/s), default 0 = none */
  float tracking_timeout_seconds;  /* 9.5, default 0.2 (>= 0, finite) */
  float tracking_fade_seconds;     /* 9.5 fade of a stop, default 0.1 (>= 0, finite) */
  float max_dt_seconds;            /* dt clamp, default 0.1 (> 0, finite) */
  uint32_t max_tracked_pairs;      /* 9.5 tracking table, default 64 */
  uint32_t max_cooldowns;          /* 9.4 cooldown table, default 256 */
  uint32_t max_groups_per_call;    /* work area of one call (groups + slide reports), default 2048 */
  uint32_t max_materials;          /* sound materials N, default 64 (1..SND_PHYSICS_MAX_MATERIALS_LIMIT) */
  uint32_t max_game_materials;     /* game -> sound material map entries, default 1024 */
  uint32_t reserved;               /* 0 */
  snd_physics_param_ids params;    /* parameter ids (defaults: hashes of the names above) */
  snd_physics_observer observer;   /* NULL = none */
  void* observer_user;
} snd_physics_config;

/* Fill defaults. Thread: any. */
static inline void snd_physics_config_init(snd_physics_config* c) {
  if (c == NULL) return;
  c->struct_size = (uint32_t)sizeof(snd_physics_config);
  c->version = SND_PHYSICS_CONFIG_VERSION;
  c->budget_per_second = 300.0f;
  c->grid_size = 0.5f;
  c->cooldown_seconds = 0.05f;
  c->impulse_threshold = 0.0f;
  c->min_slide_speed = 0.0f;
  c->tracking_timeout_seconds = 0.2f;
  c->tracking_fade_seconds = 0.1f;
  c->max_dt_seconds = 0.1f;
  c->max_tracked_pairs = 64u;
  c->max_cooldowns = 256u;
  c->max_groups_per_call = 2048u;
  c->max_materials = 64u;
  c->max_game_materials = 1024u;
  c->reserved = 0u;
  c->params.impulse = snd_hash_id(SND_PHYSICS_PARAM_IMPULSE_NAME);
  c->params.velocity = snd_hash_id(SND_PHYSICS_PARAM_VELOCITY_NAME);
  c->params.count = snd_hash_id(SND_PHYSICS_PARAM_COUNT_NAME);
  c->params.speed = snd_hash_id(SND_PHYSICS_PARAM_SPEED_NAME);
  c->params.mass = snd_hash_id(SND_PHYSICS_PARAM_MASS_NAME);
  c->params.motion = snd_hash_id(SND_PHYSICS_PARAM_MOTION_NAME);
  c->params.foot_material = snd_hash_id(SND_PHYSICS_PARAM_FOOT_MATERIAL_NAME);
  c->params.surface_weight = snd_hash_id(SND_PHYSICS_PARAM_SURFACE_WEIGHT_NAME);
  c->observer = NULL;
  c->observer_user = NULL;
}

/*
 * Create on the calling thread (a registered thread of `sys`), with an empty table and the
 * identity material map. snd_phys_create (snd/physics.h) is the same with a snd_phys_desc
 * mapped onto the defaults and the table read from a bank.
 * Errors: SND_ERR_INVALID_ARGUMENT (NULL sys / cfg / out, bad struct_size / version, a value out
 * of range), SND_ERR_OUT_OF_BUDGET (the allocation failed). On error *out is NULL.
 */
SND_API snd_result SND_CALL snd_physics_create(snd_system* sys, const snd_physics_config* cfg,
                                               const snd_allocator* allocator, snd_phys** out);
/* Same as snd_phys_destroy. NULL is a no-op. */
SND_API void SND_CALL snd_physics_destroy(snd_phys* phys);

/* 9.5 position-only one-shot: snd_play_at(event, position). Counted in requested_play_at.
 * Errors: INVALID_ARGUMENT (NULL, event SND_ID_NONE, non-finite position), WRONG_THREAD, and the
 * result of snd_play_at. */
SND_API snd_result SND_CALL snd_physics_play_at(snd_phys* phys, snd_id event, const snd_vec3* position);

/* Replace the observer. Thread: the physics thread. */
SND_API snd_result SND_CALL snd_physics_set_observer(snd_phys* phys, snd_physics_observer observer,
                                                     void* user);

/* ------------------------------------------------------------------------- */
/* Material table (9.1, 9.3)                                                  */
/* ------------------------------------------------------------------------- */
typedef struct snd_physics_curve_point {
  float x, y;
} snd_physics_curve_point;

/* Piecewise-linear curve: points with x non-decreasing (equal x = a step), finite values.
 * Below the first / above the last point: the end values. count 0 = not set (falls back). */
typedef struct snd_physics_curve {
  uint32_t count; /* 0..SND_PHYSICS_MAX_CURVE_POINTS */
  uint32_t reserved;
  snd_physics_curve_point points[SND_PHYSICS_MAX_CURVE_POINTS];
} snd_physics_curve;

typedef struct snd_physics_curves {
  snd_physics_curve impulse_to_volume;   /* impulse (N*s) -> linear volume (Impact, Break) */
  snd_physics_curve impulse_to_velocity; /* impulse (N*s) -> velocity parameter (layer selection) */
  snd_physics_curve mass_to_pitch;       /* mass (kg) -> pitch ratio (Impact, Break) */
  snd_physics_curve speed_to_volume;     /* relative speed (m/s) -> linear volume (Slide, Roll) */
  snd_physics_curve speed_to_pitch;      /* relative speed (m/s) -> pitch ratio (Slide, Roll) */
} snd_physics_curves;

/*
 * Built-in curves (used when neither the material nor the table default sets one). They are the
 * mapping of the bank format's default PhysCurves {impulseToVelocity {0.1, 1, 10, 100},
 * massToPitchCents {0, 0}, speedToVolumeDb {-30, 0}} (the PHYS mapping below):
 *   impulse_to_volume   (0.1, 0.25) (1, 0.5) (10, 0.75) (100, 1)
 *   impulse_to_velocity (0.1, 0) (1, 1/3) (10, 2/3) (100, 1)
 *   mass_to_pitch       constant 1
 *   speed_to_volume     8 points x = 0.1 * 100^(i/7), y = 10^(dB/20), dB = -30 + 30 * i/7
 *   speed_to_pitch      constant 1
 *   weight_to_volume    identity on [0, 1]
 * PHYS (bank format 2.0) per material PhysCurves {t[4], cents[2], db[2]} is mapped to:
 *   impulse_to_volume   (t0, 0.25) (t1, 0.5) (t2, 0.75) (t3, 1)
 *   impulse_to_velocity (t0, 0) (t1, 1/3) (t2, 2/3) (t3, 1)   (weak / medium / strong layers)
 *   mass_to_pitch       8 points x = 100^(i/7) kg, y = 2^(c/1200), c = cents0 + (cents1 - cents0) * i/7
 *   speed_to_volume     8 points x = 0.1 * 100^(i/7) m/s, y = 10^(d/20), d = db0 + (db1 - db0) * i/7
 *   speed_to_pitch      not set
 * and the table's impulse threshold is the smallest t0 of its materials (0 without materials).
 */

typedef struct snd_physics_table_desc {
  uint32_t struct_size;   /* = sizeof(snd_physics_table_desc) (smaller: older header, defaults) */
  uint32_t material_count;/* N (<= config.max_materials) */
  const snd_id* single;   /* [N * 4]: single[m * 4 + kind]; NULL = none */
  const snd_id* pair;     /* [N * N]: pair[min * N + max] for every kind (the lower triangle is ignored); NULL = none */
  const snd_id* pair_by_kind[SND_PHYSICS_CONTACT_KIND_COUNT]; /* per kind, replaces `pair` for that kind when non-NULL */
  const snd_id* footstep;     /* [N * SND_PHYSICS_MOTION_COUNT]: footstep[m * 6 + motion]; NULL = none */
  const snd_id* footstep_any; /* [N]: footstep event of a material for every motion; NULL = none */
  snd_id footstep_default;
  snd_id default_event[SND_PHYSICS_CONTACT_KIND_COUNT];
  const snd_physics_curves* curves;         /* [N]; NULL = none */
  const snd_physics_curves* default_curves; /* table default curves; NULL = none */
  snd_physics_curve weight_to_volume;       /* footstep weight -> volume; count 0 = identity */
  float impulse_threshold;                  /* the table's threshold (>= 0) */
  uint32_t reserved;
} snd_physics_table_desc;

/* Empty table (N = 0, no events, no curves, threshold 0). Thread: any. */
static inline void snd_physics_table_desc_init(snd_physics_table_desc* d) {
  uint32_t k;
  if (d == NULL) return;
  d->struct_size = (uint32_t)sizeof(snd_physics_table_desc);
  d->material_count = 0u;
  d->single = NULL;
  d->pair = NULL;
  for (k = 0; k < SND_PHYSICS_CONTACT_KIND_COUNT; ++k) d->pair_by_kind[k] = NULL;
  d->footstep = NULL;
  d->footstep_any = NULL;
  d->footstep_default = SND_ID_NONE;
  for (k = 0; k < SND_PHYSICS_CONTACT_KIND_COUNT; ++k) d->default_event[k] = SND_ID_NONE;
  d->curves = NULL;
  d->default_curves = NULL;
  d->weight_to_volume.count = 0u;
  d->weight_to_volume.reserved = 0u;
  for (k = 0; k < SND_PHYSICS_MAX_CURVE_POINTS; ++k) {
    d->weight_to_volume.points[k].x = 0.0f;
    d->weight_to_volume.points[k].y = 0.0f;
  }
  d->impulse_threshold = 0.0f;
  d->reserved = 0u;
}

/*
 * Replace the whole table (copied). Takes effect from the next submit / footstep; tracked
 * sliding / rolling playbacks keep playing (their later volume / pitch use the new curves).
 * Errors: INVALID_ARGUMENT (NULL, bad struct_size, a curve with count > max, non-finite values
 * or decreasing x, negative threshold), LIMIT_REACHED (N > config.max_materials). On error the
 * table is unchanged.
 */
SND_API snd_result SND_CALL snd_physics_set_table(snd_phys* phys, const snd_physics_table_desc* desc);

/*
 * Replace the table with table `table` of the bank's 'PHYS' chunk (bank format 2.0 §6.2,
 * mapping above). The chunk is validated (ranges, alignment, counts, finite curve values,
 * non-decreasing impulse thresholds) before anything changes.
 * Errors: INVALID_ARGUMENT (NULL, table SND_ID_NONE), WRONG_THREAD (not the system's owner),
 * INVALID_HANDLE, NOT_READY (bank not loaded), NOT_FOUND (no PHYS chunk / no such table),
 * CORRUPT, LIMIT_REACHED (N > config.max_materials).
 */
SND_API snd_result SND_CALL snd_physics_load_bank_table(snd_phys* phys, snd_bank_handle bank, snd_id table);

/* Sound materials of the current table (N). 0 for NULL / another thread. */
SND_API uint32_t SND_CALL snd_physics_material_count(const snd_phys* phys);

/* One entry. kind may be SND_PHYSICS_ALL_KINDS. event SND_ID_NONE clears.
 * Errors: INVALID_ARGUMENT (material >= N, kind / motion out of range). */
SND_API snd_result SND_CALL snd_physics_set_single_event(snd_phys* phys, uint16_t material,
                                                         snd_contact_kind kind, snd_id event);
SND_API snd_result SND_CALL snd_physics_set_pair_event(snd_phys* phys, uint16_t material_a,
                                                       uint16_t material_b, snd_contact_kind kind,
                                                       snd_id event);
SND_API snd_result SND_CALL snd_physics_set_default_event(snd_phys* phys, snd_contact_kind kind,
                                                          snd_id event);
/* material SND_PHYSICS_DEFAULT_MATERIAL with motion SND_PHYSICS_ANY_MOTION: footstep_default.
 * motion SND_PHYSICS_ANY_MOTION: footstep_any[material]. */
SND_API snd_result SND_CALL snd_physics_set_footstep_event(snd_phys* phys, uint16_t material,
                                                           snd_motion motion, snd_id event);
/* Curves of a material (SND_PHYSICS_DEFAULT_MATERIAL: the table default). NULL clears. */
SND_API snd_result SND_CALL snd_physics_set_curves(snd_phys* phys, uint16_t material,
                                                   const snd_physics_curves* curves);

/* ------------------------------------------------------------------------- */
/* Game material -> sound material map (9.1)                                  */
/* ------------------------------------------------------------------------- */
typedef struct snd_physics_material_entry {
  uint16_t sound_material; /* SND_PHYSICS_UNMAPPED = unknown */
  uint16_t param_count;    /* 0..SND_PHYSICS_MAX_SURFACE_PARAMS */
  snd_physics_param params[SND_PHYSICS_MAX_SURFACE_PARAMS]; /* surface parameters, distinct ids */
} snd_physics_material_entry;

/*
 * Register the map: entries[g] is game material g (copied). count 0 restores the identity
 * (material numbers are sound materials, no surface parameters). With a map, a game material
 * >= count is unknown. Errors: INVALID_ARGUMENT (NULL with count > 0, param_count too large,
 * id SND_ID_NONE / reserved / repeated, non-finite value), LIMIT_REACHED (count >
 * config.max_game_materials). On error the map is unchanged.
 */
SND_API snd_result SND_CALL snd_physics_set_material_map(snd_phys* phys,
                                                         const snd_physics_material_entry* entries,
                                                         uint32_t count);
/* Set one entry. A game material past the current count extends the map; the entries added in
 * between map to themselves (identity, no parameters). On an identity map this registers a
 * map of game_material + 1 identity entries first. Errors: as above, LIMIT_REACHED when
 * game_material >= config.max_game_materials. */
SND_API snd_result SND_CALL snd_physics_set_material_entry(snd_phys* phys, uint16_t game_material,
                                                           const snd_physics_material_entry* entry);
/* Change (or add) one surface parameter of a registered entry ("it starts to rain").
 * Errors: NOT_FOUND (no map or game_material >= count), INVALID_ARGUMENT (id, value),
 * LIMIT_REACHED (a new id with SND_PHYSICS_MAX_SURFACE_PARAMS already set). */
SND_API snd_result SND_CALL snd_physics_set_surface_param(snd_phys* phys, uint16_t game_material,
                                                          snd_id param, float value);

/* ------------------------------------------------------------------------- */
/* Statistics (cumulative)                                                    */
/* ------------------------------------------------------------------------- */
/*
 * Accounting: contacts_received = dropped_invalid + dropped_threshold + dropped_work_overflow
 *   + merged + groups + slide_reports;
 * groups = dropped_no_event + dropped_cooldown + dropped_budget + group_requests (Impact / Break
 *   groups that were requested; each requests 1 or 2 events, counted in requested_impact /
 *   requested_break, failed plays included);
 * slide_reports (one per key and call) = tracking_started + tracking_updated + tracking_restarted
 *   + tracking_dropped + (reports resolving to nothing, counted in fallback_none).
 * Resolution stages (one per resolution of a requested group / started entry): resolved_pair +
 *   fallback_pair_to_single + fallback_single_to_default + fallback_none; fallback_unknown_material
 *   counts in addition those where a material was unknown. Footstep surfaces use the footstep_*
 *   stages instead.
 */
typedef struct snd_physics_stats {
  uint32_t struct_size; /* set by the caller: bytes it can receive */
  uint32_t reserved;
  uint64_t calls_submit;
  uint64_t calls_rejected;       /* calls refused with INVALID_ARGUMENT (nothing done) */
  uint64_t contacts_received;
  uint64_t dropped_invalid;
  uint64_t dropped_threshold;
  uint64_t dropped_work_overflow;
  uint64_t merged;
  uint64_t groups;
  uint64_t dropped_no_event;
  uint64_t dropped_cooldown;
  uint64_t dropped_budget;
  uint64_t group_requests;
  uint64_t requested_impact;     /* events requested for Impact groups */
  uint64_t requested_break;
  uint64_t requested_footstep;
  uint64_t requested_play_at;    /* snd_physics_play_at */
  uint64_t play_at_fallbacks;    /* one-shots played with snd_play_at for want of an emitter */
  uint64_t resolved_pair;
  uint64_t fallback_pair_to_single;
  uint64_t fallback_single_to_default;
  uint64_t fallback_none;
  uint64_t fallback_unknown_material;
  uint64_t cooldown_evictions;   /* records still in cooldown pushed out of the full table */
  uint64_t slide_reports;
  uint64_t tracking_started;
  uint64_t tracking_updated;
  uint64_t tracking_timeouts;
  uint64_t tracking_evictions;
  uint64_t tracking_restarted;
  uint64_t tracking_dropped;     /* no entry could be freed (all reported in this call) or no emitter */
  uint64_t footsteps;            /* accepted snd_phys_play_footstep calls */
  uint64_t footstep_by_motion;
  uint64_t footstep_fallback_any_motion;
  uint64_t footstep_fallback_default;
  uint64_t footstep_fallback_none;
  uint64_t footstep_surfaces_ignored; /* weight <= 0, NaN or infinite, or past the two largest */
  uint64_t api_failures;         /* sonus C API calls that failed */
} snd_physics_stats;

/* Copy min(out->struct_size, sizeof) bytes (struct_size is kept). Errors: INVALID_ARGUMENT
 * (NULL, struct_size < 8). */
SND_API snd_result SND_CALL snd_physics_get_stats(const snd_phys* phys, snd_physics_stats* out);
SND_API snd_result SND_CALL snd_physics_reset_stats(snd_phys* phys);

SND_EXTERN_C_END

#endif /* SND_PHYSICS_PHYSICS_H */
