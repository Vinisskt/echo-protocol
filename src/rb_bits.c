#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include "../include/rb_bits.h"

Buffer* rb_init() {
    Buffer *buf = malloc(sizeof(Buffer));
    if (buf == NULL) {
        return NULL;
    }
    atomic_store(&buf->head, 0);
    atomic_store(&buf->tail, 0);
    buf->buf[0] = 0;
    atomic_store(&buf->count_put, 0);
    atomic_store(&buf->count_get, 0);
    return buf;
}

uint8_t put_bits(Buffer *buf, uint8_t *bit) {
    uint16_t h = atomic_load(&buf->head);
    uint16_t t = atomic_load(&buf->tail);
    if (((h + 1) & BUFFER_MASK) == t) {
        fprintf(stderr, "[BUF FULL] tx_rb cheio, perdendo bits!\n");
        return 0;
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
        return 0;
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
    atomic_store(&buf->head, 0);
    atomic_store(&buf->tail, 0);
    buf->buf[0] = 0;
    atomic_store(&buf->count_put, 0);
    atomic_store(&buf->count_get, 0);
}
