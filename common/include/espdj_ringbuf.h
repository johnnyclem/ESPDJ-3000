/*
 * ESP-DJ3000 — lock-free SPSC ring buffer for stereo int16 frames.
 *
 * The SD reader task (producer) fills 4-8 seconds of decoded audio into
 * PSRAM; the RT audio task (consumer) pulls windows for the resampler.
 * Single producer, single consumer, no locks: indices are only ever
 * advanced by their owning side.
 *
 * Note on scratching: the consumer never *reads* through this API while
 * reversing — the audio task keeps its own contiguous working window
 * (see deck firmware) and only tops it up from the ring.
 */
#ifndef ESPDJ_RINGBUF_H
#define ESPDJ_RINGBUF_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int16_t *data;        /* interleaved L/R, capacity_frames * 2 samples */
    size_t capacity;      /* frames, power of two */
    size_t mask;
    volatile size_t head; /* write index (frames), producer-owned */
    volatile size_t tail; /* read index (frames), consumer-owned */
} espdj_ringbuf_t;

/* `storage` must hold capacity_frames * 2 int16s; capacity power of two. */
bool espdj_ringbuf_init(espdj_ringbuf_t *rb, int16_t *storage,
                        size_t capacity_frames);

size_t espdj_ringbuf_free_frames(const espdj_ringbuf_t *rb);
size_t espdj_ringbuf_used_frames(const espdj_ringbuf_t *rb);

/* Producer: copy up to n frames in; returns frames written. */
size_t espdj_ringbuf_write(espdj_ringbuf_t *rb, const int16_t *frames, size_t n);

/* Consumer: copy up to n frames out; returns frames read. */
size_t espdj_ringbuf_read(espdj_ringbuf_t *rb, int16_t *frames, size_t n);

/* Consumer: drop up to n frames; returns frames dropped. */
size_t espdj_ringbuf_skip(espdj_ringbuf_t *rb, size_t n);

/* Both sides quiesced (e.g. track load / seek): empty the ring. */
void espdj_ringbuf_reset(espdj_ringbuf_t *rb);

#ifdef __cplusplus
}
#endif

#endif /* ESPDJ_RINGBUF_H */
