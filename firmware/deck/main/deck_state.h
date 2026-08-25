/*
 * ESP-DJ3000 deck — global deck model.
 *
 * One instance, shared between the RT audio task (core 1) and the UI /
 * bus / net tasks (core 0). Cross-core rules:
 *   - control fields are written by core 0, read by core 1 (single writer);
 *     they are ints/floats written atomically on Xtensa, no locks in the
 *     audio path.
 *   - status fields flow the other way (core 1 -> core 0).
 *   - anything structural (track load) goes through the command queue.
 */
#pragma once

#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

#include "espdj_proto.h"
#include "track_meta.h"

#define DECK_HOT_CUES 4

typedef enum {
    DECK_CMD_LOAD_TRACK = 1, /* arg: library index */
    DECK_CMD_PLAY_TOGGLE,
    DECK_CMD_CUE_PRESS,
    DECK_CMD_CUE_RELEASE,
    DECK_CMD_HOT_CUE,        /* arg: cue index */
    DECK_CMD_LOOP_TOGGLE,    /* arg: beats */
    DECK_CMD_BEAT_JUMP,      /* arg: signed beats */
    DECK_CMD_SLIP_TOGGLE,
    DECK_CMD_SYNC_TOGGLE,
    DECK_CMD_SEEK_MS,        /* arg: position ms */
} deck_cmd_op_t;

typedef struct {
    deck_cmd_op_t op;
    int32_t arg;
} deck_cmd_t;

typedef struct {
    /* ---- control (core 0 writes, core 1 reads) ---- */
    _Atomic float pitch;        /* tempo fader, fraction: +0.08 = +8% */
    _Atomic float jog_rate;     /* signed rate from ring while touched; NAN-free: 0 = released */
    _Atomic bool  jog_active;   /* ring touched: rate comes from jog, scratch mode */
    _Atomic float xfader_gain;  /* this deck's computed crossfader gain 0..1 */
    _Atomic float trim;         /* channel gain */
    _Atomic float eq_low_db, eq_mid_db, eq_high_db;
    _Atomic float filter_pos;   /* -1..1 */
    _Atomic bool  cue_monitor;  /* pre-fader tap onto the CUE bus */
    _Atomic bool  sync_enabled;

    /* ---- status (core 1 writes, core 0 reads) ---- */
    _Atomic bool     playing;
    _Atomic bool     slip;
    _Atomic bool     loop_active;
    _Atomic uint32_t position_frames; /* playhead in source frames */
    _Atomic float    effective_bpm;   /* grid bpm * current rate */
    _Atomic float    beat_phase;      /* 0..1 within the current beat */
    _Atomic uint32_t beat_number;
    _Atomic bool     track_loaded;

    /* ---- role / topology (bus task writes) ---- */
    _Atomic uint8_t side;        /* espdj_side_t */
    _Atomic bool    is_master;   /* bus master (leftmost deck, no ctrl) */
    _Atomic bool    is_sync_master;
    _Atomic bool    wireless;    /* ESP-NOW mode (undocked) */

    /* ---- loaded track ---- */
    track_meta_t meta;           /* owned by audio task after LOAD */
    char track_path[192];

    /* ---- plumbing ---- */
    QueueHandle_t cmd_q;         /* deck_cmd_t, any task -> audio task */
} deck_state_t;

extern deck_state_t g_deck;

static inline void deck_send_cmd(deck_cmd_op_t op, int32_t arg)
{
    deck_cmd_t c = { .op = op, .arg = arg };
    xQueueSend(g_deck.cmd_q, &c, 0);
}

/* Route an espdj transport op (from controller pads, phone, or partner
 * deck) into the local command queue. */
void deck_apply_transport(const espdj_transport_t *t);
