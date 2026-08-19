#ifndef RB_BITS_H
#define RB_BITS_H

#include <stdint.h>
#include <stdatomic.h>
#define BUFFER_SIZE 16384

#define BUFFER_MASK (BUFFER_SIZE - 1)

typedef struct {
    uint8_t buf[BUFFER_SIZE];
    atomic_uint_fast16_t head;
    atomic_uint_fast16_t tail;
    atomic_uint_fast8_t count_put;
    atomic_uint_fast8_t count_get;
} Buffer;

Buffer* rb_init();
uint8_t put_bits(Buffer *buf, uint8_t *bit);
uint8_t get_bits(Buffer *buf, uint8_t *bit);
void rb_reset(Buffer *buf);

#endif
