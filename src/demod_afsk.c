#include "../include/demod_afsk.h"
#include "../include/mod_fsk.h"
#include <stdint.h>
#include <math.h>

#define SYNC_THRESHOLD 30

static uint32_t corr_reg = 0;
static int corr_bits = 0;

void sync_correlator_reset(void) {
    corr_reg = 0;
    corr_bits = 0;
}

uint8_t check_sync_word(uint32_t *check_word, uint8_t *bit) {

	*check_word = (*check_word << 1) | *bit;

	if (*check_word == SYNC_WORD) {
		sync_correlator_reset();  /* reset correlation on exact match */
		return 0;
	}

	corr_reg = ((corr_reg << 1) | *bit) & 0xFFFFFFFF;
	corr_bits = (corr_bits < 32) ? corr_bits + 1 : 32;

	if (corr_bits == 32) {
		/* Quick pre-filter: sync word has 16 ones, 16 zeros. Reject if popcount far off. */
		int pop = __builtin_popcount(corr_reg);
		if (pop < 10 || pop > 22) return 1;  /* sync word has 16 ones */
		
		uint32_t diff = corr_reg ^ SYNC_WORD;
		int matches = 32 - __builtin_popcount(diff);
		if (matches >= SYNC_THRESHOLD) {
			sync_correlator_reset();  /* reset correlation on correlation match too */
			return 0;
		}
	}
	return 1;
}

static void init_hamming_window(StateGoertzel *state) {
    for (int i = 0; i < state->n; i++) {
        state->hamming[i] = 0.54f - 0.46f * cosf(2.0f * M_PI * i / (state->n - 1));
    }
}

void pre_calc_goertzel(StateGoertzel *state, uint16_t *freq) {
    state->n = SAMPLES_PER_SYMBOL;
    state->k = ((float)state->n * *freq) / SAMPLE_RATE;
    state->omega = (2.0f * M_PI * state->k) / state->n;
    state->coeff = 2.0f * cosf(state->omega);
    state->q1 = 0.0f;
    state->q2 = 0.0f;
    state->buf_idx = 0;
    state->last_mag = 0.0f;
    init_hamming_window(state);
}

float process_goertzel_windowed(StateGoertzel *state, float *sample) {
    state->window_buf[state->buf_idx] = *sample * state->hamming[state->buf_idx];
    state->buf_idx++;

    if (state->buf_idx < state->n) {
        return state->last_mag;
    }

    float q1 = 0.0f;
    float q2 = 0.0f;
    for (int i = 0; i < state->n; i++) {
        float q0 = state->window_buf[i] + (state->coeff * q1) - q2;
        q2 = q1;
        q1 = q0;
    }

    float mag = (q1 * q1) + (q2 * q2) - (q1 * q2 * state->coeff);
    state->last_mag = mag;

    state->buf_idx = 0;
    return mag;
}

void reset_state(StateGoertzel *state) {
    state->q1 = 0.0f;
    state->q2 = 0.0f;
    state->buf_idx = 0;
    state->last_mag = 0.0f;
}

/* --- Goertzel de janela longa (64 amostras) — Set B de validação --- */

static void init_hamming_window_long(StateGoertzelLong *state) {
    for (int i = 0; i < state->n; i++) {
        state->hamming[i] = 0.54f - 0.46f * cosf(2.0f * M_PI * i / (state->n - 1));
    }
}

void pre_calc_goertzel_long(StateGoertzelLong *state, uint16_t *freq) {
    state->n = SAMPLES_LONG_WINDOW;
    state->k = ((float)state->n * *freq) / SAMPLE_RATE;
    state->omega = (2.0f * M_PI * state->k) / state->n;
    state->coeff = 2.0f * cosf(state->omega);
    state->q1 = 0.0f;
    state->q2 = 0.0f;
    state->buf_idx = 0;
    state->last_mag = 0.0f;
    init_hamming_window_long(state);
}

float process_goertzel_buffer_long(StateGoertzelLong *state, const float *buf, int len) {
    float q1 = 0.0f;
    float q2 = 0.0f;
    for (int i = 0; i < len; i++) {
        float windowed = buf[i] * state->hamming[i];
        float q0 = windowed + (state->coeff * q1) - q2;
        q2 = q1;
        q1 = q0;
    }
    float mag = (q1 * q1) + (q2 * q2) - (q1 * q2 * state->coeff);
    state->last_mag = mag;
    return mag;
}
