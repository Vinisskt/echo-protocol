#include "../include/demod_afsk.h"
#include "../include/mod_afsk.h"
#include <stdint.h>
#include <math.h>

uint8_t check_sync_word(uint32_t *check_word, uint8_t *bit) {

	*check_word = (*check_word << 1) | *bit;

	if (*check_word == SYNC_WORD) {
		return 0;
	}

	return 1;
}

void pre_calc_goertzel(StateGoertzel *state, uint16_t *freq) {
	state->n = SAMPLES_PER_BIT;
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
