/*
 * ESP-DJ3000 deck — docked interconnect (design doc §2).
 *
 * DATA: half-duplex multi-drop UART, one wire, 1 Mbps, master-polled.
 * SENSE: per-face neighbor detect + resistor ID (deck 10k / ctrl 22k).
 *
 * Owns discovery, role auto-assignment and (when this deck is master)
 * the 250 Hz poll schedule and 10 Hz beat-phase broadcast. Falls back
 * to ESP-NOW pairing when nothing is docked (see espnow_link).
 */
#pragma once

#include <stdbool.h>

#include "espdj_proto.h"

void bus_link_start(void);

/* Route a transport op to whichever deck owns `side` — locally if that's
 * us, else over the bus / ESP-NOW. Used by the pads relay, the phone
 * web app and the UI. */
void link_route_transport(const espdj_transport_t *t);

/* Latest control-surface state (crossfader etc.) seen on the link;
 * returns false if no controller is present. */
bool link_get_control(espdj_control_t *out);
