#ifndef SCRAMBLER_H
#define SCRAMBLER_H

#include <stdint.h>

/* Multiplicative (self-synchronizing) scrambler, polynomial x^17 + x^12 + 1
   (same generator as AX.25 9k6). The two feedback taps are bits 16 and 11 of
   the 17-bit shift register (0-indexed), per the documented LFSR. */
#define SCRAMBLER_STATE_BITS 17
#define SCRAMBLER_TAP_HI 16
#define SCRAMBLER_TAP_LO 11

typedef struct {
    uint32_t state;
} Scrambler;

void scrambler_init(Scrambler *s);
void scrambler_reset(Scrambler *s);
uint8_t scrambler_process(Scrambler *s, uint8_t bit);

#endif
