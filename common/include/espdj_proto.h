/*
 * ESP-DJ3000 — shared control protocol.
 *
 * One frame format is used on both transports:
 *   - the half-duplex multi-drop DATA bus (1 Mbps UART, master-polled), and
 *   - ESP-NOW wireless mode (two undocked decks).
 *
 * Wire format:
 *   [SYNC=0xD7][len][type][seq][payload ... len bytes][crc8]
 *
 * crc8 (poly 0x07, init 0x00) covers len..payload. `len` counts payload
 * bytes only. Frames are short (~8 bytes at 250 Hz) so the 1 Mbps bus has
 * roughly 500x headroom, per the design doc.
 *
 * This file is portable C99: it compiles on the ESP32-S3 deck, the ESP32-C3
 * controller, and the host test harness.
 */
#ifndef ESPDJ_PROTO_H
#define ESPDJ_PROTO_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ESPDJ_SYNC_BYTE      0xD7
#define ESPDJ_MAX_PAYLOAD    48
#define ESPDJ_FRAME_OVERHEAD 5 /* sync + len + type + seq + crc */
#define ESPDJ_MAX_FRAME      (ESPDJ_MAX_PAYLOAD + ESPDJ_FRAME_OVERHEAD)

/* Unit types, matched to the SENSE resistor IDs (deck=10k, controller=22k). */
typedef enum {
    ESPDJ_UNIT_NONE       = 0,
    ESPDJ_UNIT_DECK       = 1,
    ESPDJ_UNIT_CONTROLLER = 2,
} espdj_unit_type_t;

typedef enum {
    ESPDJ_SIDE_NONE  = 0,
    ESPDJ_SIDE_LEFT  = 1,
    ESPDJ_SIDE_RIGHT = 2,
} espdj_side_t;

typedef enum {
    /* Discovery: {mac, type, leftOccupied, rightOccupied} broadcast after
     * the 100 ms SENSE debounce. */
    ESPDJ_MSG_ANNOUNCE   = 0x01,
    /* Master -> unit: your slot to speak (multi-drop half duplex). */
    ESPDJ_MSG_POLL       = 0x02,
    /* Polled unit with nothing to say. */
    ESPDJ_MSG_EMPTY      = 0x03,
    /* Controller surface state: crossfader, tempo faders, pots. 250 Hz. */
    ESPDJ_MSG_CONTROL    = 0x10,
    /* Pad / button edge events (hot cue, loop, FX, shift layer). */
    ESPDJ_MSG_PAD_EVENT  = 0x11,
    /* Transport commands routed to a deck (play/cue/loop/beat-jump/...). */
    ESPDJ_MSG_TRANSPORT  = 0x12,
    /* Timestamped beat phase from the sync master, 10 Hz. */
    ESPDJ_MSG_BEAT_PHASE = 0x20,
    /* Deck status back to master / phone (bpm, position, play state). */
    ESPDJ_MSG_DECK_STATE = 0x21,
} espdj_msg_type_t;

typedef struct {
    uint8_t type;
    uint8_t seq;
    uint8_t len;
    uint8_t payload[ESPDJ_MAX_PAYLOAD];
} espdj_frame_t;

/* ---- Typed payloads (packed, little-endian on the wire) ---- */

typedef struct {
    uint8_t mac[6];
    uint8_t unit_type;      /* espdj_unit_type_t */
    uint8_t left_occupied;  /* bool */
    uint8_t right_occupied; /* bool */
    uint8_t proto_version;
} espdj_announce_t;

typedef struct {
    uint16_t crossfader;   /* 0..4095, 0 = full left deck */
    uint16_t tempo[2];     /* per-side tempo fader, 0..4095, 2048 = 0% */
    uint16_t pot[2];       /* outboard pots, 0..4095 */
    uint8_t  xfader_curve; /* 0 = constant power .. 255 = sharp cut */
} espdj_control_t;

typedef struct {
    uint8_t side;    /* which deck the pad bank addresses */
    uint8_t pad;     /* 0..3 */
    uint8_t layer;   /* 0 = hot cue, 1 = loop, 2 = fx, 3 = shift */
    uint8_t pressed; /* 1 = down, 0 = up */
} espdj_pad_event_t;

typedef enum {
    ESPDJ_TR_PLAY_TOGGLE = 1,
    ESPDJ_TR_CUE_PRESS   = 2,
    ESPDJ_TR_CUE_RELEASE = 3,
    ESPDJ_TR_HOT_CUE     = 4,  /* arg = cue index */
    ESPDJ_TR_LOOP_TOGGLE = 5,  /* arg = beats (1,2,4,8...) */
    ESPDJ_TR_BEAT_JUMP   = 6,  /* arg = signed beats (int8) */
    ESPDJ_TR_SLIP_TOGGLE = 7,
    ESPDJ_TR_SYNC_TOGGLE = 8,
    ESPDJ_TR_LOAD_TRACK  = 9,  /* arg = library index (u16 in arg/arg2) */
} espdj_transport_op_t;

typedef struct {
    uint8_t side;
    uint8_t op;   /* espdj_transport_op_t */
    uint8_t arg;
    uint8_t arg2;
} espdj_transport_t;

typedef struct {
    uint64_t timestamp_us; /* sender clock at the instant `phase` was true */
    uint32_t beat_number;  /* absolute beat count since track start */
    uint16_t phase_q16;    /* fractional beat phase, 0..65535 = [0,1) */
    uint16_t bpm_q8;       /* effective BPM * 256 (varispeed applied) */
} espdj_beat_phase_t;

typedef struct {
    uint8_t  side;
    uint8_t  flags;        /* bit0 play, bit1 loop, bit2 slip, bit3 sync */
    uint16_t bpm_q8;
    uint32_t position_ms;
    uint32_t length_ms;
    int16_t  pitch_q8;     /* varispeed percent * 256, signed */
} espdj_deck_state_t;

/* ---- API ---- */

uint8_t espdj_crc8(const uint8_t *data, size_t len);

/* Serialize `frame` into `out` (>= ESPDJ_MAX_FRAME bytes).
 * Returns total encoded length, or 0 if frame->len > ESPDJ_MAX_PAYLOAD. */
size_t espdj_frame_encode(const espdj_frame_t *frame, uint8_t *out);

/* Incremental decoder: feed bytes as they arrive; returns true when a full,
 * CRC-valid frame has been assembled into `out`. Resyncs on bad bytes. */
typedef struct {
    uint8_t state;
    uint8_t idx;
    espdj_frame_t partial;
    uint8_t crc;
} espdj_decoder_t;

void espdj_decoder_init(espdj_decoder_t *d);
bool espdj_decoder_feed(espdj_decoder_t *d, uint8_t byte, espdj_frame_t *out);

/* Payload pack/unpack helpers (explicit little-endian, alignment-safe). */
void espdj_pack_announce(espdj_frame_t *f, const espdj_announce_t *a);
bool espdj_unpack_announce(const espdj_frame_t *f, espdj_announce_t *a);
void espdj_pack_control(espdj_frame_t *f, const espdj_control_t *c);
bool espdj_unpack_control(const espdj_frame_t *f, espdj_control_t *c);
void espdj_pack_pad_event(espdj_frame_t *f, const espdj_pad_event_t *p);
bool espdj_unpack_pad_event(const espdj_frame_t *f, espdj_pad_event_t *p);
void espdj_pack_transport(espdj_frame_t *f, const espdj_transport_t *t);
bool espdj_unpack_transport(const espdj_frame_t *f, espdj_transport_t *t);
void espdj_pack_beat_phase(espdj_frame_t *f, const espdj_beat_phase_t *b);
bool espdj_unpack_beat_phase(const espdj_frame_t *f, espdj_beat_phase_t *b);
void espdj_pack_deck_state(espdj_frame_t *f, const espdj_deck_state_t *s);
bool espdj_unpack_deck_state(const espdj_frame_t *f, espdj_deck_state_t *s);

/* ---- Topology resolution ----
 * <=3 units in a line: port occupancy fully determines order
 * (empty-left unit is leftmost). Master = controller if present,
 * else leftmost deck. */
typedef struct {
    uint8_t mac[6];
    uint8_t unit_type;
    bool left_occupied;
    bool right_occupied;
} espdj_topo_unit_t;

typedef struct {
    int count;                  /* resolved chain length, 0 if inconsistent */
    int order[3];               /* indices into the input array, left->right */
    int master;                 /* index into input array, -1 if none */
    espdj_side_t deck_side[3];  /* per input unit: LEFT/RIGHT deck role */
} espdj_topology_t;

bool espdj_resolve_topology(const espdj_topo_unit_t *units, int n,
                            espdj_topology_t *out);

/* SENSE resistor ID: deck=10k, controller=22k to GND, 47k pull-up to 3V3
 * on the reading side. Returns unit type from the measured ADC millivolts. */
espdj_unit_type_t espdj_sense_classify(uint32_t millivolts, uint32_t vref_mv);

#ifdef __cplusplus
}
#endif

#endif /* ESPDJ_PROTO_H */
