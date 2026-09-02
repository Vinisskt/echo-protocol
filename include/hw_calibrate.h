#ifndef HW_CALIBRATE_H
#define HW_CALIBRATE_H

#include "audio_io.h"
#include "echo_protocol.h"
#include "agc.h"

typedef struct {
    float noise_floor;      /* RMS médio do ruído de fundo */
    float signal_max;       /* RMS máximo observado durante teste */
    float best_freqs[4];    /* 4 melhores frequências FSK (Hz) */
    int   num_candidates;   /* número de candidatas testadas */
    float freq_mags[9];     /* magnitude de cada candidata */
    int   best_indices[4];  /* índices das 4 melhores */
} HWCalibration;

/*
 * Auto-calibra o hardware:
 * 1. Mede noise floor (silêncio ~500ms)
 * 2. Testa frequências FSK candidatas (loopback TX→RX)
 * 3. Seleciona as 4 melhores
 * 4. Ajusta parâmetros do AGC
 *
 * Retorna 0 em sucesso, -1 em erro.
 */
int hw_calibrate(AudioState *audio, EchoProtocol *echo, AGCState *agc,
                 HWCalibration *result);

#endif
