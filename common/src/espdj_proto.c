#include "espdj_proto.h"
#include <string.h>

/* CRC-8, poly 0x07, init 0x00 (bitwise; frames are tiny, no table needed). */
uint8_t espdj_crc8(const uint8_t *data, size_t len)
{
    uint8_t crc = 0x00;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int b = 0; b < 8; b++) {
            crc = (crc & 0x80) ? (uint8_t)((crc << 1) ^ 0x07)
                               : (uint8_t)(crc << 1);
        }
    }
    return crc;
}

size_t espdj_frame_encode(const espdj_frame_t *frame, uint8_t *out)
{
    if (frame->len > ESPDJ_MAX_PAYLOAD) {
        return 0;
    }
    out[0] = ESPDJ_SYNC_BYTE;
    out[1] = frame->len;
    out[2] = frame->type;
    out[3] = frame->seq;
    memcpy(&out[4], frame->payload, frame->len);
    out[4 + frame->len] = espdj_crc8(&out[1], (size_t)frame->len + 3);
    return (size_t)frame->len + ESPDJ_FRAME_OVERHEAD;
}

enum { DEC_SYNC, DEC_LEN, DEC_TYPE, DEC_SEQ, DEC_PAYLOAD, DEC_CRC };

void espdj_decoder_init(espdj_decoder_t *d)
{
    memset(d, 0, sizeof(*d));
    d->state = DEC_SYNC;
}

bool espdj_decoder_feed(espdj_decoder_t *d, uint8_t byte, espdj_frame_t *out)
{
    switch (d->state) {
    case DEC_SYNC:
        if (byte == ESPDJ_SYNC_BYTE) {
            d->state = DEC_LEN;
        }
        return false;
    case DEC_LEN:
        if (byte > ESPDJ_MAX_PAYLOAD) {
            d->state = DEC_SYNC;
            return false;
        }
        d->partial.len = byte;
        d->crc = 0;
        d->state = DEC_TYPE;
        break;
    case DEC_TYPE:
        d->partial.type = byte;
        d->state = DEC_SEQ;
        break;
    case DEC_SEQ:
        d->partial.seq = byte;
        d->idx = 0;
        d->state = (d->partial.len > 0) ? DEC_PAYLOAD : DEC_CRC;
        break;
    case DEC_PAYLOAD:
        d->partial.payload[d->idx++] = byte;
        if (d->idx >= d->partial.len) {
            d->state = DEC_CRC;
        }
        break;
    case DEC_CRC: {
        d->state = DEC_SYNC;
        uint8_t buf[3 + ESPDJ_MAX_PAYLOAD];
        buf[0] = d->partial.len;
        buf[1] = d->partial.type;
        buf[2] = d->partial.seq;
        memcpy(&buf[3], d->partial.payload, d->partial.len);
        uint8_t crc = espdj_crc8(buf, (size_t)d->partial.len + 3);
        if (crc == byte) {
            *out = d->partial;
            return true;
        }
        return false;
    }
    default:
        d->state = DEC_SYNC;
        break;
    }
    (void)out;
    return false;
}

/* ---- little-endian pack/unpack primitives ---- */

static void put_u16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void put_u32(uint8_t *p, uint32_t v) { put_u16(p, (uint16_t)v); put_u16(p + 2, (uint16_t)(v >> 16)); }
static void put_u64(uint8_t *p, uint64_t v) { put_u32(p, (uint32_t)v); put_u32(p + 4, (uint32_t)(v >> 32)); }
static uint16_t get_u16(const uint8_t *p) { return (uint16_t)(p[0] | ((uint16_t)p[1] << 8)); }
static uint32_t get_u32(const uint8_t *p) { return get_u16(p) | ((uint32_t)get_u16(p + 2) << 16); }
static uint64_t get_u64(const uint8_t *p) { return get_u32(p) | ((uint64_t)get_u32(p + 4) << 32); }

void espdj_pack_announce(espdj_frame_t *f, const espdj_announce_t *a)
{
    f->type = ESPDJ_MSG_ANNOUNCE;
    f->len = 10;
    memcpy(&f->payload[0], a->mac, 6);
    f->payload[6] = a->unit_type;
    f->payload[7] = a->left_occupied;
    f->payload[8] = a->right_occupied;
    f->payload[9] = a->proto_version;
}

bool espdj_unpack_announce(const espdj_frame_t *f, espdj_announce_t *a)
{
    if (f->type != ESPDJ_MSG_ANNOUNCE || f->len < 10) return false;
    memcpy(a->mac, &f->payload[0], 6);
    a->unit_type      = f->payload[6];
    a->left_occupied  = f->payload[7];
    a->right_occupied = f->payload[8];
    a->proto_version  = f->payload[9];
    return true;
}

void espdj_pack_control(espdj_frame_t *f, const espdj_control_t *c)
{
    f->type = ESPDJ_MSG_CONTROL;
    f->len = 11;
    put_u16(&f->payload[0], c->crossfader);
    put_u16(&f->payload[2], c->tempo[0]);
    put_u16(&f->payload[4], c->tempo[1]);
    put_u16(&f->payload[6], c->pot[0]);
    put_u16(&f->payload[8], c->pot[1]);
    f->payload[10] = c->xfader_curve;
}

bool espdj_unpack_control(const espdj_frame_t *f, espdj_control_t *c)
{
    if (f->type != ESPDJ_MSG_CONTROL || f->len < 11) return false;
    c->crossfader = get_u16(&f->payload[0]);
    c->tempo[0]   = get_u16(&f->payload[2]);
    c->tempo[1]   = get_u16(&f->payload[4]);
    c->pot[0]     = get_u16(&f->payload[6]);
    c->pot[1]     = get_u16(&f->payload[8]);
    c->xfader_curve = f->payload[10];
    return true;
}

void espdj_pack_pad_event(espdj_frame_t *f, const espdj_pad_event_t *p)
{
    f->type = ESPDJ_MSG_PAD_EVENT;
    f->len = 4;
    f->payload[0] = p->side;
    f->payload[1] = p->pad;
    f->payload[2] = p->layer;
    f->payload[3] = p->pressed;
}

bool espdj_unpack_pad_event(const espdj_frame_t *f, espdj_pad_event_t *p)
{
    if (f->type != ESPDJ_MSG_PAD_EVENT || f->len < 4) return false;
    p->side    = f->payload[0];
    p->pad     = f->payload[1];
    p->layer   = f->payload[2];
    p->pressed = f->payload[3];
    return true;
}

void espdj_pack_transport(espdj_frame_t *f, const espdj_transport_t *t)
{
    f->type = ESPDJ_MSG_TRANSPORT;
    f->len = 4;
    f->payload[0] = t->side;
    f->payload[1] = t->op;
    f->payload[2] = t->arg;
    f->payload[3] = t->arg2;
}

bool espdj_unpack_transport(const espdj_frame_t *f, espdj_transport_t *t)
{
    if (f->type != ESPDJ_MSG_TRANSPORT || f->len < 4) return false;
    t->side = f->payload[0];
    t->op   = f->payload[1];
    t->arg  = f->payload[2];
    t->arg2 = f->payload[3];
    return true;
}

void espdj_pack_beat_phase(espdj_frame_t *f, const espdj_beat_phase_t *b)
{
    f->type = ESPDJ_MSG_BEAT_PHASE;
    f->len = 16;
    put_u64(&f->payload[0], b->timestamp_us);
    put_u32(&f->payload[8], b->beat_number);
    put_u16(&f->payload[12], b->phase_q16);
    put_u16(&f->payload[14], b->bpm_q8);
}

bool espdj_unpack_beat_phase(const espdj_frame_t *f, espdj_beat_phase_t *b)
{
    if (f->type != ESPDJ_MSG_BEAT_PHASE || f->len < 16) return false;
    b->timestamp_us = get_u64(&f->payload[0]);
    b->beat_number  = get_u32(&f->payload[8]);
    b->phase_q16    = get_u16(&f->payload[12]);
    b->bpm_q8       = get_u16(&f->payload[14]);
    return true;
}

void espdj_pack_deck_state(espdj_frame_t *f, const espdj_deck_state_t *s)
{
    f->type = ESPDJ_MSG_DECK_STATE;
    f->len = 14;
    f->payload[0] = s->side;
    f->payload[1] = s->flags;
    put_u16(&f->payload[2], s->bpm_q8);
    put_u32(&f->payload[4], s->position_ms);
    put_u32(&f->payload[8], s->length_ms);
    put_u16(&f->payload[12], (uint16_t)s->pitch_q8);
}

bool espdj_unpack_deck_state(const espdj_frame_t *f, espdj_deck_state_t *s)
{
    if (f->type != ESPDJ_MSG_DECK_STATE || f->len < 14) return false;
    s->side        = f->payload[0];
    s->flags       = f->payload[1];
    s->bpm_q8      = get_u16(&f->payload[2]);
    s->position_ms = get_u32(&f->payload[4]);
    s->length_ms   = get_u32(&f->payload[8]);
    s->pitch_q8    = (int16_t)get_u16(&f->payload[12]);
    return true;
}

/* ---- topology ---- */

bool espdj_resolve_topology(const espdj_topo_unit_t *units, int n,
                            espdj_topology_t *out)
{
    memset(out, 0, sizeof(*out));
    out->master = -1;
    if (n < 1 || n > 3) {
        return false;
    }

    /* Find the leftmost unit: the one whose left port is empty. */
    int leftmost = -1;
    for (int i = 0; i < n; i++) {
        if (!units[i].left_occupied) {
            if (leftmost >= 0) {
                return false; /* two chains / inconsistent SENSE state */
            }
            leftmost = i;
        }
    }
    if (leftmost < 0) {
        return false; /* would imply a loop */
    }

    /* Walk the chain by occupancy. With <=3 units in a line, occupancy
     * flags alone fix the order: leftmost, then any middle (both ports
     * occupied), then the right end (right port empty). */
    out->order[0] = leftmost;
    int pos = 1;
    int middle = -1, rightend = -1;
    for (int i = 0; i < n; i++) {
        if (i == leftmost) continue;
        if (units[i].left_occupied && units[i].right_occupied) {
            if (middle >= 0) return false;
            middle = i;
        } else if (units[i].left_occupied && !units[i].right_occupied) {
            if (rightend >= 0) return false;
            rightend = i;
        } else {
            return false;
        }
    }
    if (n == 1) {
        if (units[leftmost].right_occupied) return false;
    } else if (n == 2) {
        if (middle >= 0 || rightend < 0) return false;
        if (!units[leftmost].right_occupied) return false;
        out->order[pos++] = rightend;
    } else {
        if (middle < 0 || rightend < 0) return false;
        if (!units[leftmost].right_occupied) return false;
        out->order[pos++] = middle;
        out->order[pos++] = rightend;
    }
    out->count = n;

    /* Master: controller if present, else leftmost deck. */
    for (int i = 0; i < n; i++) {
        if (units[i].unit_type == ESPDJ_UNIT_CONTROLLER) {
            out->master = i;
            break;
        }
    }
    if (out->master < 0) {
        out->master = out->order[0];
    }

    /* Deck roles: first deck in chain order is LEFT, second is RIGHT.
     * A single deck (or deck+controller) still gets a side so the
     * controller pad banks address it correctly. */
    espdj_side_t next = ESPDJ_SIDE_LEFT;
    for (int p = 0; p < n; p++) {
        int i = out->order[p];
        if (units[i].unit_type == ESPDJ_UNIT_DECK) {
            out->deck_side[i] = next;
            next = ESPDJ_SIDE_RIGHT;
        } else {
            out->deck_side[i] = ESPDJ_SIDE_NONE;
        }
    }
    return true;
}

/* SENSE divider: 47k pull-up to vref, ID resistor to GND.
 * deck 10k     -> vref * 10/57  = 0.175 * vref
 * controller 22k -> vref * 22/69 = 0.319 * vref
 * open (no mate) -> vref
 * Classify with generous windows (+/- ~40%) to absorb pogo contact
 * resistance and ADC error. */
espdj_unit_type_t espdj_sense_classify(uint32_t millivolts, uint32_t vref_mv)
{
    if (vref_mv == 0) return ESPDJ_UNIT_NONE;
    uint32_t frac_x1000 = (millivolts * 1000u) / vref_mv;
    if (frac_x1000 >= 105 && frac_x1000 < 245) {
        return ESPDJ_UNIT_DECK;
    }
    if (frac_x1000 >= 245 && frac_x1000 < 450) {
        return ESPDJ_UNIT_CONTROLLER;
    }
    return ESPDJ_UNIT_NONE;
}
