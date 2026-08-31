#ifndef MOD_FSK_H
#define MOD_FSK_H

#include "rb_bits.h"
#include <stdint.h>

#define FREQ_00       2000
#define FREQ_01       3000
#define FREQ_10       4000
#define FREQ_11       5000
#define SYMBOL_RATE   1500
#define SAMPLES_PER_SYMBOL (SAMPLE_RATE / SYMBOL_RATE)

/* Set B: validadores de janela longa (64 amostras = 2 símbolos) */
#define SAMPLES_LONG_WINDOW (SAMPLES_PER_SYMBOL * 2)

/* Set C: frequências dos monitores de banda (ruído broadband) */
#define FREQ_MON_LOW    1000
#define FREQ_MON_MID    1500
#define FREQ_MON_HIGH   6000
#define NUM_FREQ_MON    3

/* Thresholds de validação */
#define VAL_RATIO_THRESHOLD     0.3f   /* mag_valid/mag_main < 30% = confirmado */
#define NOISE_ENERGY_THRESHOLD  0.5f   /* mag_mon/mag_main < 50% = sem ruído */

#ifndef SAMPLE_RATE
#define SAMPLE_RATE   48000
#endif
#ifndef SYNC_WORD
#define SYNC_WORD     0x930B51DE
#endif
#ifndef PREAMBLE
#define PREAMBLE      0xAAAA
#endif

typedef struct StateFSK_s {
    float step_cos[4];
    float step_sin[4];
    float current_cos;
    float current_sin;
    uint16_t sample_count;
} StateFSK;

void pre_calc_fsk(StateFSK *state);
float generate_fsk(StateFSK *state, uint8_t *symbol);
void push_preamble(Buffer *buf);
void push_sync_word(Buffer *buf);

#endif
