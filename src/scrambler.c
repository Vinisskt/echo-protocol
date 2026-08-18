#include "../include/scrambler.h"

void scrambler_init(Scrambler *s) {
    s->state = 1;  // Non-zero initial state
}

void scrambler_reset(Scrambler *s) {
    s->state = 1;
}

// Self-synchronizing scrambler: x^17 + x^12 + 1
// Used in V.34, DOCSIS, 802.11, etc.
// Feed-forward (additive) scrambler - self-synchronizes in ~17 bits
uint8_t scrambler_process(Scrambler *s, uint8_t bit) {
    // Tap bits 17 and 12 (0-indexed: 16 and 11)
    uint8_t feedback = ((s->state >> 16) & 1) ^ ((s->state >> 11) & 1);
    
    // Shift state and insert feedback
    s->state = ((s->state << 1) | feedback) & ((1u << SCRAMBLER_STATE_BITS) - 1);
    
    // XOR input bit with feedback (which is now the LSB of state)
    return bit ^ (s->state & 1);
}