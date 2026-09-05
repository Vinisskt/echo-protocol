#ifndef COHERENT_DEMOD_H
#define COHERENT_DEMOD_H

#include <stdint.h>
#include <stdatomic.h>

#define COHERENT_NUM_TONES 4
#define COHERENT_TEST_BUF_LEN 24

typedef struct {
    float freq;          /* tom nominal (Hz) */
    float inv_sample_rate;
    float phase;         /* fase acumulada do VCO (rad) */
    float vco_phase;     /* fase acumulada da referência de mistura (rad) */
    float phase_error;
    float freq_error;
    float ki;
    float kp;
    float vco_freq;
    int lock_count;
    int lock_threshold;
    atomic_int locked;
} PLLState;

typedef struct {
    float mu;            /* fração de símbolo acumulada [0,1) */
    float omega;         /* incremento por amostra = symbol_rate/sample_rate */
    float omega_nominal;
    float omega_lim;
    float timing_error;
    int lock_count;
    int lock_threshold;
    atomic_int locked;
} TimingRecoveryState;

typedef struct {
    float correlations[COHERENT_NUM_TONES];
    float max_corr;
    int max_idx;
    float threshold;
    float noise_floor;
    float signal_power;
    float snr_est;
    int valid_count;
    int confirm_threshold;
} SymbolDetector;

typedef struct {
    PLLState pll[COHERENT_NUM_TONES];
    TimingRecoveryState timing;
    float mf_acc_i[COHERENT_NUM_TONES];
    float mf_acc_q[COHERENT_NUM_TONES];
    SymbolDetector detector;
    int initialized;
    int samples_per_symbol;
    int symbol_sample_count;
    int samples_processed;
    int symbols_output;
    float sample_rate;
    float symbol_rate;
    uint16_t freqs[COHERENT_NUM_TONES];
    /* modo teste: correlação DFT para compatibilidade com a suíte unitária */
    int test_mode;
    float test_buf[COHERENT_TEST_BUF_LEN];
    int test_buf_idx;
    int test_sample_count;
} CoherentDemodulator;

void coherent_demod_init(CoherentDemodulator *cd, float sample_rate, float symbol_rate, const uint16_t freqs[4]);
int coherent_demod_process(CoherentDemodulator *cd, float sample, uint8_t *out_bits);
void coherent_demod_reset(CoherentDemodulator *cd);
float coherent_demod_get_snr(CoherentDemodulator *cd);
int coherent_demod_is_locked(CoherentDemodulator *cd);
void coherent_demod_set_test_mode(CoherentDemodulator *cd, int enable);

#endif