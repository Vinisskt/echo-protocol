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

/* Helper: create mock audio state */
static AudioState mock_audio(float tx_gain, float rx_gain, float rms) {
    AudioState a = {0};
    a.tx_gain = tx_gain;
    a.rx_gain = rx_gain;
    atomic_init(&a.in_rms, rms);
    a.agc_freeze = 0;
    return a;
}

/* Helper: create mock echo protocol with stats */
static EchoProtocol mock_echo(uint64_t sync, uint64_t pkts, uint64_t corrupt) {
    EchoProtocol e = {0};
    e.stats.rx_sync_found = sync;
    e.stats.rx_packets = pkts;
    e.stats.rx_corrupted = corrupt;
    return e;
}

/* Helper: test linear_to_db using public API equivalent */
static float test_linear_to_db(float linear) {
    if (linear <= 0.0f) return -120.0f;
    return 20.0f * log10f(linear);
}

static float test_db_to_linear(float db) {
    return powf(10.0f, db / 20.0f);
}

void test_agc_init_defaults() {
    TEST("agc_init sets default configuration");
    AGCState agc;
    agc_init(&agc);
    
    if (agc.tx_gain_db_min != -20.0f) { FAIL("tx_gain_db_min"); return; }
    if (agc.tx_gain_db_max != 10.0f) { FAIL("tx_gain_db_max"); return; }
    if (agc.rx_gain_db_min != 0.0f) { FAIL("rx_gain_db_min"); return; }
    if (agc.rx_gain_db_max != 24.0f) { FAIL("rx_gain_db_max"); return; }
    if (agc.target_db != -20.0f) { FAIL("target_db"); return; }
    if (agc.loop_bw != 0.015f) { FAIL("loop_bw"); return; }
    if (agc.alpha != 0.1f) { FAIL("alpha"); return; }
    if (agc.enabled != 1) { FAIL("enabled"); return; }
    if (agc.phase != AGC_CALIBRATING) { FAIL("phase not CALIBRATING"); return; }
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
    float test_vals[] = {-20, -10, 0, 10, 20};
    for (int i = 0; i < 5; i++) {
        float db = test_vals[i];
        float lin = test_db_to_linear(db);
        float back = test_linear_to_db(lin);
        if (fabsf(db - back) > 0.01f) { FAIL("conversion not inverse for %f", db); return; }
    }
    /* Test edge case: linear <= 0 */
    float neg = test_linear_to_db(0.0f);
    if (neg > -100.0f) { FAIL("linear_to_db(0) should be very negative"); return; }
    PASS();
}

void test_agc_calibrate_clip_detection() {
    TEST("agc_calibrate detects clip and reduces RX gain");
    AGCState agc;
    agc_init(&agc);
    agc.calib_start_time = time(NULL);
    agc.last_adjust = 0;
    
    EchoProtocol echo = mock_echo(0, 0, 0);
    AudioState audio = mock_audio(1.0f, 2.0f, 0.9f);  /* rms > rms_max (0.7) */
    
    agc_tune(&agc, &echo, &audio);
    
    /* Should have reduced RX gain due to clip */
    if (audio.rx_gain >= 2.0f) { FAIL("RX gain not reduced after clip"); return; }
    PASS();
}

void test_agc_calibrate_auto_echo() {
    TEST("agc_calibrate detects auto-echo (syncs without packets) and reduces TX gain");
    AGCState agc;
    agc_init(&agc);
    agc.calib_start_time = time(NULL);
    agc.last_adjust = 0;
    agc.echo_sync_thresh = 2;
    
    /* Use mutable echo to simulate incrementing counters */
    EchoProtocol echo = {0};
    echo.stats.rx_sync_found = 5;
    echo.stats.rx_packets = 0;
    echo.stats.rx_corrupted = 5;
    AudioState audio = mock_audio(1.0f, 1.0f, 0.3f);
    
    /* First call: streak=1 */
    agc_tune(&agc, &echo, &audio);
    /* Manually increment counters as if time passed */
    echo.stats.rx_sync_found = 10;
    echo.stats.rx_corrupted = 10;
    agc.last_adjust = time(NULL) - 3;
    
    /* Second call: streak=2 -> should trigger */
    agc_tune(&agc, &echo, &audio);
    
    if (audio.tx_gain >= 1.0f) { FAIL("TX gain not reduced for auto-echo"); return; }
    PASS();
}

void test_agc_calibrate_convergence() {
    TEST("agc_calibrate marks converged when packets received with good RMS");
    AGCState agc;
    agc_init(&agc);
    agc.calib_start_time = time(NULL);
    agc.last_adjust = 0;
    
    EchoProtocol echo = mock_echo(5, 5, 0);  /* good packets */
    AudioState audio = mock_audio(1.0f, 1.0f, 0.3f);  /* good RMS */
    
    agc_tune(&agc, &echo, &audio);
    
    if (!agc.calib_done) { FAIL("not marked converged"); return; }
    if (agc.best_rms != 0.3f) { FAIL("best_rms not recorded"); return; }
    PASS();
}

void test_agc_calibrate_to_steady_transition() {
    TEST("agc transitions from CALIBRATING to STEADY after 20s or convergence");
    AGCState agc;
    agc_init(&agc);
    agc.calib_start_time = time(NULL) - 25;  /* 25 seconds ago */
    agc.last_adjust = time(NULL) - 25;
    
    EchoProtocol echo = mock_echo(0, 0, 0);
    AudioState audio = mock_audio(1.0f, 1.0f, 0.3f);
    
    agc_tune(&agc, &echo, &audio);
    
    if (agc.phase != AGC_STEADY) { FAIL("not transitioned to STEADY"); return; }
    PASS();
}

void test_agc_steady_clip_reduction() {
    TEST("agc_steady reduces RX gain on clip");
    AGCState agc;
    agc_init(&agc);
    agc.phase = AGC_STEADY;
    agc.last_adjust = time(NULL) - 10;
    agc.settle_secs = 5;
    
    EchoProtocol echo = mock_echo(0, 0, 0);
    AudioState audio = mock_audio(1.0f, 2.0f, 0.9f);  /* clip */
    
    agc_tune(&agc, &echo, &audio);
    
    if (audio.rx_gain >= 2.0f) { FAIL("RX gain not reduced on clip in steady"); return; }
    PASS();
}

void test_agc_steady_loop_control() {
    TEST("agc_steady adjusts RX gain via proportional loop to hit target dB");
    AGCState agc;
    agc_init(&agc);
    agc.phase = AGC_STEADY;
    agc.last_adjust = time(NULL) - 10;
    agc.settle_secs = 5;
    agc.target_db = -20.0f;
    agc.loop_bw = 0.015f;
    agc.alpha = 0.1f;
    agc.power_avg = 0.0f;  /* will be initialized */
    
    EchoProtocol echo = mock_echo(0, 0, 0);
    AudioState audio = mock_audio(1.0f, 1.0f, 0.05f);  /* RMS too low (-26 dB) */
    
    agc_tune(&agc, &echo, &audio);
    
    /* Should increase RX gain to bring RMS up toward target */
    if (audio.rx_gain <= 1.0f) { FAIL("RX gain not increased for low RMS"); return; }
    PASS();
}

void test_agc_steady_silence_rx_gain_up() {
    TEST("agc_steady increases RX gain on silence (no sync, no packets, low RMS)");
    AGCState agc;
    agc_init(&agc);
    agc.phase = AGC_STEADY;
    agc.last_adjust = time(NULL) - 10;
    agc.settle_secs = 5;
    
    EchoProtocol echo = mock_echo(0, 0, 0);  /* silent link */
    AudioState audio = mock_audio(1.0f, 1.0f, 0.02f);  /* very low RMS */
    
    agc_tune(&agc, &echo, &audio);
    
    if (audio.rx_gain <= 1.0f) { FAIL("RX gain not increased on silence"); return; }
    PASS();
}

void test_agc_steady_silence_tx_gain_up_when_rx_maxed() {
    TEST("agc_steady increases TX gain on silence when RX gain at max");
    AGCState agc;
    agc_init(&agc);
    agc.phase = AGC_STEADY;
    agc.last_adjust = time(NULL) - 10;
    agc.settle_secs = 5;
    /* Set power_avg so proportional loop error_db < 1 dB (won't trigger) */
    agc.power_avg = 0.01f;  /* ~ -20 dB, close to target */
    
    EchoProtocol echo = mock_echo(0, 0, 0);
    /* RX gain at max (16 = +24 dB linear = 15.85) */
    AudioState audio = mock_audio(0.5f, 16.0f, 0.02f);
    
    agc_tune(&agc, &echo, &audio);
    
    if (audio.tx_gain <= 0.5f) { FAIL("TX gain not increased when RX at max"); return; }
    PASS();
}

void test_agc_freeze_during_tx() {
    TEST("agc_tune returns early when audio->agc_freeze is set");
    AGCState agc;
    agc_init(&agc);
    agc.phase = AGC_STEADY;
    agc.last_adjust = time(NULL) - 10;
    
    EchoProtocol echo = mock_echo(0, 0, 0);
    AudioState audio = mock_audio(1.0f, 1.0f, 0.9f);  /* would trigger clip */
    audio.agc_freeze = 1;  /* freeze AGC */
    
    agc_tune(&agc, &echo, &audio);
    
    /* Gain should not change because AGC is frozen */
    if (audio.rx_gain != 1.0f) { FAIL("RX gain changed despite freeze"); return; }
    PASS();
}

void test_agc_disabled_via_env( void ) {
    TEST("agc_init respects ECHO_AGC=0 environment variable");
    setenv("ECHO_AGC", "0", 1);
    AGCState agc;
    agc_init(&agc);
    unsetenv("ECHO_AGC");
    
    if (agc.enabled != 0) { FAIL("AGC not disabled by env"); return; }
    PASS();
}

int main() {
    printf("=== AGC Tests ===\n\n");
    
    printf("[init]\n");
    test_agc_init_defaults();
    test_agc_set_initial_gains();
    test_agc_db_conversions();
    test_agc_disabled_via_env();
    
    printf("\n[calibrate]\n");
    test_agc_calibrate_clip_detection();
    test_agc_calibrate_auto_echo();
    test_agc_calibrate_convergence();
    test_agc_calibrate_to_steady_transition();
    
    printf("\n[steady]\n");
    test_agc_steady_clip_reduction();
    test_agc_steady_loop_control();
    test_agc_steady_silence_rx_gain_up();
    test_agc_steady_silence_tx_gain_up_when_rx_maxed();
    
    printf("\n[freeze]\n");
    test_agc_freeze_during_tx();
    
    printf("\n=== Summary: %d PASS, %d FAIL ===\n", tests_passed, tests_failed);
    return tests_failed > 0 ? 1 : 0;
}