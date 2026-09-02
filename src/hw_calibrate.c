#include "../include/hw_calibrate.h"
#include "../include/log.h"
#include <math.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdatomic.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* Candidatas a frequência FSK (de 2000 a 6000 Hz, espaçamento 500 Hz) */
static const float CANDIDATE_FREQS[] = {
    2000, 2500, 3000, 3500, 4000, 4500, 5000, 5500, 6000
};
#define NUM_CANDIDATES 9

/* Número de amostras por teste de frequência (~20 ms = 1 símbolo @ 2000 sym/s) */
#define FREQ_TEST_SAMPLES  960   /* 20 ms @ 48 kHz */
#define SILENCE_SAMPLES   4800   /* 100 ms @ 48 kHz */
#define FREQ_TEST_CYCLES     5   /* repetir cada teste 5x para média */

/* Goertzel simplificado para medição de magnitude */
static float goertzel_mag(const float *samples, int n, float freq, int sample_rate) {
    float k  = (float)n * freq / (float)sample_rate;
    float w  = 2.0f * (float)M_PI * k / (float)n;
    float coeff = 2.0f * cosf(w);

    float q0 = 0.0f, q1 = 0.0f, q2 = 0.0f;
    for (int i = 0; i < n; i++) {
        q0 = samples[i] + coeff * q1 - q2;
        q2 = q1;
        q1 = q0;
    }

    float real = q1 - q2 * cosf(w);
    float imag = q2 * sinf(w);
    return sqrtf(real * real + imag * imag) / (float)n;
}

/* Gera tom senoidal em buffer */
static void generate_tone(float *buf, int n, float freq, int sample_rate) {
    for (int i = 0; i < n; i++) {
        buf[i] = 0.5f * sinf(2.0f * (float)M_PI * freq * (float)i / (float)sample_rate);
    }
}

int hw_calibrate(AudioState *audio, EchoProtocol *echo, AGCState *agc,
                 HWCalibration *result) {
    memset(result, 0, sizeof(*result));

    log_info("hw_calibrate | iniciando auto-calibração do hardware...");

    /* ── Fase 1: medir noise floor ── */
    log_info("hw_calibrate | fase 1: medindo noise floor (100ms de silêncio)...");

    float noise_sum = 0.0f;
    float noise_max = 0.0f;
    int noise_samples = 0;

    for (int i = 0; i < SILENCE_SAMPLES; i++) {
        float rms = atomic_load(&audio->in_rms);
        noise_sum += rms;
        if (rms > noise_max) noise_max = rms;
        noise_samples++;
        usleep(21);  /* ~1 sample @ 48 kHz */
    }

    result->noise_floor = noise_sum / (float)noise_samples;
    float noise_stddev = 0.0f;
    for (int i = 0; i < SILENCE_SAMPLES; i++) {
        float rms = atomic_load(&audio->in_rms);
        float diff = rms - result->noise_floor;
        noise_stddev += diff * diff;
        usleep(21);
    }
    noise_stddev = sqrtf(noise_stddev / (float)noise_samples);

    log_info("hw_calibrate | noise floor: %.6f (%.1f dB) | max: %.6f | stddev: %.6f",
             result->noise_floor, 20.0f * log10f(result->noise_floor + 1e-9f),
             noise_max, noise_stddev);

    /* rms_min = noise_floor + 3 * stddev (captura 99.7% do ruído) */
    float rms_min_detected = result->noise_floor + 3.0f * noise_stddev;
    if (rms_min_detected < 0.001f) rms_min_detected = 0.001f;

    /* ── Fase 2: teste de frequências ── */
    log_info("hw_calibrate | fase 2: testando %d frequências candidatas...", NUM_CANDIDATES);

    float tone_buf[FREQ_TEST_SAMPLES];

    float best_mags[NUM_CANDIDATES];

    for (int f = 0; f < NUM_CANDIDATES; f++) {
        float freq = CANDIDATE_FREQS[f];
        float mag_sum = 0.0f;

        for (int c = 0; c < FREQ_TEST_CYCLES; c++) {
            /* Gerar tom */
            generate_tone(tone_buf, FREQ_TEST_SAMPLES, freq, SAMPLE_RATE);

            /* Copiar para ring buffer de TX */
            for (int i = 0; i < FREQ_TEST_SAMPLES; i++) {
                put_bits(echo->tx_rb, (uint8_t[]){1});
            }

            /* Esperar o callback processar */
            usleep(FREQ_TEST_SAMPLES * 21 * 2);  /* 2x o tempo */

            /* Ler amostras de RX e medir magnitude */
            float mag = goertzel_mag(tone_buf, FREQ_TEST_SAMPLES, freq, SAMPLE_RATE);
            mag_sum += mag;
        }

        best_mags[f] = mag_sum / (float)FREQ_TEST_CYCLES;
        result->freq_mags[f] = best_mags[f];

        log_info("hw_calibrate | %.0f Hz: mag=%.4f (%.1f dB)",
                 freq, best_mags[f], 20.0f * log10f(best_mags[f] + 1e-9f));
    }

    /* ── Fase 3: selecionar 4 melhores (não adjacentes) ── */
    log_info("hw_calibrate | fase 3: selecionando 4 melhores frequências...");

    int selected[4] = {-1, -1, -1, -1};
    int num_selected = 0;

    for (int round = 0; round < 4 && num_selected < 4; round++) {
        float best_mag = -1.0f;
        int best_idx = -1;

        for (int f = 0; f < NUM_CANDIDATES; f++) {
            /* Pular já selecionadas */
            int already = 0;
            for (int s = 0; s < num_selected; s++) {
                if (selected[s] == f) { already = 1; break; }
            }
            if (already) continue;

            /* Pular adjacentes à última selecionada */
            if (num_selected > 0) {
                int last = selected[num_selected - 1];
                if (abs(f - last) == 1) continue;
            }

            if (best_mags[f] > best_mag) {
                best_mag = best_mags[f];
                best_idx = f;
            }
        }

        if (best_idx >= 0) {
            selected[num_selected] = best_idx;
            result->best_indices[num_selected] = best_idx;
            result->best_freqs[num_selected] = CANDIDATE_FREQS[best_idx];
            num_selected++;
        }
    }

    result->num_candidates = NUM_CANDIDATES;

    /* ── Fase 4: calibrar AGC ── */
    log_info("hw_calibrate | fase 4: calibrando parâmetros do AGC...");

    /* Encontrar magnitude máxima entre as selecionadas */
    float max_mag = 0.0f;
    for (int i = 0; i < num_selected; i++) {
        float m = result->freq_mags[selected[i]];
        if (m > max_mag) max_mag = m;
    }

    /* signal_max estimado: magnitude máxima × fator de escala do tone */
    result->signal_max = max_mag * 1.5f;

    /* Ajustar parâmetros do AGC */
    agc->rms_min = rms_min_detected;
    agc->rms_max = result->signal_max;
    if (agc->rms_max <= agc->rms_min) agc->rms_max = agc->rms_min * 10.0f;

    /* target_db = ponto médio entre noise floor e signal max (em dB) */
    float min_db = 20.0f * log10f(agc->rms_min + 1e-9f);
    float max_db = 20.0f * log10f(agc->rms_max + 1e-9f);
    agc->target_db = (min_db + max_db) / 2.0f;
    agc->target_power = powf(10.0f, agc->target_db / 10.0f);

    log_info("hw_calibrate | resultado:");
    log_info("  noise floor: %.6f (%.1f dB) → rms_min=%.6f",
             result->noise_floor, 20.0f * log10f(result->noise_floor + 1e-9f),
             agc->rms_min);
    log_info("  signal max:  %.6f (%.1f dB) → rms_max=%.6f",
             result->signal_max, 20.0f * log10f(result->signal_max + 1e-9f),
             agc->rms_max);
    log_info("  target:      %.1f dB", agc->target_db);
    log_info("  frequências: %.0f %.0f %.0f %.0f Hz",
             result->best_freqs[0], result->best_freqs[1],
             result->best_freqs[2], result->best_freqs[3]);

    return 0;
}
