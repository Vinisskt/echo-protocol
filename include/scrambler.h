#ifndef SCRAMBLER_H
#define SCRAMBLER_H

#include <stdint.h>

#define SCRAMBLER_STATE_BITS 17
#define SCRAMBLER_POLY 0x12000  // x^17 + x^12 + 1 (bits 17 and 12)

typedef struct {
    uint32_t state;
} Scrambler;

void scrambler_init(Scrambler *s);
void scrambler_reset(Scrambler *s);
uint8_t scrambler_process(Scrambler *s, uint8_t bit);

#endif