#ifndef DEMOD_AFSK_H
#define DEMOD_AFSK_H

#include "mod_fsk.h"
#include "rb_bits.h"
#include <stdint.h>

typedef struct StateGoertzel_s {
    int n;
    float k, omega, coeff, q1, q2;
    float window_buf[SAMPLES_PER_SYMBOL];
    float hamming[SAMPLES_PER_SYMBOL];
    int buf_idx;
    float last_mag;
} StateGoertzel;

uint8_t check_sync_word(uint32_t *check_word, uint8_t *bit);
void pre_calc_goertzel(StateGoertzel *state, uint16_t *freq);
float process_goertzel(StateGoertzel *state, float *sample);
float process_goertzel_windowed(StateGoertzel *state, float *sample);
void reset_state(StateGoertzel *state);
void sync_correlator_reset(void);

#endif
