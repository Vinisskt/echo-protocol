#include <stdio.h>
#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "../include/demod_afsk.h"
#include "../include/mod_afsk.h"
#include "../include/mod_fsk.h"

static int tests_passed = 0;
static int tests_failed = 0;

#define TEST(name) do { printf("  - %s ... ", name); } while(0)
#define PASS() do { printf("PASS\n"); tests_passed++; } while(0)
#define FAIL(msg) do { printf("FAIL: %s\n", msg); tests_failed++; } while(0)
#define EPSILON 0.0001f

void test_pre_calc_goertzel_coefficient() {
    TEST("pre_calc_goertzel calculates coeff and omega correctly");
    StateGoertzel state;
    uint16_t freq = FREQ_MARK;
    pre_calc_goertzel(&state, &freq);
    float expected_k = ((float)SAMPLES_PER_SYMBOL * FREQ_MARK) / SAMPLE_RATE;
    float expected_omega = (2.0f * (float)M_PI * expected_k) / SAMPLES_PER_SYMBOL;
    float expected_coeff = 2.0f * cosf(expected_omega);
    if (fabsf(state.k - expected_k) > 0.01f) { FAIL("k incorrect"); return; }
    if (fabsf(state.coeff - expected_coeff) > 0.01f) { FAIL("coeff incorrect"); return; }
    if (state.n != SAMPLES_PER_SYMBOL) { FAIL("n != SAMPLES_PER_SYMBOL"); return; }
    PASS();
}

void test_pre_calc_goertzel_different_frequencies() {
    TEST("pre_calc_goertzel produces different coeff for FREQ_SPACE");
    StateGoertzel state_mark, state_space;
    uint16_t f_m = FREQ_MARK, f_s = FREQ_SPACE;
    pre_calc_goertzel(&state_mark, &f_m);
    pre_calc_goertzel(&state_space, &f_s);
    if (fabsf(state_mark.coeff - state_space.coeff) < 0.001f) {
        FAIL("coeff should differ between MARK and SPACE");
        return;
    }
    PASS();
}

void test_pre_calc_goertzel_initial_zero_state() {
    TEST("pre_calc_goertzel initializes q1=0, q2=0");
    StateGoertzel state;
    uint16_t freq = FREQ_MARK;
    pre_calc_goertzel(&state, &freq);
    if (fabsf(state.q1) > EPSILON) { FAIL("q1 != 0"); return; }
    if (fabsf(state.q2) > EPSILON) { FAIL("q2 != 0"); return; }
    PASS();
}

void test_process_goertzel_positive_magnitude() {
    TEST("process_goertzel returns magnitude >= 0");
    StateGoertzel state;
    uint16_t freq = FREQ_MARK;
    pre_calc_goertzel(&state, &freq);
    for (int i = 0; i < SAMPLES_PER_BIT; i++) {
        float sample = sinf(2.0f * (float)M_PI * FREQ_MARK * i / SAMPLE_RATE);
        float mag = process_goertzel(&state, &sample);
        if (mag < 0 && fabsf(mag) > EPSILON) {
            FAIL("negative magnitude");
            return;
        }
    }
    PASS();
}

void test_process_goertzel_mark_greater_than_space_for_mark_signal() {
    TEST("process_goertzel: magnitude MARK > SPACE for MARK signal");
    StateGoertzel space, mark;
    uint16_t f_s = FREQ_SPACE, f_m = FREQ_MARK;
    pre_calc_goertzel(&space, &f_s);
    pre_calc_goertzel(&mark, &f_m);
    for (int i = 0; i < SAMPLES_PER_BIT; i++) {
        float sample = sinf(2.0f * (float)M_PI * FREQ_MARK * i / SAMPLE_RATE);
        process_goertzel(&space, &sample);
        process_goertzel(&mark, &sample);
    }
    float mag_s = (space.q1 * space.q1) + (space.q2 * space.q2) - (space.q1 * space.q2 * space.coeff);
    float mag_m = (mark.q1 * mark.q1) + (mark.q2 * mark.q2) - (mark.q1 * mark.q2 * mark.coeff);
    if (mag_m <= mag_s) {
        FAIL("MARK not greater than SPACE for MARK signal");
        return;
    }
    PASS();
}

void test_process_goertzel_space_greater_than_mark_for_space_signal() {
    TEST("process_goertzel: magnitude SPACE > MARK for SPACE signal");
    StateGoertzel space, mark;
    uint16_t f_s = FREQ_SPACE, f_m = FREQ_MARK;
    pre_calc_goertzel(&space, &f_s);
    pre_calc_goertzel(&mark, &f_m);
    for (int i = 0; i < SAMPLES_PER_BIT; i++) {
        float sample = sinf(2.0f * (float)M_PI * FREQ_SPACE * i / SAMPLE_RATE);
        process_goertzel(&space, &sample);
        process_goertzel(&mark, &sample);
    }
    float mag_s = (space.q1 * space.q1) + (space.q2 * space.q2) - (space.q1 * space.q2 * space.coeff);
    float mag_m = (mark.q1 * mark.q1) + (mark.q2 * mark.q2) - (mark.q1 * mark.q2 * mark.coeff);
    if (mag_s <= mag_m) {
        FAIL("SPACE not greater than MARK for SPACE signal");
        return;
    }
    PASS();
}

void test_reset_state_clears_values() {
    TEST("reset_state clears q1 and q2");
    StateGoertzel state;
    uint16_t freq = FREQ_MARK;
    pre_calc_goertzel(&state, &freq);
    float sample = 1.0f;
    process_goertzel(&state, &sample);
    reset_state(&state);
    if (fabsf(state.q1) > EPSILON) { FAIL("q1 != 0 after reset"); return; }
    if (fabsf(state.q2) > EPSILON) { FAIL("q2 != 0 after reset"); return; }
    PASS();
}

void test_reset_state_multiple_calls() {
    TEST("reset_state can be called multiple times with no side effects");
    StateGoertzel state;
    uint16_t freq = FREQ_MARK;
    pre_calc_goertzel(&state, &freq);
    reset_state(&state);
    reset_state(&state);
    reset_state(&state);
    if (fabsf(state.q1) > EPSILON || fabsf(state.q2) > EPSILON) {
        FAIL("multiple reset altered q1/q2");
        return;
    }
    PASS();
}

void test_check_sync_word_detects_correctly() {
    TEST("check_sync_word returns 0 on detecting exact SYNC_WORD");
    uint32_t shift_reg = 0;
    uint8_t bit;
    for (int i = 0; i < 32; i++) {
        bit = (uint8_t)((i % 2));
        check_sync_word(&shift_reg, &bit);
    }
    shift_reg = 0;
    for (int i = 31; i >= 0; i--) {
        bit = (SYNC_WORD >> i) & 1;
        uint8_t result = check_sync_word(&shift_reg, &bit);
        if (i > 0 && result == 0) { FAIL("premature sync detected"); return; }
        if (i == 0 && result != 0) { FAIL("sync not detected at last bit"); return; }
    }
    PASS();
}

void test_check_sync_word_no_false_positive() {
    TEST("check_sync_word does not false-positive on random bits");
    uint32_t shift_reg = 0;
    uint8_t bit;
    for (int i = 0; i < 1000; i++) {
        bit = rand() % 2;
        if (check_sync_word(&shift_reg, &bit) == 0) {
            FAIL("false positive detected");
            return;
        }
    }
    PASS();
}

void test_check_sync_word_persistence() {
    TEST("check_sync_word returns 1 after sync + 1 extra bit");
    uint32_t shift_reg = 0;
    uint8_t bit;
    for (int i = 31; i >= 0; i--) {
        bit = (SYNC_WORD >> i) & 1;
        check_sync_word(&shift_reg, &bit);
    }
    bit = 0;
    uint8_t result = check_sync_word(&shift_reg, &bit);
    if (result == 0) { FAIL("sync persisted after extra bit"); return; }
    PASS();
}

void test_check_sync_word_detects_second_sync() {
    TEST("check_sync_word detects a new SYNC_WORD after shifting out the previous one");
    uint32_t shift_reg = 0;
    uint8_t bit;

    for (int i = 31; i >= 0; i--) {
        bit = (SYNC_WORD >> i) & 1;
        check_sync_word(&shift_reg, &bit);
    }

    bit = 0; check_sync_word(&shift_reg, &bit);

    for (int i = 31; i >= 0; i--) {
        bit = (SYNC_WORD >> i) & 1;
        uint8_t result = check_sync_word(&shift_reg, &bit);
        if (i == 0 && result != 0) { FAIL("second sync not detected"); return; }
    }
    PASS();
}

int main() {
    printf("=== Complete Tests: AFSK Demodulator (demod_afsk) ===\n\n");

    printf("[pre_calc_goertzel]\n");
    test_pre_calc_goertzel_coefficient();
    test_pre_calc_goertzel_different_frequencies();
    test_pre_calc_goertzel_initial_zero_state();

    printf("\n[process_goertzel]\n");
    test_process_goertzel_positive_magnitude();
    test_process_goertzel_mark_greater_than_space_for_mark_signal();
    test_process_goertzel_space_greater_than_mark_for_space_signal();

    printf("\n[reset_state]\n");
    test_reset_state_clears_values();
    test_reset_state_multiple_calls();

    printf("\n[check_sync_word]\n");
    test_check_sync_word_detects_correctly();
    test_check_sync_word_no_false_positive();
    test_check_sync_word_persistence();
    test_check_sync_word_detects_second_sync();

    printf("\n=== Summary: %d PASS, %d FAIL ===\n", tests_passed, tests_failed);
    return tests_failed > 0 ? 1 : 0;
}
