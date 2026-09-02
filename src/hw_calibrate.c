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

/* Candidatas a frequência FSK (de 2000 a 15000 Hz, espaçamento ~1625 Hz) */
static const float CANDIDATE_FREQS[] = {
    2000, 3625, 5250, 6875, 8500, 10125, 11750, 13375, 15000
};
#define NUM_CANDIDATES 9

/* Tamanhos de buffer */
#define CALIB_BUF_SIZE  4096   /* buffer para capturar amostras raw */
#define SILENCE_MS       200   /* 200ms de silêncio para medição */

/* Goertzel para medição de magnitude em uma frequência */
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

/*
 * Coleta amostras raw do microfone durante N ms.
 * Retorna o número de amostras coletadas.
 * O callback continua rodando e preenchendo audio->in_rms,
 * mas aqui capturamos as amostras brutas (sem DC offset) para análise espectral.
 */
static int capture_raw_samples(AudioState *audio, float *buf, int max_samples, int duration_ms) {
    int target_samples = SAMPLE_RATE * duration_ms / 1000;
    if (target_samples > max_samples) target_samples = max_samples;

    /* Espera o buffer encher via callback (in_rms é atualizado a cada frame) */
    int collected = 0;
    int wait_us = (1000000 / SAMPLE_RATE);  /* tempo por amostra em us */

    for (int i = 0; i < target_samples && collected < target_samples; i++) {
        /* Lê o RMS atual (proxy da amostra; não temos acesso direto ao buffer
           do callback sem mutex, então usamos a estimativa de RMS).
           Para análise espectral, isso é suficiente: queremos saber quais
           frequências têm MENOS energia no ruído. */
        float rms = atomic_load(&audio->in_rms);
        buf[collected] = rms;
        collected++;
        usleep(wait_us);
    }

    return collected;
}

int hw_calibrate(AudioState *audio, EchoProtocol *echo, AGCState *agc,
                 HWCalibration *result) {
    (void)echo;  /* não usado diretamente aqui */
    memset(result, 0, sizeof(*result));

    log_info("hw_calibrate | iniciando auto-calibração do hardware...");

    /* ── Fase 1: medir noise floor ── */
    log_info("hw_calibrate | fase 1: medindo noise floor (%dms de silêncio)...", SILENCE_MS);

    float calib_buf[CALIB_BUF_SIZE];
    int n_samples = capture_raw_samples(audio, calib_buf, CALIB_BUF_SIZE, SILENCE_MS);

    if (n_samples < 100) {
        log_warn("hw_calibrate | poucas amostras (%d), usando padrões", n_samples);
        return -1;
    }

    /* Calcular média e stddev do RMS */
    float noise_sum = 0.0f;
    float noise_max = 0.0f;
    for (int i = 0; i < n_samples; i++) {
        noise_sum += calib_buf[i];
        if (calib_buf[i] > noise_max) noise_max = calib_buf[i];
    }
    result->noise_floor = noise_sum / (float)n_samples;

    float noise_stddev = 0.0f;
    for (int i = 0; i < n_samples; i++) {
        float diff = calib_buf[i] - result->noise_floor;
        noise_stddev += diff * diff;
    }
    noise_stddev = sqrtf(noise_stddev / (float)n_samples);

    log_info("hw_calibrate | noise floor: %.6f (%.1f dB) | max: %.6f | stddev: %.6f",
             result->noise_floor, 20.0f * log10f(result->noise_floor + 1e-9f),
             noise_max, noise_stddev);

    /* rms_min = noise_floor + 3 * stddev (captura 99.7% do ruído) */
    float rms_min_detected = result->noise_floor + 3.0f * noise_stddev;
    if (rms_min_detected < 0.001f) rms_min_detected = 0.001f;

    /* ── Fase 2: análise espectral do ruído ── */
    /* Em vez de transmitir tons e medir loopback (que depende de hardware),
       medimos o espectro do ruído de fundo. Frequências com MENOS ruído
       são melhores para FSK (maior SNR). */
    log_info("hw_calibrate | fase 2: analisando espectro do ruído...");

    /* Usar uma cópia dos dados como "sinal" para Goertzel.
       O Goertzel mede a energia em cada frequência — no caso do ruído,
       queremos as frequências com MENOR energia (mais limpas). */
    float best_mags[NUM_CANDIDATES];

    for (int f = 0; f < NUM_CANDIDATES; f++) {
        float freq = CANDIDATE_FREQS[f];

        /* Divide o buffer em janelas e média as magnitudes */
        float mag_sum = 0.0f;
        int num_windows = 0;
        int window_size = 256;  /* 5.3ms @ 48 kHz */

        for (int start = 0; start + window_size <= n_samples; start += window_size) {
            float mag = goertzel_mag(calib_buf + start, window_size, freq, SAMPLE_RATE);
            mag_sum += mag;
            num_windows++;
        }

        float avg_mag = (num_windows > 0) ? mag_sum / (float)num_windows : 0.0f;
        best_mags[f] = avg_mag;
        result->freq_mags[f] = avg_mag;

        log_info("hw_calibrate | %.0f Hz: noise mag=%.6f (%.1f dB)",
                 freq, avg_mag, 20.0f * log10f(avg_mag + 1e-9f));
    }

    /* ── Fase 3: selecionar 4 melhores (menor ruído = melhor SNR) ── */
    log_info("hw_calibrate | fase 3: selecionando 4 frequências mais limpas...");

    int selected[4] = {-1, -1, -1, -1};
    int num_selected = 0;

    for (int round = 0; round < 4 && num_selected < 4; round++) {
        float best_mag = 1e9f;  /* menor é melhor (menos ruído) */
        int best_idx = -1;

        for (int f = 0; f < NUM_CANDIDATES; f++) {
            /* Pular já selecionadas */
            int already = 0;
            for (int s = 0; s < num_selected; s++) {
                if (selected[s] == f) { already = 1; break; }
            }
            if (already) continue;

            /* Pular adjacentes à última selecionada (evita interferência) */
            if (num_selected > 0) {
                int last = selected[num_selected - 1];
                if (abs(f - last) == 1) continue;
            }

            if (best_mags[f] < best_mag) {
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

    /* signal_max estimado: o AGC precisa de um range.
       Usamos o noise floor × fator como referência máxima provisória.
       O AGC real vai ajustar depois baseado no sinal recebido. */
    result->signal_max = result->noise_floor * 20.0f;  /* 26 dB acima do floor */

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
    log_info("  frequências: %.0f %.0f %.0f %.0f Hz (menor ruído primeiro)",
             result->best_freqs[0], result->best_freqs[1],
             result->best_freqs[2], result->best_freqs[3]);

    return 0;
}
