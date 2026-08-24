#include "../include/mod_fsk.h"
#include <math.h>

#define SIZE_PREAMBLE (16 - 1)
#define SIZE_SYNC_WORD (32 - 1)

/* Push the nbits MSB-first bits of value into the lock-free bit buffer. */
static void push_bits(Buffer *buf, uint32_t value, int nbits) {
    for (int i = nbits - 1; i >= 0; i--) {
        uint8_t bit = (value >> i) & 1;
        put_bits(buf, &bit);
    }
}

void push_preamble(Buffer *buf) {
    push_bits(buf, PREAMBLE, SIZE_PREAMBLE + 1);
}

void push_preamble_n(Buffer *buf, int count) {
    for (int i = 0; i < count; i++) {
        push_preamble(buf);
    }
}

void push_sync_word(Buffer *buf) {
    push_bits(buf, SYNC_WORD, SIZE_SYNC_WORD + 1);
}

void pre_calc_fsk(StateFSK *state) {
    static const uint16_t freqs[4] = {FREQ_00, FREQ_01, FREQ_10, FREQ_11};
    for (int i = 0; i < 4; i++) {
        float f = 2.0f * (float)M_PI * freqs[i] / SAMPLE_RATE;
        state->step_cos[i] = cosf(f);
        state->step_sin[i] = sinf(f);
    }
    state->current_cos = 1.0f;
    state->current_sin = 0.0f;
    state->sample_count = 0;
}

float generate_fsk(StateFSK *state, uint8_t *symbol) {
    uint8_t idx = *symbol & 3;
    float step_cos = state->step_cos[idx];
    float step_sin = state->step_sin[idx];

    float next_cos = state->current_cos * step_cos - state->current_sin * step_sin;
    float next_sin = state->current_sin * step_cos + state->current_cos * step_sin;

    state->current_cos = next_cos;
    state->current_sin = next_sin;

    return state->current_sin * 0.5f;
}
