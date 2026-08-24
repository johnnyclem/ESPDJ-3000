/*
 * ESP-DJ3000 deck — minimal streaming WAV reader.
 *
 * v1 accepts 16-bit PCM, 44.1 kHz, mono or stereo (mono duplicated to
 * both channels on read). Also opens the 8-channel interleaved stems
 * variant; the caller picks a stereo pair (or downmix) at read time.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

typedef struct {
    FILE *f;
    uint16_t channels;
    uint32_t sample_rate;
    uint16_t bits;
    uint32_t data_offset;   /* file offset of PCM data */
    uint32_t data_bytes;
    uint32_t total_frames;
} wav_reader_t;

bool wav_open(wav_reader_t *w, const char *path);
void wav_close(wav_reader_t *w);

/* Seek to an absolute frame. */
bool wav_seek_frame(wav_reader_t *w, uint32_t frame);

/* Read up to n frames as stereo interleaved int16 into dst.
 * Mono sources are duplicated L=R; sources with >2 channels are read
 * raw into scratch and channels ch0/ch0+1 are extracted (stems pair).
 * Returns frames delivered (0 at EOF). */
uint32_t wav_read_stereo(wav_reader_t *w, int16_t *dst, uint32_t n,
                         uint16_t ch0, int16_t *scratch, uint32_t scratch_frames);
