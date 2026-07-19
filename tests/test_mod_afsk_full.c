#include <stdio.h>
#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "../include/mod_afsk.h"
#include "../include/rb_bits.h"

static int tests_passed = 0;
static int tests_failed = 0;

#define TEST(name) do { printf("  - %s ... ", name); } while(0)
#define PASS() do { printf("PASS\n"); tests_passed++; } while(0)
#define FAIL(msg) do { printf("FAIL: %s\n", msg); tests_failed++; } while(0)
#define EPSILON 0.0001f

void test_pre_calc_afsk_space_coefficient() {
    TEST("pre_calc_afsk calculates step_cos_space correctly");
    StateAFSK state;
    pre_calc_afsk(&state);
    float expected = cosf(2.0f * M_PI * FREQ_SPACE / SAMPLE_RATE);
    if (fabsf(state.step_cos_space - expected) > EPSILON) {
        FAIL("step_cos_space incorrect");
        return;
    }
    PASS();
}

void test_pre_calc_afsk_mark_coefficient() {
    TEST("pre_calc_afsk calculates step_cos_mark correctly");
    StateAFSK state;
    pre_calc_afsk(&state);
    float expected = cosf(2.0f * M_PI * FREQ_MARK / SAMPLE_RATE);
    if (fabsf(state.step_cos_mark - expected) > EPSILON) {
        FAIL("step_cos_mark incorrect");
        return;
    }
    PASS();
}

void test_pre_calc_afsk_sin_coefficients() {
    TEST("pre_calc_afsk calculates step_sin for both frequencies");
    StateAFSK state;
    pre_calc_afsk(&state);
    float exp_s = sinf(2.0f * M_PI * FREQ_SPACE / SAMPLE_RATE);
    float exp_m = sinf(2.0f * M_PI * FREQ_MARK / SAMPLE_RATE);
    if (fabsf(state.step_sin_space - exp_s) > EPSILON) { FAIL("step_sin_space"); return; }
    if (fabsf(state.step_sin_mark - exp_m) > EPSILON) { FAIL("step_sin_mark"); return; }
    PASS();
}

void test_pre_calc_afsk_initial_state() {
    TEST("pre_calc_afsk initializes current_cos=1, current_sin=0, sample_count=0");
    StateAFSK state;
    pre_calc_afsk(&state);
    if (fabsf(state.current_cos - 1.0f) > EPSILON) { FAIL("current_cos != 1"); return; }
    if (fabsf(state.current_sin - 0.0f) > EPSILON) { FAIL("current_sin != 0"); return; }
    if (state.sample_count != 0) { FAIL("sample_count != 0"); return; }
    PASS();
}

void test_generate_afsk_mark_phase_shift() {
    TEST("generate_afsk with bit 1 produces MARK phase shift");
    StateAFSK state;
    pre_calc_afsk(&state);
    uint8_t bit = 1;
    float last_phase = atan2f(state.current_sin, state.current_cos);
    for (int i = 0; i < SAMPLES_PER_BIT; i++) {
        generate_afsk(&state, &bit);
    }
    float current_phase = atan2f(state.current_sin, state.current_cos);
    float total_delta = current_phase - last_phase;
    if (total_delta < 0) total_delta += 2.0f * (float)M_PI;
    float expected_delta = (2.0f * (float)M_PI * FREQ_MARK * SAMPLES_PER_BIT) / SAMPLE_RATE;
    expected_delta = fmodf(expected_delta, 2.0f * (float)M_PI);
    float diff = fabsf(total_delta - expected_delta);
    if (diff > 0.01f && fabsf(diff - 2.0f * (float)M_PI) > 0.01f) {
        FAIL("MARK phase delta incorrect");
        return;
    }
    PASS();
}

void test_generate_afsk_space_phase_shift() {
    TEST("generate_afsk with bit 0 produces SPACE phase shift");
    StateAFSK state;
    pre_calc_afsk(&state);
    uint8_t bit = 0;
    float last_phase = atan2f(state.current_sin, state.current_cos);
    for (int i = 0; i < SAMPLES_PER_BIT; i++) {
        generate_afsk(&state, &bit);
    }
    float current_phase = atan2f(state.current_sin, state.current_cos);
    float total_delta = current_phase - last_phase;
    if (total_delta < 0) total_delta += 2.0f * (float)M_PI;
    float expected_delta = (2.0f * (float)M_PI * FREQ_SPACE * SAMPLES_PER_BIT) / SAMPLE_RATE;
    expected_delta = fmodf(expected_delta, 2.0f * (float)M_PI);
    float diff = fabsf(total_delta - expected_delta);
    if (diff > 0.01f && fabsf(diff - 2.0f * (float)M_PI) > 0.01f) {
        FAIL("SPACE phase delta incorrect");
        return;
    }
    PASS();
}

void test_generate_afsk_frequency_alternation() {
    TEST("generate_afsk alternates between MARK and SPACE correctly");
    StateAFSK state;
    pre_calc_afsk(&state);
    uint8_t bits[] = {1,0,1,0};
    for (int b = 0; b < 4; b++) {
        float last_phase = atan2f(state.current_sin, state.current_cos);
        for (int i = 0; i < SAMPLES_PER_BIT; i++) {
            generate_afsk(&state, &bits[b]);
        }
        float current_phase = atan2f(state.current_sin, state.current_cos);
        float delta = current_phase - last_phase;
        if (delta < 0) delta += 2.0f * (float)M_PI;
        float freq = (bits[b] == 1) ? FREQ_MARK : FREQ_SPACE;
        float expected = (2.0f * (float)M_PI * freq * SAMPLES_PER_BIT) / SAMPLE_RATE;
        expected = fmodf(expected, 2.0f * (float)M_PI);
        float diff = fabsf(delta - expected);
        if (diff > 0.01f && fabsf(diff - 2.0f * (float)M_PI) > 0.01f) {
            FAIL("frequency alternation incorrect");
            return;
        }
    }
    PASS();
}

void test_generate_afsk_constant_magnitude() {
    TEST("generate_afsk maintains magnitude approximately 0.5");
    StateAFSK state;
    pre_calc_afsk(&state);
    uint8_t bit = 1;
    for (int i = 0; i < SAMPLES_PER_BIT; i++) {
        float sample = generate_afsk(&state, &bit);
        if (fabsf(sample) > 0.51f) {
            FAIL("magnitude exceeded 0.51");
            return;
        }
    }
    PASS();
}

void test_generate_afsk_continuous_phase() {
    TEST("generate_afsk maintains continuous phase between consecutive bits");
    StateAFSK state;
    pre_calc_afsk(&state);
    uint8_t bit = 1;
    float last_cos = state.current_cos;
    float last_sin = state.current_sin;
    generate_afsk(&state, &bit);
    if (fabsf(state.current_cos - (last_cos * state.step_cos_mark - last_sin * state.step_sin_mark)) > EPSILON) {
        FAIL("phase not continuous at transition");
        return;
    }
    PASS();
}

void test_push_preamble_alternating_pattern() {
    TEST("push_preamble inserts 24 bits (8 zeros + 16 alternating 1010...)");
    Buffer *rb = rb_init();
    assert(rb != NULL);
    push_preamble(rb);
    uint8_t bit;
    for (int i = 0; i < 24; i++) {
        if (!get_bits(rb, &bit)) { FAIL("preamble has fewer than 24 bits"); free(rb); return; }
        uint8_t expected;
        if (i < 8) {
            expected = 0;
        } else {
            expected = ((i - 8) % 2 == 0) ? 1 : 0;
        }
        if (bit != expected) { FAIL("preamble bit incorrect"); free(rb); return; }
    }
    PASS();
    free(rb);
}

void test_push_sync_word_correct_value() {
    TEST("push_sync_word inserts SYNC_WORD (0x930B51DE) 32 bits");
    Buffer *rb = rb_init();
    assert(rb != NULL);
    push_sync_word(rb);
    uint8_t bit;
    for (int i = 31; i >= 0; i--) {
        if (!get_bits(rb, &bit)) { FAIL("sync word has fewer than 32 bits"); free(rb); return; }
        uint8_t expected = (SYNC_WORD >> i) & 1;
        if (bit != expected) { FAIL("sync word bit incorrect"); free(rb); return; }
    }
    PASS();
    free(rb);
}

void test_push_preamble_and_sync_word_sequence() {
    TEST("push_preamble + push_sync_word in sequence");
    Buffer *rb = rb_init();
    assert(rb != NULL);
    push_preamble(rb);
    push_sync_word(rb);
    uint8_t bit;
    for (int i = 0; i < 24; i++) {
        if (!get_bits(rb, &bit)) { FAIL("preamble incomplete"); free(rb); return; }
    }
    for (int i = 31; i >= 0; i--) {
        if (!get_bits(rb, &bit)) { FAIL("sync word incomplete"); free(rb); return; }
        uint8_t expected = (SYNC_WORD >> i) & 1;
        if (bit != expected) { FAIL("sync word incorrect after preamble"); free(rb); return; }
    }
    PASS();
    free(rb);
}

int main() {
    printf("=== Complete Tests: AFSK Modulator (mod_afsk) ===\n\n");

    printf("[pre_calc_afsk]\n");
    test_pre_calc_afsk_space_coefficient();
    test_pre_calc_afsk_mark_coefficient();
    test_pre_calc_afsk_sin_coefficients();
    test_pre_calc_afsk_initial_state();

    printf("\n[generate_afsk]\n");
    test_generate_afsk_mark_phase_shift();
    test_generate_afsk_space_phase_shift();
    test_generate_afsk_frequency_alternation();
    test_generate_afsk_constant_magnitude();
    test_generate_afsk_continuous_phase();

    printf("\n[push_preamble / push_sync_word]\n");
    test_push_preamble_alternating_pattern();
    test_push_sync_word_correct_value();
    test_push_preamble_and_sync_word_sequence();

    printf("\n=== Summary: %d PASS, %d FAIL ===\n", tests_passed, tests_failed);
    return tests_failed > 0 ? 1 : 0;
}
