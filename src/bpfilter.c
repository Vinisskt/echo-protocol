#include "../include/bpfilter.h"
#include <math.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846f
#endif

/* Calcula coeficientes de biquad passa-banda (Cookbook RBJ, constant 0 dB peak gain)
 * A largura de banda em Hz ~2000 garante settling rápido (~8 amostras @ 48kHz),
 * menor que o símbolo (24 amostras), sem resíduo entre símbolos. */
void biquad_bandpass_init(BiquadFilter *f, float freq_center, float bw, int sample_rate) {
    float w0 = 2.0f * M_PI * freq_center / (float)sample_rate;
    float Q  = freq_center / bw;                     /* fB0 = center/bw */
    float alpha = sinf(w0) / (2.0f * Q);

    float cos_w0 = cosf(w0);

    float a0 = 1.0f + alpha;
    f->b0 =  alpha / a0;
    f->b1 =  0.0f;
    f->b2 = -alpha / a0;
    f->a1 = -2.0f * cos_w0 / a0;
    f->a2 =  (1.0f - alpha) / a0;

    f->x1 = f->x2 = 0.0f;
    f->y1 = f->y2 = 0.0f;
}

float biquad_process(BiquadFilter *f, float x) {
    /* Direct Form II Transposed */
    float y = f->b0 * x + f->x1;
    f->x1 = f->b1 * x + f->x2 - f->a1 * y;
    f->x2 = f->b2 * x - f->a2 * y;
    return y;
}

void biquad_reset(BiquadFilter *f) {
    f->x1 = f->x2 = 0.0f;
    f->y1 = f->y2 = 0.0f;
}

/* Frequências FSK padrão (devem bater com mod_fsk.h) */
static const float DEFAULT_FSK_FREQS[4] = {
    2000.0f, 6500.0f, 11000.0f, 15000.0f
};

void bandpass_bank_init(BandpassBank *bank, int sample_rate, float bandwidth_hz) {
    for (int i = 0; i < 4; i++) {
        biquad_bandpass_init(&bank->filters[i], DEFAULT_FSK_FREQS[i], bandwidth_hz, sample_rate);
    }
    bank->initialized = 1;
}

void bandpass_bank_process(BandpassBank *bank, float sample, float *out_mags) {
    if (!bank->initialized) return;

    for (int i = 0; i < 4; i++) {
        out_mags[i] = biquad_process(&bank->filters[i], sample);
    }
}

void bandpass_bank_reset(BandpassBank *bank) {
    for (int i = 0; i < 4; i++) {
        biquad_reset(&bank->filters[i]);
    }
}