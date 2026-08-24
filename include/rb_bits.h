#ifndef RB_BITS_H
#define RB_BITS_H

#include <stdint.h>
#include <stdatomic.h>

#define BUFFER_SIZE 16384
#define BUFFER_MASK (BUFFER_SIZE - 1)
#define RB_CACHE_LINE 64

typedef struct {
    uint8_t buf[BUFFER_SIZE];
    /* Producer-owned: written only by the single producer thread. */
    _Alignas(RB_CACHE_LINE) atomic_uint_fast16_t head;
    atomic_uint_fast8_t count_put;
    /* Consumer-owned: written only by the single consumer thread. A separate
       cache line avoids false sharing with the producer fields. */
    _Alignas(RB_CACHE_LINE) atomic_uint_fast16_t tail;
    atomic_uint_fast8_t count_get;
} Buffer;

Buffer* rb_init(void);
uint8_t put_bits(Buffer *buf, uint8_t *bit);
uint8_t get_bits(Buffer *buf, uint8_t *bit);
void rb_reset(Buffer *buf);

#endif
