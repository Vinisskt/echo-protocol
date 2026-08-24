#include "../include/scrambler.h"

void scrambler_init(Scrambler *s) {
    scrambler_reset(s);
}

void scrambler_reset(Scrambler *s) {
    s->state = 1;  /* non-zero seed: a zero seed locks the LFSR forever */
}

/* Multiplicative self-synchronizing scrambler (x^17 + x^12 + 1).
   Feedback XORs taps 16 and 11; the output is input XOR feedback and the
   register is fed with the output, so it re-syncs after ~17 received bits
   without requiring a shared seed. */
uint8_t scrambler_process(Scrambler *s, uint8_t bit) {
    uint8_t feedback = ((s->state >> SCRAMBLER_TAP_HI) & 1) ^
                       ((s->state >> SCRAMBLER_TAP_LO) & 1);

    s->state = ((s->state << 1) | feedback) &
               ((1u << SCRAMBLER_STATE_BITS) - 1);

    return bit ^ (s->state & 1);
}
