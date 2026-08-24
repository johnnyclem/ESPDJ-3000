/*
 * ESP-DJ3000 deck — track.json sidecar metadata (produced by tools/prep).
 *
 * v1 media policy: 16-bit/44.1k WAV only, zero on-device analysis. All
 * musical knowledge (BPM, beatgrid anchor, key, cues, waveform overview)
 * comes off the SD card next to the WAV.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#define TRACK_MAX_CUES      8
#define TRACK_OVERVIEW_BINS 240 /* one per display degree-ish; ring render */

typedef struct {
    char title[64];
    char artist[64];
    char key[8];            /* Camelot or standard, display only */
    float bpm;              /* grid BPM at 0% pitch */
    double first_beat_s;    /* beatgrid anchor (seconds into the file) */
    int beats_per_bar;
    /* hot cues, seconds; <0 = unset */
    double cues[TRACK_MAX_CUES];
    /* normalized 0..255 peak-ish levels for the ring waveform */
    uint8_t overview[TRACK_OVERVIEW_BINS];
    int overview_len;
    /* stems variant present? (track.8ch.wav, gated on SD bench) */
    bool has_stems;
} track_meta_t;

/* Parse `<wav_path minus .wav>.json`. Returns false and fills sane
 * defaults (120 BPM, anchor 0) if the sidecar is missing or invalid —
 * a bare WAV must still play. */
bool track_meta_load(const char *wav_path, track_meta_t *out);

/* ---- SD library scan ---- */
#define LIBRARY_MAX_TRACKS 256

typedef struct {
    char path[192];
    char title[64];
    char artist[64];
    float bpm;
} library_entry_t;

typedef struct {
    library_entry_t entry[LIBRARY_MAX_TRACKS];
    int count;
} library_t;

/* Scan /sdcard for *.wav (skipping *.8ch.wav stems files), read titles
 * from sidecars where present. */
void library_scan(library_t *lib, const char *root);
