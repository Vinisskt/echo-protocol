#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include <stdatomic.h>
#include "../include/agc.h"
#include "../include/echo_protocol.h"
#include "../include/audio_io.h"

static int tests_passed = 0;
static int tests_failed = 0;

#define TEST(name) do { printf("  - %s ... ", name); } while(0)
#define PASS() do { printf("PASS\n"); tests_passed++; } while(0)
#define FAIL(fmt, ...) do { printf("FAIL: " fmt "\n", ##__VA_ARGS__); tests_failed++; } while(0)

static AudioState mock_audio(float tx_gain, float rx_gain, float rms) {
    AudioState a = {0};
    a.tx_gain = tx_gain;
    a.rx_gain = rx_gain;
    atomic_init(&a.in_rms, rms);
    a.agc_freeze = 0;
    return a;
}

static EchoProtocol mock_echo(uint64_t sync, uint64_t pkts, uint64_t corrupt) {
    EchoProtocol e = {0};
    e.stats.rx_sync_found = sync;
    e.stats.rx_packets = pkts;
    e.stats.rx_corrupted = corrupt;
    return e;
}

static float test_linear_to_db(float linear) {
    if (linear <= 0.0f) return -120.0f;
    return 20.0f * log10f(linear);
}

static float test_db_to_linear(float db) {
    return powf(10.0f, db / 20.0f);
}

/* ── Init ── */

void test_agc_init_defaults() {
    TEST("agc_init sets default configuration");
    AGCState agc;
    agc_init(&agc);

    if (agc.tx_gain_db_min != -20.0f) { FAIL("tx_gain_db_min"); return; }
    if (agc.tx_gain_db_max != 10.0f)  { FAIL("tx_gain_db_max"); return; }
    if (agc.rx_gain_db_min != 0.0f)   { FAIL("rx_gain_db_min"); return; }
    if (agc.rx_gain_db_max != 24.0f)  { FAIL("rx_gain_db_max"); return; }
    if (agc.target_db != -20.0f)      { FAIL("target_db"); return; }
    if (agc.alpha != 0.1f)            { FAIL("alpha"); return; }
    if (agc.enabled != 1)             { FAIL("enabled"); return; }
    if (agc.phase != AGC_CALIBRATING) { FAIL("phase"); return; }
    PASS();
}

void test_agc_init_binary_search_ranges() {
    TEST("agc_init sets binary search ranges");
    AGCState agc;
    agc_init(&agc);

    float tx_lo = test_db_to_linear(-20.0f);
    float tx_hi = test_db_to_linear(10.0f);
    float rx_lo = test_db_to_linear(0.0f);
    float rx_hi = test_db_to_linear(24.0f);

    if (fabsf(agc.calib_tx_low - tx_lo) > 0.01f) { FAIL("calib_tx_low"); return; }
    if (fabsf(agc.calib_tx_high - tx_hi) > 0.01f) { FAIL("calib_tx_high"); return; }
    if (fabsf(agc.calib_rx_low - rx_lo) > 0.01f) { FAIL("calib_rx_low"); return; }
    if (fabsf(agc.calib_rx_high - rx_hi) > 0.01f) { FAIL("calib_rx_high"); return; }
    PASS();
}

void test_agc_init_multiplicative_params() {
    TEST("agc_init sets multiplicative loop parameters");
    AGCState agc;
    agc_init(&agc);

    if (agc.hyst_margin != 0.20f)  { FAIL("hyst_margin"); return; }
    if (agc.beta_attack != 0.15f)  { FAIL("beta_attack"); return; }
    if (agc.beta_release != 0.03f) { FAIL("beta_release"); return; }
    if (agc.gain_smooth != 1.0f)   { FAIL("gain_smooth"); return; }
    PASS();
}

void test_agc_set_initial_gains() {
    TEST("agc_set_initial_gains sets gains on audio state");
    AudioState audio = {0};
    agc_set_initial_gains(&audio, 2.5f, 0.5f);

    if (audio.tx_gain != 2.5f) { FAIL("tx_gain not set"); return; }
    if (audio.rx_gain != 0.5f) { FAIL("rx_gain not set"); return; }
    PASS();
}

void test_agc_db_conversions() {
    TEST("db_to_linear and linear_to_db are inverses");
    float vals[] = {-20, -10, 0, 10, 20};
    for (int i = 0; i < 5; i++) {
        float db  = vals[i];
        float lin = test_db_to_linear(db);
        float back = test_linear_to_db(lin);
        if (fabsf(db - back) > 0.01f) { FAIL("not inverse for %f", db); return; }
    }
    float neg = test_linear_to_db(0.0f);
    if (neg > -100.0f) { FAIL("linear_to_db(0) should be very negative"); return; }
    PASS();
}

void test_agc_disabled_via_env() {
    TEST("agc_init respects ECHO_AGC=0");
    setenv("ECHO_AGC", "0", 1);
    AGCState agc;
    agc_init(&agc);
    unsetenv("ECHO_AGC");

    if (agc.enabled != 0) { FAIL("not disabled"); return; }
    PASS();
}

/* ── Calibração: busca binária ── */

void test_agc_calibrate_clip_detection() {
    TEST("calibrate detects clip and reduces RX gain");
    AGCState agc;
    agc_init(&agc);
    agc.calib_start_time = time(NULL);
    agc.last_adjust = 0;

    EchoProtocol echo = mock_echo(0, 0, 0);
    AudioState audio = mock_audio(1.0f, 2.0f, 0.9f);

    agc_tune(&agc, &echo, &audio);

    if (audio.rx_gain >= 2.0f) { FAIL("RX gain not reduced"); return; }
    PASS();
}

void test_agc_calibrate_auto_echo() {
    TEST("calibrate detects auto-echo and reduces TX gain");
    AGCState agc;
    agc_init(&agc);
    agc.calib_start_time = time(NULL);
    agc.last_adjust = 0;
    agc.echo_sync_thresh = 2;

    EchoProtocol echo = {0};
    echo.stats.rx_sync_found = 5;
    echo.stats.rx_packets = 0;
    echo.stats.rx_corrupted = 5;
    AudioState audio = mock_audio(1.0f, 1.0f, 0.3f);

    /* First call: streak=1, binary search sets tx_gain to mid (~1.63) */
    agc_tune(&agc, &echo, &audio);
    float tx_after_first = audio.tx_gain;

    /* Manually increment counters to simulate new observations */
    echo.stats.rx_sync_found = 10;
    echo.stats.rx_corrupted = 10;
    agc.last_adjust = time(NULL) - 3;

    /* Second call: streak=2 -> auto-echo fires, reduces TX by 3 dB */
    agc_tune(&agc, &echo, &audio);

    if (audio.tx_gain >= tx_after_first) {
        FAIL("TX gain not reduced (was %.2f, now %.2f)", tx_after_first, audio.tx_gain);
        return;
    }
    PASS();
}

void test_agc_calibrate_convergence() {
    TEST("calibrate marks converged when packets received with good RMS");
    AGCState agc;
    agc_init(&agc);
    agc.calib_start_time = time(NULL);
    agc.last_adjust = 0;

    EchoProtocol echo = mock_echo(5, 5, 0);
    AudioState audio = mock_audio(1.0f, 1.0f, 0.3f);

    agc_tune(&agc, &echo, &audio);

    if (!agc.calib_done)    { FAIL("not converged"); return; }
    if (agc.best_rms != 0.3f) { FAIL("best_rms"); return; }
    PASS();
}

void test_agc_calibrate_binary_search_moves_bounds() {
    TEST("calibrate binary search adjusts bounds correctly");
    AGCState agc;
    agc_init(&agc);
    agc.calib_start_time = time(NULL);
    agc.last_adjust = 0;

    /* Simula iteração sem pacotes → high deve descer */
    EchoProtocol echo = mock_echo(0, 0, 0);
    AudioState audio = mock_audio(1.0f, 1.0f, 0.3f);

    float tx_hi_before = agc.calib_tx_high;

    agc_tune(&agc, &echo, &audio);

    if (agc.calib_tx_iter != 1) { FAIL("tx_iter not incremented"); return; }
    if (agc.calib_tx_high >= tx_hi_before) { FAIL("tx_high should decrease"); return; }
    PASS();
}

void test_agc_calibrate_to_steady_transition() {
    TEST("transitions from CALIBRATING to STEADY after timeout");
    AGCState agc;
    agc_init(&agc);
    agc.calib_start_time = time(NULL) - 25;
    agc.last_adjust = time(NULL) - 25;

    EchoProtocol echo = mock_echo(0, 0, 0);
    AudioState audio = mock_audio(1.0f, 1.0f, 0.3f);

    agc_tune(&agc, &echo, &audio);

    if (agc.phase != AGC_STEADY) { FAIL("not transitioned"); return; }
    PASS();
}

/* ── Steady: laço multiplicativo ── */

void test_agc_steady_clip_reduction() {
    TEST("steady reduces RX gain on clip");
    AGCState agc;
    agc_init(&agc);
    agc.phase = AGC_STEADY;
    agc.last_adjust = time(NULL) - 10;
    agc.settle_secs = 5;

    EchoProtocol echo = mock_echo(0, 0, 0);
    AudioState audio = mock_audio(1.0f, 2.0f, 0.9f);

    agc_tune(&agc, &echo, &audio);

    if (audio.rx_gain >= 2.0f) { FAIL("RX gain not reduced"); return; }
    PASS();
}

void test_agc_steady_multiplicative_low_signal() {
    TEST("steady increases RX gain via multiplicative loop for low power");
    AGCState agc;
    agc_init(&agc);
    agc.phase = AGC_STEADY;
    agc.last_adjust = time(NULL) - 10;
    agc.settle_secs = 5;

    EchoProtocol echo = mock_echo(0, 0, 0);
    float rms = 0.05f;  /* -26 dB — below target */
    AudioState audio = mock_audio(1.0f, 1.0f, rms);

    agc_tune(&agc, &echo, &audio);

    /* power = 0.0025, target = 0.01 → ratio = 4 → gain should increase */
    if (audio.rx_gain <= 1.0f) { FAIL("RX gain not increased for low signal"); return; }
    PASS();
}

void test_agc_steady_hysteresis_no_op() {
    TEST("steady does nothing when power is within hysteresis band");
    AGCState agc;
    agc_init(&agc);
    agc.phase = AGC_STEADY;
    agc.last_adjust = time(NULL) - 10;
    agc.settle_secs = 5;
    agc.gain_smooth = 2.0f;

    /* target_power = 0.01, margin = 0.20 → band [0.00833, 0.012] */
    float rms = sqrtf(0.01f);  /* power = 0.01, exactly on target */
    EchoProtocol echo = mock_echo(0, 0, 0);
    AudioState audio = mock_audio(1.0f, 2.0f, rms);

    float rx_before = audio.rx_gain;
    agc_tune(&agc, &echo, &audio);

    /* gain_smooth should not change (within band) */
    if (fabsf(agc.gain_smooth - 2.0f) > 0.01f) { FAIL("gain_smooth changed inside band"); return; }
    if (fabsf(audio.rx_gain - rx_before) > 0.01f) { FAIL("rx_gain changed inside band"); return; }
    PASS();
}

void test_agc_steady_silence_freezes_gains() {
    TEST("steady freezes gains during silence");
    AGCState agc;
    agc_init(&agc);
    agc.phase = AGC_STEADY;
    agc.last_adjust = time(NULL) - 10;
    agc.settle_secs = 5;

    EchoProtocol echo = mock_echo(0, 0, 0);
    AudioState audio = mock_audio(1.5f, 8.0f, 0.02f);

    float tx_before = audio.tx_gain;
    float rx_before = audio.rx_gain;

    agc_tune(&agc, &echo, &audio);

    if (fabsf(audio.tx_gain - tx_before) > 0.01f) { FAIL("TX gain changed during silence"); return; }
    if (fabsf(audio.rx_gain - rx_before) > 0.01f) { FAIL("RX gain changed during silence"); return; }
    if (!agc.frozen) { FAIL("not frozen"); return; }
    PASS();
}

void test_agc_freeze_during_tx() {
    TEST("agc_tune returns early when agc_freeze is set");
    AGCState agc;
    agc_init(&agc);
    agc.phase = AGC_STEADY;
    agc.last_adjust = time(NULL) - 10;

    EchoProtocol echo = mock_echo(0, 0, 0);
    AudioState audio = mock_audio(1.0f, 1.0f, 0.9f);
    audio.agc_freeze = 1;

    agc_tune(&agc, &echo, &audio);

    if (audio.rx_gain != 1.0f) { FAIL("RX gain changed despite freeze"); return; }
    PASS();
}

int main() {
    printf("=== AGC Tests ===\n\n");

    printf("[init]\n");
    test_agc_init_defaults();
    test_agc_init_binary_search_ranges();
    test_agc_init_multiplicative_params();
    test_agc_set_initial_gains();
    test_agc_db_conversions();
    test_agc_disabled_via_env();

    printf("\n[calibrate]\n");
    test_agc_calibrate_clip_detection();
    test_agc_calibrate_auto_echo();
    test_agc_calibrate_convergence();
    test_agc_calibrate_binary_search_moves_bounds();
    test_agc_calibrate_to_steady_transition();

    printf("\n[steady]\n");
    test_agc_steady_clip_reduction();
    test_agc_steady_multiplicative_low_signal();
    test_agc_steady_hysteresis_no_op();
    test_agc_steady_silence_freezes_gains();

    printf("\n[freeze]\n");
    test_agc_freeze_during_tx();

    printf("\n=== Summary: %d PASS, %d FAIL ===\n", tests_passed, tests_failed);
    return tests_failed > 0 ? 1 : 0;
}
