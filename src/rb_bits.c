#include <stdint.h>
#include <stdlib.h>
#include <stdatomic.h>
#include "../include/rb_bits.h"

/* Reset all indices/offsets and clear the first byte. Subsequent bytes are
   zeroed lazily by put_bits when the write cursor advances to them, so only
   byte 0 needs clearing here. */
static void rb_clear(Buffer *buf) {
    atomic_store(&buf->head, 0);
    atomic_store(&buf->tail, 0);
    buf->buf[0] = 0;
    atomic_store(&buf->count_put, 0);
    atomic_store(&buf->count_get, 0);
}

Buffer* rb_init(void) {
    Buffer *buf = malloc(sizeof(Buffer));
    if (buf == NULL) {
        return NULL;
    }
    rb_clear(buf);
    return buf;
}

uint8_t put_bits(Buffer *buf, uint8_t *bit) {
    uint16_t h = atomic_load(&buf->head);
    uint16_t t = atomic_load(&buf->tail);
    /* One slot is always kept free so that "full" ((h+1)&MASK == t) can be
       distinguished from "empty" (t == h && cg == cp). */
    if (((h + 1) & BUFFER_MASK) == t) {
        return 0;   /* buffer full: caller must apply backpressure */
    }

    uint8_t cp = atomic_load(&buf->count_put);
    if (cp == 8) {
        h = (h + 1) & BUFFER_MASK;
        atomic_store(&buf->head, h);
        buf->buf[h] = 0;
        atomic_store(&buf->count_put, 0);
        cp = 0;
    }

    buf->buf[h] |= (*bit << cp);
    atomic_store(&buf->count_put, cp + 1);

    return 1;
}

uint8_t get_bits(Buffer *buf, uint8_t *bit) {
    uint16_t h = atomic_load(&buf->head);
    uint16_t t = atomic_load(&buf->tail);

    uint8_t cp = atomic_load(&buf->count_put);
    uint8_t cg = atomic_load(&buf->count_get);

    if (t == h && cg == cp) {
        return 0;   /* empty */
    }

    if (cg == 8) {
        t = (t + 1) & BUFFER_MASK;
        atomic_store(&buf->tail, t);
        atomic_store(&buf->count_get, 0);
        cg = 0;
    }

    *bit = (buf->buf[t] >> cg) & 1;
    atomic_store(&buf->count_get, cg + 1);
    return 1;
}

void rb_reset(Buffer *buf) {
    if (buf == NULL) return;
    rb_clear(buf);
}
