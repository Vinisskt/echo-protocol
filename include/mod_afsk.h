#ifndef MOD_AFSK_H
#define MOD_AFSK_H

#include "../include/rb_bits.h"
#include <stdint.h>

#define FREQ_SPACE 1200
#define FREQ_MARK 2400
#define SAMPLE_RATE 48000
#define SYNC_WORD 0x930B51DE
#define BIT_RATE 1200
#define SAMPLES_PER_BIT (SAMPLE_RATE / BIT_RATE)
#define PREAMBLE 1010101010101010

typedef struct {
	float step_cos_space;
	float step_cos_mark;
	float step_sin_space;
	float step_sin_mark;
	float current_sin;
	float current_cos;
	uint16_t sample_count;
} StateAFSK;

void pre_calc_afsk(StateAFSK *state);
void generate_afsk(StateAFSK *state, uint8_t *bit);

#endif // MOD_AFSK_H

