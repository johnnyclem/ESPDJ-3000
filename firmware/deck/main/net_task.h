/*
 * ESP-DJ3000 deck — phone-as-controller (design doc §4).
 *
 * The leftmost/master deck raises SoftAP "ESP-DJ" with a captive portal
 * (zero typing) plus mDNS dj.local for repeat visits. Serves the
 * single-file SPA from LittleFS and a WebSocket control channel
 * (30 Hz client throttle). Ops for the partner deck are relayed over
 * DATA or ESP-NOW via link_route_transport().
 */
#pragma once

void net_task_start(void);
