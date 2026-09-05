#include <stdio.h>
#include <math.h>
#include "../include/level_control.h"

static int tests_passed = 0;
static int tests_failed = 0;

#define TEST(name) do { printf("  - %s ... ", name); } while(0)
#define PASS() do { printf("PASS\n"); tests_passed++; } while(0)
#define FAIL(fmt, ...) do { printf("FAIL: " fmt "\n", ##__VA_ARGS__); tests_failed++; } while(0)
#define FLT_EQ(a,b) (fabsf((a)-(b)) < 1e-4f)

/* ── Bons caminhos ── */

void test_tx_passthrough_gain1() {
    TEST("TX gain=1.0: amostra passa intacta (0.5 -> 0.5)");
    if (!FLT_EQ(level_scale_tx(0.5f, 1.0f), 0.5f)) { FAIL("expected 0.5"); return; }
    if (!FLT_EQ(level_scale_tx(-0.3f, 1.0f), -0.3f)) { FAIL("expected -0.3"); return; }
    PASS();
}

void test_tx_scales_linear() {
    TEST("TX gain=0.3: 0.5 -> 0.15 (calibração conservadora)");
    if (!FLT_EQ(level_scale_tx(0.5f, 0.3f), 0.15f)) { FAIL("expected 0.15"); return; }
    PASS();
}

void test_tx_warmup_gain_2_5_clamps() {
    TEST("TX gain=2.5 (warm-up IR): 0.5 -> clamp em 0.99 (0.5*2.5=1.25 cortaria)");
    if (!FLT_EQ(level_scale_tx(0.5f, 2.5f), TX_GAIN_LIMIT)) { FAIL("expected 0.99"); return; }
    PASS();
}

void test_tx_gain_zero_silence() {
    TEST("TX gain=0: silêncio (0.5 -> 0)");
    if (!FLT_EQ(level_scale_tx(0.5f, 0.0f), 0.0f)) { FAIL("expected 0.0"); return; }
    PASS();
}

void test_tx_symmetric_negative() {
    TEST("TX negativa: simetria (gain 0.3, -0.5 -> -0.15)");
    if (!FLT_EQ(level_scale_tx(-0.5f, 0.3f), -0.15f)) { FAIL("expected -0.15"); return; }
    PASS();
}

/* ── Médios (bordas) ── */

void test_tx_boundary_just_below_clamp() {
    TEST("TX nowhere near clamp: 0.5 * 1.9 = 0.95 (dentro do limite)");
    if (!FLT_EQ(level_scale_tx(0.5f, 1.9f), 0.95f)) { FAIL("expected 0.95"); return; }
    PASS();
}

void test_tx_amplitude_zero_input() {
    TEST("TX amostra 0 com ganho alto: 0 -> 0");
    if (!FLT_EQ(level_scale_tx(0.0f, 3.0f), 0.0f)) { FAIL("expected 0.0"); return; }
    PASS();
}

void test_rx_passthrough_gain1() {
    TEST("RX gain=1.0: amostra passa intacta");
    if (!FLT_EQ(level_scale_rx(0.2f, 1.0f), 0.2f)) { FAIL("expected 0.2"); return; }
    PASS();
}

void test_rx_max_gain_24db() {
    TEST("RX gain 15.8x (+24 dB máx. AGC): 0.1 -> 1.58");
    if (!FLT_EQ(level_scale_rx(0.1f, 15.8f), 1.58f)) { FAIL("expected 1.58"); return; }
    PASS();
}

void test_rx_gain_zero() {
    TEST("RX gain=0: 0.3 -> 0");
    if (!FLT_EQ(level_scale_rx(0.3f, 0.0f), 0.0f)) { FAIL("expected 0.0"); return; }
    PASS();
}

void test_rx_negative_symmetric() {
    TEST("RX negativa: simetria (gain 4, -0.1 -> -0.4)");
    if (!FLT_EQ(level_scale_rx(-0.1f, 4.0f), -0.4f)) { FAIL("expected -0.4"); return; }
    PASS();
}

/* ── Horríveis (hostis) ── */

void test_tx_nan_gain() {
    TEST("TX ganho NaN: 0 (não propaga NaN)");
    if (!(level_scale_tx(0.5f, NAN) == 0.0f)) { FAIL("expected 0.0"); return; }
    PASS();
}

void test_tx_nan_input() {
    TEST("TX amostra NaN: 0");
    if (!(level_scale_tx(NAN, 1.0f) == 0.0f)) { FAIL("expected 0.0"); return; }
    PASS();
}

void test_tx_huge_gain() {
    TEST("TX ganho 1e9: clamp em 0.99");
    if (!FLT_EQ(level_scale_tx(0.5f, 1e9f), TX_GAIN_LIMIT)) { FAIL("expected 0.99"); return; }
    PASS();
}

void test_tx_infinity_input() {
    TEST("TX amostra +inf: 0");
    if (!(level_scale_tx(INFINITY, 1.0f) == 0.0f)) { FAIL("expected 0.0"); return; }
    PASS();
}

void test_rx_nan_gain() {
    TEST("RX ganho NaN: 0");
    if (!(level_scale_rx(0.2f, NAN) == 0.0f)) { FAIL("expected 0.0"); return; }
    PASS();
}

void test_rx_huge_gain() {
    TEST("RX ganho 1e20: clamp em 8.0 (guarda de sanidade)");
    if (!FLT_EQ(level_scale_rx(0.5f, 1e20f), RX_GAIN_LIMIT)) { FAIL("expected 8.0"); return; }
    PASS();
}

void test_rx_huge_sample() {
    TEST("RX amostra enorme com gain 1: clamp em 8.0");
    if (!FLT_EQ(level_scale_rx(1e10f, 1.0f), RX_GAIN_LIMIT)) { FAIL("expected 8.0"); return; }
    PASS();
}

void test_negative_gain_symmetric_clamp() {
    TEST("ganho TX negativo: clamp simétrico (0.5 * -3 -> -0.99)");
    if (!FLT_EQ(level_scale_tx(0.5f, -3.0f), -TX_GAIN_LIMIT)) { FAIL("expected -0.99"); return; }
    PASS();
}

int main() {
    printf("=== Level Control (AGC integration) ===\n\n");

    printf("[bons]\n");
    test_tx_passthrough_gain1();
    test_tx_scales_linear();
    test_tx_warmup_gain_2_5_clamps();
    test_tx_gain_zero_silence();
    test_tx_symmetric_negative();

    printf("\n[médios]\n");
    test_tx_boundary_just_below_clamp();
    test_tx_amplitude_zero_input();
    test_rx_passthrough_gain1();
    test_rx_max_gain_24db();
    test_rx_gain_zero();
    test_rx_negative_symmetric();

    printf("\n[horríveis]\n");
    test_tx_nan_gain();
    test_tx_nan_input();
    test_tx_huge_gain();
    test_tx_infinity_input();
    test_rx_nan_gain();
    test_rx_huge_gain();
    test_rx_huge_sample();
    test_negative_gain_symmetric_clamp();

    printf("\n=== Summary: %d PASS, %d FAIL ===\n", tests_passed, tests_failed);
    return tests_failed > 0 ? 1 : 0;
}