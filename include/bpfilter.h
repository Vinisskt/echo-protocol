#ifndef BPFILTER_H
#define BPFILTER_H

#include <stdint.h>

/* Filtro IIR passa-banda biquad (Direct Form II Transposed)
 * 2ª ordem = 2 polos + 2 zeros = 12 dB/octava de rolloff
 * Coeficientes calculados offline para cada frequência central
 */
typedef struct {
    float b0, b1, b2;  /* numerador */
    float a1, a2;      /* denominador (a0 = 1.0) */
    float x1, x2;      /* estado de entrada (Direct Form II) */
    float y1, y2;      /* estado de saída */
} BiquadFilter;

/* Inicializa biquad passa-banda
 * freq_center: frequência central (Hz)
 * bw: largura de banda (Hz) - ex: 500 Hz para filtro estreito
 * sample_rate: taxa de amostragem (Hz)
 */
void biquad_bandpass_init(BiquadFilter *f, float freq_center, float bw, int sample_rate);

/* Processa uma amostra */
float biquad_process(BiquadFilter *f, float x);

/* Reset do estado */
void biquad_reset(BiquadFilter *f);

/* Banco de 4 filtros passa-banda (um por frequência FSK) */
typedef struct {
    BiquadFilter filters[4];
    int initialized;
} BandpassBank;

/* Inicializa banco com as 4 frequências FSK configuradas */
void bandpass_bank_init(BandpassBank *bank, int sample_rate, float bandwidth_hz);

/* Processa amostra através dos 4 filtros, retorna magnitudes */
void bandpass_bank_process(BandpassBank *bank, float sample, float *out_mags);

/* Reset de todos os filtros */
void bandpass_bank_reset(BandpassBank *bank);

#endif