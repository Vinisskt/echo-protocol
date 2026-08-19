#include "../include/demod_afsk.h"
#include "../include/mod_afsk.h"
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

void pre_calc_goertzel(StateGoertzel *state, uint16_t *freq) {
	state->n = SAMPLES_PER_SYMBOL;
	state->k = ((float)state->n * *freq) / SAMPLE_RATE;
	state->omega = (2.0f * M_PI * state->k) / state->n;
	state->coeff = 2.0f * cosf(state->omega);
	state->q1 = 0; 
	state->q2 = 0;

	return;
}

float process_goertzel(StateGoertzel *state, float *sample) {
	float q0 = *sample + (state->coeff * state->q1) - state->q2;
	state->q2 = state->q1;
	state->q1 = q0;
	
	return (state->q1 * state->q1) + (state->q2 * state->q2) - (state->q1 * state->q2 * state->coeff);
}

void reset_state(StateGoertzel *state) {
	state->q1 = 0.0f;
	state->q2 = 0.0f;
	return;
}
