#include "../include/mod_afsk.h"
#include <stdint.h>
#include <math.h>

#define SIZE_PREAMBLE (24 - 1)
#define SIZE_SYNC_WORD (32 - 1)

void push_preamble(Buffer *buf) {

	uint8_t bit;
	for (int i = SIZE_PREAMBLE; i >= 0; i--) {
		bit = (PREAMBLE >> i) & 1;
		put_bits(buf, &bit);
	}
	return;
}

void push_sync_word(Buffer *buf) {
	
	uint8_t bit;
	for (int i = SIZE_SYNC_WORD; i >= 0; i--) {
		bit = (SYNC_WORD >> i) & 1;
		put_bits(buf, &bit);
	}
	return;
}

void pre_calc_afsk(StateAFSK *state) {
	float samples_space = (float) (2.0 * M_PI * FREQ_SPACE) / SAMPLE_RATE;
	float samples_mark = (float) (2.0 * M_PI * FREQ_MARK) / SAMPLE_RATE;
	state->step_cos_mark = cosf(samples_mark);
	state->step_cos_space = cosf(samples_space);
	state->step_sin_mark = sinf(samples_mark);
	state->step_sin_space = sinf(samples_space);
	state->current_cos = 1.0f;
	state->current_sin = 0.0f;
	state->sample_count = 0;
	return;
}

float generate_afsk(StateAFSK *state, uint8_t *bit) {
	
	float step_cos = (*bit == 1) ? state->step_cos_mark : state->step_cos_space;
	float step_sin = (*bit == 1) ? state->step_sin_mark : state->step_sin_space;
	
	float next_cos = state->current_cos * step_cos - state->current_sin * step_sin;
	float next_sin = state->current_sin * step_cos + state->current_cos * step_sin;

	state->current_cos = next_cos;
	state->current_sin = next_sin;
	
	return state->current_sin * 0.5f;
}

