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

/* Goertzel de janela longa (64 amostras) — 2x resolução de frequência.
   Usado como Set B para validar decisões do Set A. */
typedef struct StateGoertzelLong_s {
    int n;
    float k, omega, coeff, q1, q2;
    float window_buf[SAMPLES_LONG_WINDOW];
    float hamming[SAMPLES_LONG_WINDOW];
    int buf_idx;
    float last_mag;
} StateGoertzelLong;

uint8_t check_sync_word(uint32_t *check_word, uint8_t *bit);
void pre_calc_goertzel(StateGoertzel *state, uint16_t *freq);
float process_goertzel(StateGoertzel *state, float *sample);
float process_goertzel_windowed(StateGoertzel *state, float *sample);
void reset_state(StateGoertzel *state);

void pre_calc_goertzel_long(StateGoertzelLong *state, uint16_t *freq);
float process_goertzel_windowed_long(StateGoertzelLong *state, float *sample);
float process_goertzel_buffer_long(StateGoertzelLong *state, const float *buf, int len);
void reset_state_long(StateGoertzelLong *state);

void sync_correlator_reset(void);

#endif
