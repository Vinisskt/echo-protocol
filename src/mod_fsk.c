#include "../include/mod_fsk.h"
#include <math.h>

#define SIZE_PREAMBLE (24 - 1)
#define SIZE_SYNC_WORD (32 - 1)

void push_preamble(Buffer *buf) {
    uint8_t bit;
    for (int i = SIZE_PREAMBLE; i >= 0; i--) {
        bit = (PREAMBLE >> i) & 1;
        put_bits(buf, &bit);
    }
}

void push_sync_word(Buffer *buf) {
    uint8_t bit;
    for (int i = SIZE_SYNC_WORD; i >= 0; i--) {
        bit = (SYNC_WORD >> i) & 1;
        put_bits(buf, &bit);
    }
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
