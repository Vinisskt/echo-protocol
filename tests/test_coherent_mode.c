#include <stdio.h>
#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include "../include/echo_protocol.h"
#include "../include/mod_fsk.h"
#include "../include/coherent_demod.h"

static int tests_passed = 0;
static int tests_failed = 0;

#define TEST(name) do { printf("  - %s ... ", name); } while(0)
#define PASS() do { printf("PASS\n"); tests_passed++; } while(0)
#define FAIL(fmt, ...) do { printf("FAIL: " fmt "\n", ##__VA_ARGS__); tests_failed++; } while(0)

/* Gera rng gaussiana (Box-Muller) */
static double gauss_rand(void) {
    double u1 = (double)rand() / ((double)RAND_MAX + 1.0);
    double u2 = (double)rand() / ((double)RAND_MAX + 1.0);
    if (u1 < 1e-12) u1 = 1e-12;
    return sqrt(-2.0 * log(u1)) * cos(2.0 * M_PI * u2);
}

/* Modula símbolos em 24 amostras/símbolo, fase contínua. Retorna nº amostras.
 * se sigma > 0, soma ruído AWGN. */
static int modulate_symbols(const uint8_t *symbols, int num_symbols, float sigma,
                            float *out, int max_samples) {
    StateFSK tx;
    pre_calc_fsk(&tx);
    int n = 0;
    for (int s = 0; s < num_symbols; s++) {
        uint8_t sym = symbols[s] & 3;
        for (int k = 0; k < SAMPLES_PER_SYMBOL; k++) {
            if (n >= max_samples) return n;
            float v = generate_fsk(&tx, &sym);
            if (sigma > 0.0f) v += (float)gauss_rand() * sigma;
            out[n++] = v;
        }
    }
    return n;
}

static int demodulate_symbols_production(const float *samples, int num_samples,
                                         uint8_t *out_bits, int max_bits) {
    uint16_t freqs[4] = {FREQ_00, FREQ_01, FREQ_10, FREQ_11};
    CoherentDemodulator cd;
    coherent_demod_init(&cd, SAMPLE_RATE, SYMBOL_RATE, freqs);
    int nbits = 0;
    for (int i = 0; i < num_samples; i++) {
        uint8_t bits[2] = {0, 0};
        int ready = coherent_demod_process(&cd, samples[i], bits);
        if (ready >= 2 && nbits + 2 <= max_bits) {
            out_bits[nbits++] = bits[0];
            out_bits[nbits++] = bits[1];
        }
    }
    return nbits;
}

static void symbol_to_bits(uint8_t sym, uint8_t *b0, uint8_t *b1) {
    *b0 = (sym >> 1) & 1;
    *b1 = sym & 1;
}

void test_coherent_clean_no_errors() {
    TEST("coherent production: clean signal -> 0 bit errors");
    srand(1);
    const int NSYM = 1000;
    uint8_t symbols[NSYM];
    for (int i = 0; i < NSYM; i++) symbols[i] = (uint8_t)(rand() & 3);

    float samples[NSYM * SAMPLES_PER_SYMBOL];
    int n = modulate_symbols(symbols, NSYM, 0.0f, samples, sizeof(samples)/sizeof(samples[0]));
    if (n != NSYM * SAMPLES_PER_SYMBOL) { FAIL("modulation truncated"); return; }

    uint8_t bits[NSYM * 2];
    int nb = demodulate_symbols_production(samples, n, bits, sizeof(bits));
    if (nb != NSYM * 2) { FAIL("expected %d bits, got %d", NSYM*2, nb); return; }

    int err = 0;
    for (int i = 0; i < NSYM; i++) {
        uint8_t b0, b1;
        symbol_to_bits(symbols[i], &b0, &b1);
        if (bits[i*2] != b0) err++;
        if (bits[i*2+1] != b1) err++;
    }
    if (err > 0) { FAIL("%d/%d bit errors", err, NSYM*2); return; }
    PASS();
}

void test_coherent_medium_noise_low_ber() {
    TEST("coherent production: moderate noise -> low BER");
    srand(2);
    const int NSYM = 2000;
    uint8_t symbols[NSYM];
    for (int i = 0; i < NSYM; i++) symbols[i] = (uint8_t)(rand() & 3);

    float samples[NSYM * SAMPLES_PER_SYMBOL];
    float sigma = 0.22f;
    int n = modulate_symbols(symbols, NSYM, sigma, samples, sizeof(samples)/sizeof(samples[0]));

    uint8_t bits[NSYM * 2];
    int nb = demodulate_symbols_production(samples, n, bits, sizeof(bits));
    if (nb != NSYM * 2) { FAIL("expected %d bits, got %d", NSYM*2, nb); return; }

    int err = 0;
    for (int i = 0; i < NSYM; i++) {
        uint8_t b0, b1;
        symbol_to_bits(symbols[i], &b0, &b1);
        if (bits[i*2] != b0) err++;
        if (bits[i*2+1] != b1) err++;
    }
    double ber = (double)err / (NSYM * 2);
    if (ber > 0.05) { FAIL("BER %.4f > 5%% com ruído moderado", ber); return; }
    PASS();
}

void test_coherent_horrible_noise_below_random() {
    TEST("coherent production: heavy noise -> BER well below random (50%)");
    srand(3);
    const int NSYM = 3000;
    uint8_t symbols[NSYM];
    for (int i = 0; i < NSYM; i++) symbols[i] = (uint8_t)(rand() & 3);

    float samples[NSYM * SAMPLES_PER_SYMBOL];
    float sigma = 0.60f;
    int n = modulate_symbols(symbols, NSYM, sigma, samples, sizeof(samples)/sizeof(samples[0]));

    uint8_t bits[NSYM * 2];
    int nb = demodulate_symbols_production(samples, n, bits, sizeof(bits));
    if (nb != NSYM * 2) { FAIL("expected %d bits, got %d", NSYM*2, nb); return; }

    int err = 0;
    for (int i = 0; i < NSYM; i++) {
        uint8_t b0, b1;
        symbol_to_bits(symbols[i], &b0, &b1);
        if (bits[i*2] != b0) err++;
        if (bits[i*2+1] != b1) err++;
    }
    double ber = (double)err / (NSYM * 2);
    if (ber > 0.30) { FAIL("BER %.4f > 30%% com ruído forte", ber); return; }
    PASS();
}

void test_coherent_phase_offset_robust() {
    TEST("coherent production: receiver PLL phases offset -> no impact (|I|^2+|Q|^2)");
    srand(4);
    const int NSYM = 1000;
    uint8_t symbols[NSYM];
    for (int i = 0; i < NSYM; i++) symbols[i] = (uint8_t)(rand() & 3);

    float samples[NSYM * SAMPLES_PER_SYMBOL];
    int n = modulate_symbols(symbols, NSYM, 0.0f, samples, sizeof(samples)/sizeof(samples[0]));

    uint16_t freqs[4] = {FREQ_00, FREQ_01, FREQ_10, FREQ_11};
    CoherentDemodulator cd;
    coherent_demod_init(&cd, SAMPLE_RATE, SYMBOL_RATE, freqs);
    for (int i = 0; i < 4; i++) {
        cd.pll[i].vco_phase = 1.3f + (float)i * 0.7f;
        cd.pll[i].phase = cd.pll[i].vco_phase;
    }

    uint8_t bits[NSYM * 2];
    int nb = 0;
    for (int i = 0; i < n; i++) {
        uint8_t out[2] = {0, 0};
        int ready = coherent_demod_process(&cd, samples[i], out);
        if (ready >= 2 && nb + 2 <= (int)sizeof(bits)) {
            bits[nb++] = out[0];
            bits[nb++] = out[1];
        }
    }
    if (nb != NSYM * 2) { FAIL("expected %d bits, got %d", NSYM*2, nb); return; }
    int err = 0;
    for (int i = 0; i < NSYM; i++) {
        uint8_t b0, b1;
        symbol_to_bits(symbols[i], &b0, &b1);
        if (bits[i*2] != b0) err++;
        if (bits[i*2+1] != b1) err++;
    }
    if (err > 0) { FAIL("%d/%d bit errors", err, NSYM*2); return; }
    PASS();
}

void test_coherent_mode_matches_test_mode() {
    TEST("coherent production == DFT test mode on clean signal");
    srand(5);
    const int NSYM = 500;
    uint8_t symbols[NSYM];
    for (int i = 0; i < NSYM; i++) symbols[i] = (uint8_t)(rand() & 3);

    float samples[NSYM * SAMPLES_PER_SYMBOL];
    int n = modulate_symbols(symbols, NSYM, 0.0f, samples, sizeof(samples)/sizeof(samples[0]));

    uint8_t bits_prod[NSYM * 2];
    int nb1 = demodulate_symbols_production(samples, n, bits_prod, sizeof(bits_prod));

    uint16_t freqs[4] = {FREQ_00, FREQ_01, FREQ_10, FREQ_11};
    CoherentDemodulator cdt;
    coherent_demod_init(&cdt, SAMPLE_RATE, SYMBOL_RATE, freqs);
    coherent_demod_set_test_mode(&cdt, 1);
    uint8_t bits_test[NSYM * 2];
    int nb2 = 0;
    for (int i = 0; i < n; i++) {
        uint8_t bits[2] = {0, 0};
        int ready = coherent_demod_process(&cdt, samples[i], bits);
        if (ready >= 2 && nb2 + 2 <= (int)sizeof(bits_test)) {
            bits_test[nb2++] = bits[0];
            bits_test[nb2++] = bits[1];
        }
    }
    if (nb1 != NSYM * 2 || nb2 != NSYM * 2) {
        FAIL("bit counts differ: prod=%d test=%d", nb1, nb2);
        return;
    }
    int diff = 0;
    for (int i = 0; i < nb1; i++) if (bits_prod[i] != bits_test[i]) diff++;
    if (diff > 0) { FAIL("%d/%d bits differente entre modos", diff, nb1); return; }
    PASS();
}

void test_coherent_end_to_end_sync_production() {
    TEST("audio_to_rb: SEARCHING -> DATA em modo produção (coerente)");
    EchoProtocol echo;
    memset(&echo, 0, sizeof(echo));
    echo.rx_rb = rb_init();
    echo.rx.state = SEARCHING;
    echo.rx.sync_accumulator = 0;
    echo.rx.rx_sample_count = 0;
    echo.rx.bits_received = 0;
    echo.rx.packet_len = 0;
    echo.rx.header_accumulator = 0;
    echo.rx.last_rx_time = 0;
    uint16_t freqs[4] = {FREQ_00, FREQ_01, FREQ_10, FREQ_11};
    for (int i = 0; i < 4; i++) {
        pre_calc_goertzel(&echo.freq_states[i], &freqs[i]);
        pre_calc_goertzel_long(&echo.freq_valid[i], &freqs[i]);
    }
    uint16_t mon_freqs[NUM_FREQ_MON] = {FREQ_MON_LOW, FREQ_MON_MID, FREQ_MON_HIGH};
    for (int i = 0; i < NUM_FREQ_MON; i++) {
        pre_calc_goertzel(&echo.freq_mon[i], &mon_freqs[i]);
    }
    bandpass_bank_init(&echo.bp_bank, SAMPLE_RATE, 2000.0f);
    coherent_demod_init(&echo.coh_demod, SAMPLE_RATE, SYMBOL_RATE, freqs);
    echo.pending_candidate = -1;
    echo.block_counter = 0;
    echo.long_samples_count = 0;

    /* TX: preamble + sync word em bits, modulados por generate_fsk */
    Buffer *txb = rb_init();
    push_preamble(txb);
    push_sync_word(txb);
    StateFSK tx;
    pre_calc_fsk(&tx);
    uint8_t bit;
    while (get_bits(txb, &bit) == 1) {
        uint8_t b0 = (uint8_t)bit;
        uint8_t b1 = 0;
        int ok = get_bits(txb, &b1);
        uint8_t sym = (b0 << 1) | b1;
        if (!ok) sym = 3;
        for (int k = 0; k < SAMPLES_PER_SYMBOL; k++) {
            float sample = generate_fsk(&tx, &sym);
            audio_to_rb(&echo, &sample);
        }
    }
    if (echo.rx.state != DATA) { FAIL("não transicionou para DATA"); free(echo.rx_rb); free(txb); return; }
    PASS();
    free(echo.rx_rb);
    free(txb);
}

int main(void) {
    srand((unsigned)time(NULL));
    test_coherent_clean_no_errors();
    test_coherent_medium_noise_low_ber();
    test_coherent_horrible_noise_below_random();
    test_coherent_phase_offset_robust();
    test_coherent_mode_matches_test_mode();
    test_coherent_end_to_end_sync_production();
    printf("\nResultados: %d PASS, %d FAIL\n", tests_passed, tests_failed);
    return tests_failed ? 1 : 0;
}