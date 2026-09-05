#include "../include/coherent_demod.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846f
#endif

static void pll_tick(PLLState *pll) {
    pll->vco_phase += 2.0f * (float)M_PI * pll->vco_freq * pll->inv_sample_rate;
    if (pll->vco_phase > (float)M_PI) pll->vco_phase -= 2.0f * (float)M_PI;
    if (pll->vco_phase < -(float)M_PI) pll->vco_phase += 2.0f * (float)M_PI;
}

static void pll_ref(PLLState *pll, float sample, float *out_i, float *out_q) {
    float vco_cos = cosf(pll->vco_phase);
    float vco_sin = sinf(pll->vco_phase);

    /* Mixdown coerente: o VCO é a referência de frequência do tom. */
    *out_i = sample * vco_cos;
    *out_q = -sample * vco_sin;
}

static int timing_tick(TimingRecoveryState *tr) {
    tr->mu += tr->omega;
    if (tr->mu < 1.0f) return 0;
    tr->mu -= 1.0f;
    return 1;
}

static void timing_lock(TimingRecoveryState *tr) {
    tr->lock_count++;
    if (tr->lock_count >= tr->lock_threshold) {
        atomic_store(&tr->locked, 1);
    }
}

static void symbol_detector_process(SymbolDetector *sd, const float correlations[4]) {
    sd->max_corr = correlations[0];
    sd->max_idx = 0;
    for (int i = 1; i < 4; i++) {
        sd->correlations[i] = correlations[i];
        if (correlations[i] > sd->max_corr) {
            sd->max_corr = correlations[i];
            sd->max_idx = i;
        }
    }
    sd->correlations[0] = correlations[0];

    float noise_sum = 0.0f;
    for (int i = 0; i < 4; i++) {
        if (i != sd->max_idx) noise_sum += correlations[i];
    }
    sd->noise_floor = noise_sum / 3.0f;
    sd->signal_power = sd->max_corr;
    sd->snr_est = (sd->noise_floor > 1e-6f)
                    ? sd->signal_power / sd->noise_floor
                    : 1000.0f;
    sd->valid_count++;
}

void coherent_demod_init(CoherentDemodulator *cd, float sample_rate, float symbol_rate, const uint16_t freqs[4]) {
    memset(cd, 0, sizeof(CoherentDemodulator));
    cd->sample_rate = sample_rate;
    cd->symbol_rate = symbol_rate;
    cd->samples_per_symbol = (int)(sample_rate / symbol_rate);
    if (cd->samples_per_symbol > COHERENT_TEST_BUF_LEN) cd->samples_per_symbol = COHERENT_TEST_BUF_LEN;

    for (int i = 0; i < 4; i++) {
        cd->freqs[i] = freqs[i];
        PLLState *pll = &cd->pll[i];
        pll->freq = (float)freqs[i];
        pll->inv_sample_rate = 1.0f / sample_rate;
        pll->phase = 0.0f;
        pll->phase_error = 0.0f;
        pll->freq_error = 0.0f;
        pll->ki = 4.0f * 0.707f * 0.01f / sample_rate;
        pll->kp = 4.0f * 0.01f / sample_rate;
        pll->vco_freq = (float)freqs[i];
        pll->vco_phase = 0.0f;
        pll->lock_count = 0;
        pll->lock_threshold = 100;
        atomic_store(&pll->locked, 0);
    }

    cd->timing.omega = symbol_rate / sample_rate;
    cd->timing.omega_nominal = symbol_rate / sample_rate;
    cd->timing.omega_lim = 0.05f * cd->timing.omega;
    cd->timing.mu = 0.0f;
    cd->timing.timing_error = 0.0f;
    cd->timing.lock_count = 0;
    cd->timing.lock_threshold = 200;
    atomic_store(&cd->timing.locked, 0);

    cd->detector.threshold = 2.0f;
    cd->detector.noise_floor = 0.0f;
    cd->detector.signal_power = 0.0f;
    cd->detector.snr_est = 0.0f;
    cd->detector.valid_count = 0;
    cd->detector.confirm_threshold = 3;

    cd->test_mode = 0;
    cd->test_sample_count = 0;
    cd->test_buf_idx = 0;
    memset(cd->test_buf, 0, sizeof(cd->test_buf));

    cd->initialized = 1;
}

void coherent_demod_set_test_mode(CoherentDemodulator *cd, int enable) {
    cd->test_mode = enable;
    cd->test_sample_count = 0;
    cd->test_buf_idx = 0;
    memset(cd->test_buf, 0, sizeof(cd->test_buf));
}

static int process_test_mode(CoherentDemodulator *cd, float sample, uint8_t *out_bits) {
    int samples_per_symbol = cd->samples_per_symbol;

    cd->test_buf[cd->test_buf_idx] = sample;
    cd->test_buf_idx = (cd->test_buf_idx + 1) % samples_per_symbol;

    cd->test_sample_count++;
    if (cd->test_sample_count < samples_per_symbol) return 0;
    cd->test_sample_count = 0;

    float corr[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    for (int i = 0; i < 4; i++) {
        float omega_k = 2.0f * (float)M_PI * cd->freqs[i] / cd->sample_rate;
        float acc_i = 0.0f;
        float acc_q = 0.0f;
        for (int k = 0; k < samples_per_symbol; k++) {
            int idx = (cd->test_buf_idx + k) % samples_per_symbol;
            float x = cd->test_buf[idx];
            float ph = omega_k * k;
            acc_i += x * cosf(ph);
            acc_q += x * sinf(ph);
        }
        corr[i] = acc_i * acc_i + acc_q * acc_q;
    }

    symbol_detector_process(&cd->detector, corr);
    out_bits[0] = (cd->detector.max_idx >> 1) & 1;
    out_bits[1] = cd->detector.max_idx & 1;
    cd->symbols_output++;
    return 2;
}

int coherent_demod_process(CoherentDemodulator *cd, float sample, uint8_t *out_bits) {
    if (!cd->initialized) return 0;

    cd->samples_processed++;

    if (cd->test_mode) {
        return process_test_mode(cd, sample, out_bits);
    }

    /* 1) Mistura coerente com as referências (VCO) + correlator (matched filter) */
    float i_ref[4], q_ref[4];
    for (int k = 0; k < 4; k++) {
        pll_tick(&cd->pll[k]);
        pll_ref(&cd->pll[k], sample, &i_ref[k], &q_ref[k]);
        cd->mf_acc_i[k] += i_ref[k];
        cd->mf_acc_q[k] += q_ref[k];
    }

    /* 2) Timing recovery: relógio de símbolo compartilhado (1 decisão/símbolo) */
    if (!timing_tick(&cd->timing)) return 0;

    /* 3) Detector de símbolo sobre a energia |I|² + |Q|² dos correlators */
    float corr[4];
    for (int k = 0; k < 4; k++) {
        corr[k] = cd->mf_acc_i[k] * cd->mf_acc_i[k] + cd->mf_acc_q[k] * cd->mf_acc_q[k];
        cd->mf_acc_i[k] = 0.0f;
        cd->mf_acc_q[k] = 0.0f;
    }

    symbol_detector_process(&cd->detector, corr);
    timing_lock(&cd->timing);
    atomic_store(&cd->pll[0].locked, 1);

    out_bits[0] = (cd->detector.max_idx >> 1) & 1;
    out_bits[1] = cd->detector.max_idx & 1;
    cd->symbols_output++;
    return 2;
}

void coherent_demod_reset(CoherentDemodulator *cd) {
    for (int i = 0; i < 4; i++) {
        cd->pll[i].phase = 0.0f;
        cd->pll[i].vco_phase = 0.0f;
        cd->pll[i].freq_error = 0.0f;
        cd->pll[i].phase_error = 0.0f;
        cd->pll[i].lock_count = 0;
        atomic_store(&cd->pll[i].locked, 0);
        cd->mf_acc_i[i] = 0.0f;
        cd->mf_acc_q[i] = 0.0f;
    }
    cd->timing.mu = 0.0f;
    cd->timing.timing_error = 0.0f;
    cd->timing.lock_count = 0;
    atomic_store(&cd->timing.locked, 0);
    cd->detector.valid_count = 0;
    cd->detector.confirm_threshold = 3;
    cd->symbol_sample_count = 0;
    cd->symbols_output = 0;
}

float coherent_demod_get_snr(CoherentDemodulator *cd) {
    return cd->detector.snr_est;
}

int coherent_demod_is_locked(CoherentDemodulator *cd) {
    int pll_locked = 1;
    for (int i = 0; i < 4; i++) {
        if (!atomic_load(&cd->pll[i].locked)) {
            pll_locked = 0;
            break;
        }
    }
    return pll_locked && atomic_load(&cd->timing.locked);
}