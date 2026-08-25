/*
 * ESP-DJ3000 controller — bus master.
 *
 * The controller, when present, is always bus master (§2): it answers
 * discovery, runs the 250 Hz control-frame broadcast (~8 bytes -> ~500x
 * bus headroom), polls decks for state, and relays pad events.
 */
#pragma once

#include "espdj_proto.h"

void bus_master_start(void);
void bus_master_send_control(const espdj_control_t *c);
void bus_master_send_pad(const espdj_pad_event_t *p);
void bus_master_send_transport(const espdj_transport_t *t);

/* Latest polled state per deck side (for pad LED feedback). */
bool bus_master_deck_state(espdj_side_t side, espdj_deck_state_t *out);
