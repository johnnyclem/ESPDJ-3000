/*
 * ESP-DJ3000 deck — wireless deck-to-deck mode (design doc §2).
 *
 * Two undocked decks pair over ESP-NOW: same control frames, same
 * beat-sync messages, ~2-5 ms typical latency. Each deck uses its own
 * audio outs (no bus to sum on). Also the graceful fallback if a pogo
 * DATA contact goes flaky.
 *
 * Pairing: while active and unpaired, broadcast our ANNOUNCE at 2 Hz on
 * the ESP-NOW broadcast address; first deck ANNOUNCE heard becomes the
 * peer. Lower MAC takes Left/sync-master, so both sides agree with no
 * extra round trip.
 */
#pragma once

#include <stdbool.h>

#include "espdj_proto.h"

void espnow_link_start(void);

/* Enabled while nothing is docked (bus task drives this). */
void espnow_link_set_active(bool active);

bool espnow_link_paired(void);
void espnow_link_send_frame(const espdj_frame_t *f);
void espnow_link_broadcast_beat(void);
