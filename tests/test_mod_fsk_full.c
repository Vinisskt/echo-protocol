#include <stdio.h>
#include <math.h>
#include <string.h>
#include <stdlib.h>
#include "../include/mod_fsk.h"
#include "../include/rb_bits.h"

static int tests_passed = 0;
static int tests_failed = 0;

#define TEST(name) do { printf("  - %s ... ", name); } while(0)
#define PASS() do { printf("PASS\n"); tests_passed++; } while(0)
#define FAIL(msg) do { printf("FAIL: %s\n", msg); tests_failed++; } while(0)

void test_pre_calc_fsk_initializes_state() {
    TEST("pre_calc_fsk initializes state correctly");
    StateFSK state;
    memset(&state, 0xFF, sizeof(state));
    pre_calc_fsk(&state);
    if (fabsf(state.current_cos - 1.0f) > 0.0001f) { FAIL("current_cos not 1"); return; }
    if (fabsf(state.current_sin) > 0.0001f) { FAIL("current_sin not 0"); return; }
    if (state.sample_count != 0) { FAIL("sample_count not 0"); return; }
    for (int i = 0; i < 4; i++) {
        if (fabsf(state.step_cos[i]) > 1.0f || fabsf(state.step_sin[i]) > 1.0f) {
            FAIL("step out of range"); return;
        }
    }
    PASS();
}

void test_generate_fsk_produces_valid_output() {
    TEST("generate_fsk returns finite output for all symbols");
    StateFSK state;
    pre_calc_fsk(&state);
    uint8_t symbols[4] = {0, 1, 2, 3};
    for (int s = 0; s < 4; s++) {
        for (int i = 0; i < 100; i++) {
            float out = generate_fsk(&state, &symbols[s]);
            if (isnan(out) || isinf(out)) { FAIL("non-finite output"); return; }
            if (fabsf(out) > 1.0f) { FAIL("output exceeds 1.0"); return; }
        }
    }
    PASS();
}

void test_generate_fsk_different_frequencies() {
    TEST("generate_fsk produces different outputs for different symbols");
    StateFSK state;
    pre_calc_fsk(&state);
    uint8_t sym0 = 0, sym1 = 3;
    float out0[48], out1[48];
    for (int i = 0; i < 48; i++) out0[i] = generate_fsk(&state, &sym0);
    pre_calc_fsk(&state);
    for (int i = 0; i < 48; i++) out1[i] = generate_fsk(&state, &sym1);
    int match = 1;
    for (int i = 0; i < 48; i++) {
        if (fabsf(out0[i] - out1[i]) > 0.001f) { match = 0; break; }
    }
    if (match) { FAIL("symbol 0 and 3 produce identical output"); return; }
    PASS();
}

void test_generate_fsk_maintains_phase_continuity() {
    TEST("generate_fsk maintains continuous phase between symbols");
    StateFSK state;
    pre_calc_fsk(&state);
    uint8_t sym = 1;
    float prev = generate_fsk(&state, &sym);
    for (int i = 1; i < 48; i++) {
        float cur = generate_fsk(&state, &sym);
        if (fabsf(cur - prev) > 1.5f) { FAIL("phase jump detected"); return; }
        prev = cur;
    }
    sym = 2;
    float after = generate_fsk(&state, &sym);
    if (fabsf(after - prev) > 1.5f) { FAIL("phase jump at symbol boundary"); return; }
    PASS();
}

void test_generate_fsk_magnitude_stable() {
    TEST("generate_fsk maintains magnitude near 0.5");
    StateFSK state;
    pre_calc_fsk(&state);
    uint8_t sym = 2;
    for (int i = 0; i < 200; i++) {
        float out = generate_fsk(&state, &sym);
        float mag = state.current_cos * state.current_cos + state.current_sin * state.current_sin;
        if (fabsf(mag - 1.0f) > 0.01f) { FAIL("magnitude deviates from 1"); return; }
        if (fabsf(out) > 0.6f) { FAIL("output amplitude exceeds 0.6"); return; }
    }
    PASS();
}

void test_push_preamble_inserts_24_bits() {
    TEST("push_preamble inserts 24 bits (8 zeros + 16 alternating 1010...)");
    Buffer *buf = rb_init();
    push_preamble(buf);
    uint8_t bit;
    for (int i = 0; i < 24; i++) {
        if (!get_bits(buf, &bit)) { FAIL("preamble has fewer than 24 bits"); free(buf); return; }
        uint8_t expected;
        if (i < 8) {
            expected = 0;
        } else {
            expected = ((i - 8) % 2 == 0) ? 1 : 0;
        }
        if (bit != expected) { FAIL("preamble bit incorrect"); free(buf); return; }
    }
    PASS();
    free(buf);
}

void test_push_sync_word_inserts_32_bits() {
    TEST("push_sync_word inserts SYNC_WORD (0x930B51DE) 32 bits");
    Buffer *buf = rb_init();
    push_sync_word(buf);
    uint8_t bit;
    for (int i = 31; i >= 0; i--) {
        if (!get_bits(buf, &bit)) { FAIL("sync word has fewer than 32 bits"); free(buf); return; }
        uint8_t expected = (SYNC_WORD >> i) & 1;
        if (bit != expected) { FAIL("sync word bit incorrect"); free(buf); return; }
    }
    PASS();
    free(buf);
}

void test_push_preamble_then_sync_word() {
    TEST("push_preamble + push_sync_word in sequence");
    Buffer *buf = rb_init();
    push_preamble(buf);
    push_sync_word(buf);
    uint8_t bit;
    for (int i = 0; i < 24; i++) {
        if (!get_bits(buf, &bit)) { FAIL("preamble incomplete"); free(buf); return; }
    }
    for (int i = 31; i >= 0; i--) {
        if (!get_bits(buf, &bit)) { FAIL("sync word incomplete after preamble"); free(buf); return; }
        uint8_t expected = (SYNC_WORD >> i) & 1;
        if (bit != expected) { FAIL("sync word wrong after preamble"); free(buf); return; }
    }
    PASS();
    free(buf);
}

void test_generate_fsk_each_symbol_unique() {
    TEST("generate_fsk produces distinct output for each of the 4 symbols");
    float averages[4] = {0};
    for (int s = 0; s < 4; s++) {
        StateFSK state;
        pre_calc_fsk(&state);
        uint8_t sym = (uint8_t)s;
        float sum = 0;
        for (int i = 0; i < SAMPLES_PER_SYMBOL; i++) {
            sum += fabsf(generate_fsk(&state, &sym));
        }
        averages[s] = sum / SAMPLES_PER_SYMBOL;
    }
    for (int i = 0; i < 4; i++) {
        for (int j = i + 1; j < 4; j++) {
            if (fabsf(averages[i] - averages[j]) < 0.001f) {
                FAIL("two symbols have nearly identical average output");
                return;
            }
        }
    }
    PASS();
}

int main() {
    printf("=== Complete Tests: FSK Modulator (mod_fsk) ===\n\n");

    printf("[pre_calc_fsk]\n");
    test_pre_calc_fsk_initializes_state();

    printf("\n[generate_fsk]\n");
    test_generate_fsk_produces_valid_output();
    test_generate_fsk_different_frequencies();
    test_generate_fsk_maintains_phase_continuity();
    test_generate_fsk_magnitude_stable();
    test_generate_fsk_each_symbol_unique();

    printf("\n[push_preamble / push_sync_word]\n");
    test_push_preamble_inserts_24_bits();
    test_push_sync_word_inserts_32_bits();
    test_push_preamble_then_sync_word();

    printf("\n=== Summary: %d PASS, %d FAIL ===\n", tests_passed, tests_failed);
    return tests_failed > 0 ? 1 : 0;
}
