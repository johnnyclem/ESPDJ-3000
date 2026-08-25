#include "espdj_ringbuf.h"
#include <string.h>

bool espdj_ringbuf_init(espdj_ringbuf_t *rb, int16_t *storage,
                        size_t capacity_frames)
{
    if (!storage || capacity_frames < 2 ||
        (capacity_frames & (capacity_frames - 1)) != 0) {
        return false;
    }
    rb->data = storage;
    rb->capacity = capacity_frames;
    rb->mask = capacity_frames - 1;
    rb->head = 0;
    rb->tail = 0;
    return true;
}

size_t espdj_ringbuf_used_frames(const espdj_ringbuf_t *rb)
{
    return rb->head - rb->tail;
}

size_t espdj_ringbuf_free_frames(const espdj_ringbuf_t *rb)
{
    return rb->capacity - (rb->head - rb->tail);
}

size_t espdj_ringbuf_write(espdj_ringbuf_t *rb, const int16_t *frames, size_t n)
{
    size_t free = espdj_ringbuf_free_frames(rb);
    if (n > free) n = free;
    size_t head = rb->head;
    for (size_t done = 0; done < n;) {
        size_t idx = (head + done) & rb->mask;
        size_t run = rb->capacity - idx;
        if (run > n - done) run = n - done;
        memcpy(&rb->data[idx * 2], &frames[done * 2], run * 2 * sizeof(int16_t));
        done += run;
    }
    rb->head = head + n; /* publish after the copy */
    return n;
}

size_t espdj_ringbuf_read(espdj_ringbuf_t *rb, int16_t *frames, size_t n)
{
    size_t used = espdj_ringbuf_used_frames(rb);
    if (n > used) n = used;
    size_t tail = rb->tail;
    for (size_t done = 0; done < n;) {
        size_t idx = (tail + done) & rb->mask;
        size_t run = rb->capacity - idx;
        if (run > n - done) run = n - done;
        memcpy(&frames[done * 2], &rb->data[idx * 2], run * 2 * sizeof(int16_t));
        done += run;
    }
    rb->tail = tail + n;
    return n;
}

size_t espdj_ringbuf_skip(espdj_ringbuf_t *rb, size_t n)
{
    size_t used = espdj_ringbuf_used_frames(rb);
    if (n > used) n = used;
    rb->tail += n;
    return n;
}

void espdj_ringbuf_reset(espdj_ringbuf_t *rb)
{
    rb->tail = rb->head;
}
