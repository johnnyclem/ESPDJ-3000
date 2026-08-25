#include "test_util.h"
#include "espdj_proto.h"

static void test_roundtrip_all_messages(void)
{
    espdj_frame_t f, out;
    uint8_t wire[ESPDJ_MAX_FRAME];
    espdj_decoder_t dec;

    espdj_announce_t a = {
        .mac = {0xAA, 0xBB, 0xCC, 0x01, 0x02, 0x03},
        .unit_type = ESPDJ_UNIT_DECK,
        .left_occupied = 0, .right_occupied = 1, .proto_version = 1,
    };
    f.seq = 7;
    espdj_pack_announce(&f, &a);
    size_t n = espdj_frame_encode(&f, wire);
    CHECK(n == (size_t)f.len + ESPDJ_FRAME_OVERHEAD);

    espdj_decoder_init(&dec);
    bool got = false;
    for (size_t i = 0; i < n; i++) {
        got = espdj_decoder_feed(&dec, wire[i], &out);
    }
    CHECK(got);
    espdj_announce_t a2;
    CHECK(espdj_unpack_announce(&out, &a2));
    CHECK(memcmp(a2.mac, a.mac, 6) == 0);
    CHECK(a2.unit_type == ESPDJ_UNIT_DECK);
    CHECK(a2.right_occupied == 1 && a2.left_occupied == 0);
    CHECK(out.seq == 7);

    espdj_beat_phase_t b = {
        .timestamp_us = 0x0123456789ABCDEFull,
        .beat_number = 1234567,
        .phase_q16 = 40000,
        .bpm_q8 = (uint16_t)(128.5 * 256),
    };
    f.seq = 8;
    espdj_pack_beat_phase(&f, &b);
    n = espdj_frame_encode(&f, wire);
    espdj_decoder_init(&dec);
    got = false;
    for (size_t i = 0; i < n; i++) got = espdj_decoder_feed(&dec, wire[i], &out);
    CHECK(got);
    espdj_beat_phase_t b2;
    CHECK(espdj_unpack_beat_phase(&out, &b2));
    CHECK(b2.timestamp_us == b.timestamp_us);
    CHECK(b2.beat_number == b.beat_number);
    CHECK(b2.phase_q16 == b.phase_q16);
    CHECK(b2.bpm_q8 == b.bpm_q8);

    espdj_control_t c = {
        .crossfader = 2048, .tempo = {100, 4000}, .pot = {0, 4095},
        .xfader_curve = 200,
    };
    f.seq = 9;
    espdj_pack_control(&f, &c);
    n = espdj_frame_encode(&f, wire);
    espdj_decoder_init(&dec);
    got = false;
    for (size_t i = 0; i < n; i++) got = espdj_decoder_feed(&dec, wire[i], &out);
    CHECK(got);
    espdj_control_t c2;
    CHECK(espdj_unpack_control(&out, &c2));
    CHECK(c2.crossfader == 2048 && c2.tempo[1] == 4000 && c2.xfader_curve == 200);

    espdj_transport_t t = { .side = ESPDJ_SIDE_RIGHT, .op = ESPDJ_TR_BEAT_JUMP,
                            .arg = (uint8_t)(int8_t)-4, .arg2 = 0 };
    f.seq = 10;
    espdj_pack_transport(&f, &t);
    n = espdj_frame_encode(&f, wire);
    espdj_decoder_init(&dec);
    got = false;
    for (size_t i = 0; i < n; i++) got = espdj_decoder_feed(&dec, wire[i], &out);
    CHECK(got);
    espdj_transport_t t2;
    CHECK(espdj_unpack_transport(&out, &t2));
    CHECK((int8_t)t2.arg == -4);

    espdj_deck_state_t s = { .side = 1, .flags = 0x0B, .bpm_q8 = 174 * 256,
                             .position_ms = 65000, .length_ms = 312000,
                             .pitch_q8 = (int16_t)(-2.5 * 256) };
    f.seq = 11;
    espdj_pack_deck_state(&f, &s);
    n = espdj_frame_encode(&f, wire);
    espdj_decoder_init(&dec);
    got = false;
    for (size_t i = 0; i < n; i++) got = espdj_decoder_feed(&dec, wire[i], &out);
    CHECK(got);
    espdj_deck_state_t s2;
    CHECK(espdj_unpack_deck_state(&out, &s2));
    CHECK(s2.pitch_q8 == s.pitch_q8 && s2.length_ms == 312000);
}

static void test_decoder_resync_on_garbage(void)
{
    espdj_frame_t f = { .type = ESPDJ_MSG_POLL, .seq = 1, .len = 1,
                        .payload = {5} };
    uint8_t wire[ESPDJ_MAX_FRAME];
    size_t n = espdj_frame_encode(&f, wire);

    /* Garbage prefix, one corrupted frame, then a clean frame. */
    uint8_t stream[3 * ESPDJ_MAX_FRAME];
    size_t pos = 0;
    stream[pos++] = 0x00;
    stream[pos++] = 0xFF;
    stream[pos++] = ESPDJ_SYNC_BYTE; /* false sync */
    stream[pos++] = 200;             /* bogus len -> instant resync */
    memcpy(&stream[pos], wire, n);
    stream[pos + 2] ^= 0xFF; /* corrupt type -> CRC fails */
    pos += n;
    memcpy(&stream[pos], wire, n);
    pos += n;

    espdj_decoder_t dec;
    espdj_decoder_init(&dec);
    espdj_frame_t out;
    int frames = 0;
    for (size_t i = 0; i < pos; i++) {
        if (espdj_decoder_feed(&dec, stream[i], &out)) frames++;
    }
    CHECK(frames == 1);
    CHECK(out.type == ESPDJ_MSG_POLL && out.payload[0] == 5);
}

static void set_unit(espdj_topo_unit_t *u, uint8_t mac0, int type, bool l, bool r)
{
    memset(u, 0, sizeof(*u));
    u->mac[0] = mac0;
    u->unit_type = (uint8_t)type;
    u->left_occupied = l;
    u->right_occupied = r;
}

static void test_topology(void)
{
    espdj_topo_unit_t u[3];
    espdj_topology_t topo;

    /* Single deck. */
    set_unit(&u[0], 1, ESPDJ_UNIT_DECK, false, false);
    CHECK(espdj_resolve_topology(u, 1, &topo));
    CHECK(topo.master == 0 && topo.deck_side[0] == ESPDJ_SIDE_LEFT);

    /* Deck|Deck: leftmost deck is master, sides assigned in order. */
    set_unit(&u[0], 1, ESPDJ_UNIT_DECK, true, false);  /* right end */
    set_unit(&u[1], 2, ESPDJ_UNIT_DECK, false, true);  /* left end */
    CHECK(espdj_resolve_topology(u, 2, &topo));
    CHECK(topo.order[0] == 1 && topo.order[1] == 0);
    CHECK(topo.master == 1);
    CHECK(topo.deck_side[1] == ESPDJ_SIDE_LEFT);
    CHECK(topo.deck_side[0] == ESPDJ_SIDE_RIGHT);

    /* Ctrl|Deck: controller is master even when leftmost is the ctrl. */
    set_unit(&u[0], 1, ESPDJ_UNIT_CONTROLLER, false, true);
    set_unit(&u[1], 2, ESPDJ_UNIT_DECK, true, false);
    CHECK(espdj_resolve_topology(u, 2, &topo));
    CHECK(topo.master == 0);
    CHECK(topo.deck_side[1] == ESPDJ_SIDE_LEFT); /* only deck takes its side */

    /* Deck|Ctrl|Deck: classic dual+mixer. */
    set_unit(&u[0], 1, ESPDJ_UNIT_DECK, false, true);
    set_unit(&u[1], 2, ESPDJ_UNIT_CONTROLLER, true, true);
    set_unit(&u[2], 3, ESPDJ_UNIT_DECK, true, false);
    CHECK(espdj_resolve_topology(u, 3, &topo));
    CHECK(topo.order[0] == 0 && topo.order[1] == 1 && topo.order[2] == 2);
    CHECK(topo.master == 1);
    CHECK(topo.deck_side[0] == ESPDJ_SIDE_LEFT);
    CHECK(topo.deck_side[2] == ESPDJ_SIDE_RIGHT);
    CHECK(topo.deck_side[1] == ESPDJ_SIDE_NONE);

    /* Inconsistent occupancy is rejected. */
    set_unit(&u[0], 1, ESPDJ_UNIT_DECK, false, false);
    set_unit(&u[1], 2, ESPDJ_UNIT_DECK, false, false);
    CHECK(!espdj_resolve_topology(u, 2, &topo));
}

static void test_sense_classify(void)
{
    /* 47k pull-up to 3300 mV: deck 10k -> ~579 mV, ctrl 22k -> ~1052 mV. */
    CHECK(espdj_sense_classify(579, 3300) == ESPDJ_UNIT_DECK);
    CHECK(espdj_sense_classify(1052, 3300) == ESPDJ_UNIT_CONTROLLER);
    CHECK(espdj_sense_classify(3300, 3300) == ESPDJ_UNIT_NONE); /* open */
    CHECK(espdj_sense_classify(0, 3300) == ESPDJ_UNIT_NONE);    /* short */
    /* 30% contact-resistance drift still classifies. */
    CHECK(espdj_sense_classify(579 * 130 / 100, 3300) == ESPDJ_UNIT_DECK);
    CHECK(espdj_sense_classify(1052 * 125 / 100, 3300) == ESPDJ_UNIT_CONTROLLER);
}

int main(void)
{
    test_roundtrip_all_messages();
    test_decoder_resync_on_garbage();
    test_topology();
    test_sense_classify();
    TEST_MAIN_END();
}
