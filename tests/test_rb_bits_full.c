#include <stdio.h>
#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "../include/rb_bits.h"

static int tests_passed = 0;
static int tests_failed = 0;

#define TEST(name) do { printf("  - %s ... ", name); } while(0)
#define PASS() do { printf("PASS\n"); tests_passed++; } while(0)
#define FAIL(msg) do { printf("FAIL: %s\n", msg); tests_failed++; } while(0)

void test_rb_init_returns_non_null() {
    TEST("rb_init returns non-null pointer");
    Buffer *rb = rb_init();
    if (rb == NULL) { FAIL("returned NULL"); return; }
    free(rb);
    PASS();
}

void test_rb_init_zero_initial_state() {
    TEST("rb_init sets all fields to zero");
    Buffer *rb = rb_init();
    assert(rb != NULL);
    if (rb->head != 0) { FAIL("head != 0"); free(rb); return; }
    if (rb->tail != 0) { FAIL("tail != 0"); free(rb); return; }
    if (rb->count_put != 0) { FAIL("count_put != 0"); free(rb); return; }
    if (rb->count_get != 0) { FAIL("count_get != 0"); free(rb); return; }
    if (rb->buf[0] != 0) { FAIL("buf[0] != 0"); free(rb); return; }
    free(rb);
    PASS();
}

void test_rb_init_clears_entire_buffer() {
    TEST("rb_init zeros the entire internal buffer");
    Buffer *rb = rb_init();
    assert(rb != NULL);
    for (int i = 0; i < BUFFER_SIZE; i++) {
        if (rb->buf[i] != 0) {
            FAIL("buf[pos] != 0");
            free(rb);
            return;
        }
    }
    free(rb);
    PASS();
}

void test_put_bits_single_bit_0() {
    TEST("put_bits inserts bit 0 and returns 1");
    Buffer *rb = rb_init();
    assert(rb != NULL);
    uint8_t bit = 0;
    if (put_bits(rb, &bit) != 1) { FAIL("returned != 1"); free(rb); return; }
    if (rb->count_put != 1) { FAIL("count_put != 1"); free(rb); return; }
    if (rb->buf[rb->head] != 0) { FAIL("buf[head] wrong"); free(rb); return; }
    free(rb);
    PASS();
}

void test_put_bits_single_bit_1() {
    TEST("put_bits inserts bit 1 and returns 1");
    Buffer *rb = rb_init();
    assert(rb != NULL);
    uint8_t bit = 1;
    if (put_bits(rb, &bit) != 1) { FAIL("returned != 1"); free(rb); return; }
    if (rb->count_put != 1) { FAIL("count_put != 1"); free(rb); return; }
    if (rb->buf[rb->head] != 1) { FAIL("buf[head] wrong"); free(rb); return; }
    free(rb);
    PASS();
}

void test_put_bits_forms_complete_byte() {
    TEST("put_bits forms byte 0xAA from 8 bits (LSB first ordering)");
    Buffer *rb = rb_init();
    assert(rb != NULL);
    uint8_t bits[] = {0,1,0,1,0,1,0,1};
    for (int i = 0; i < 8; i++) {
        if (put_bits(rb, &bits[i]) != 1) { FAIL("failed mid-insertion"); free(rb); return; }
    }
    if (rb->buf[rb->head] != 0xAA) { FAIL("formed byte wrong"); free(rb); return; }
    if (rb->count_put != 8) { FAIL("count_put != 8"); free(rb); return; }
    if (rb->head != 0) { FAIL("head advanced before 8 bits"); free(rb); return; }
    free(rb);
    PASS();
}

void test_put_bits_advances_head_after_8_bits() {
    TEST("put_bits advances head after inserting 8 bits");
    Buffer *rb = rb_init();
    assert(rb != NULL);
    uint8_t bits[] = {0,1,0,1,0,1,0,1};
    for (int i = 0; i < 8; i++) put_bits(rb, &bits[i]);
    uint8_t extra = 0;
    put_bits(rb, &extra);
    if (rb->head != 1) { FAIL("head != 1 after 9 bits"); free(rb); return; }
    if (rb->count_put != 1) { FAIL("count_put != 1 after reset"); free(rb); return; }
    free(rb);
    PASS();
}

void test_put_bits_full_buffer_returns_0() {
    TEST("put_bits returns 0 when buffer is full");
    Buffer *rb = rb_init();
    assert(rb != NULL);
    uint8_t bit = 1;
    int total = 0;
    while (put_bits(rb, &bit) == 1) {
        total++;
    }
    if (total <= 0) { FAIL("zero bits inserted"); free(rb); return; }
    printf("(%d bits before full) ", total);
    PASS();
    free(rb);
}

void test_get_bits_empty_returns_0() {
    TEST("get_bits returns 0 on empty buffer, does not modify bit");
    Buffer *rb = rb_init();
    assert(rb != NULL);
    uint8_t bit = 77;
    if (get_bits(rb, &bit) != 0) { FAIL("returned != 0"); free(rb); return; }
    if (bit != 77) { FAIL("modified the bit value"); free(rb); return; }
    free(rb);
    PASS();
}

void test_get_bits_reads_bit_correctly() {
    TEST("get_bits reads back inserted bit 1");
    Buffer *rb = rb_init();
    assert(rb != NULL);
    uint8_t in = 1;
    put_bits(rb, &in);
    uint8_t out = 99;
    if (get_bits(rb, &out) != 1) { FAIL("returned != 1"); free(rb); return; }
    if (out != 1) { FAIL("bit read != 1"); free(rb); return; }
    free(rb);
    PASS();
}

void test_get_bits_reads_16_bits_correctly() {
    TEST("get_bits reads 16-bit sequence correctly");
    Buffer *rb = rb_init();
    assert(rb != NULL);
    uint8_t in[] = {1,0,1,0,1,0,1,0, 0,1,0,1,0,1,0,1};
    for (int i = 0; i < 16; i++) put_bits(rb, &in[i]);
    uint8_t out;
    for (int i = 0; i < 16; i++) {
        if (!get_bits(rb, &out)) { FAIL("get_bits failed"); free(rb); return; }
        if (out != in[i]) { FAIL("bit read != bit inserted"); free(rb); return; }
    }
    PASS();
    free(rb);
}

void test_get_bits_underrun_after_full_consume() {
    TEST("get_bits returns 0 after consuming all bits");
    Buffer *rb = rb_init();
    assert(rb != NULL);
    uint8_t in = 1;
    put_bits(rb, &in);
    uint8_t out;
    get_bits(rb, &out);
    if (get_bits(rb, &out) != 0) { FAIL("did not return 0"); free(rb); return; }
    free(rb);
    PASS();
}

void test_get_bits_underrun_mid_byte() {
    TEST("get_bits returns 0 after consuming partial byte");
    Buffer *rb = rb_init();
    assert(rb != NULL);
    uint8_t in[] = {1,0,1};
    for (int i = 0; i < 3; i++) put_bits(rb, &in[i]);
    uint8_t out;
    for (int i = 0; i < 3; i++) {
        if (!get_bits(rb, &out)) { FAIL("get failed mid-sequence"); free(rb); return; }
    }
    if (get_bits(rb, &out) != 0) { FAIL("did not return 0"); free(rb); return; }
    free(rb);
    PASS();
}

void test_put_get_integrity_1000_bits() {
    TEST("put+get integrity for 1000 alternating bits");
    Buffer *rb = rb_init();
    assert(rb != NULL);
    for (int i = 0; i < 1000; i++) {
        uint8_t in = i % 2;
        if (!put_bits(rb, &in)) { FAIL("buffer full earlier than expected"); free(rb); return; }
    }
    for (int i = 0; i < 1000; i++) {
        uint8_t out;
        if (!get_bits(rb, &out)) { FAIL("get failed"); free(rb); return; }
        if (out != (uint8_t)(i % 2)) { FAIL("data corrupted"); free(rb); return; }
    }
    PASS();
    free(rb);
}

void test_wraparound_full_cycles() {
    TEST("wraparound: fill and drain buffer 3 consecutive times");
    Buffer *rb = rb_init();
    assert(rb != NULL);
    uint8_t bit = 1;
    for (int cycle = 0; cycle < 3; cycle++) {
        int put_count = 0;
        while (put_bits(rb, &bit) == 1) put_count++;
        int get_count = 0;
        uint8_t out;
        while (get_bits(rb, &out) == 1) get_count++;
        if (put_count != get_count) {
            FAIL("put_count != get_count on cycle");
            free(rb);
            return;
        }
        if (put_count == 0) {
            FAIL("zero bits in cycle");
            free(rb);
            return;
        }
    }
    PASS();
    free(rb);
}

void test_buffer_empty_after_drain() {
    TEST("buffer becomes empty (get returns 0) after draining completely");
    Buffer *rb = rb_init();
    assert(rb != NULL);
    uint8_t in[] = {1,1,1,1,0,0,0,0};
    for (int i = 0; i < 8; i++) put_bits(rb, &in[i]);
    uint8_t out;
    while (get_bits(rb, &out) == 1);
    if (get_bits(rb, &out) != 0) { FAIL("get_bits did not return 0"); free(rb); return; }
    PASS();
    free(rb);
}

void test_get_bits_lsb_first_ordering() {
    TEST("LSB first: bit[0] goes to bit 0 of the byte");
    Buffer *rb = rb_init();
    assert(rb != NULL);
    uint8_t bits[] = {1,1,1,1,1,1,1,1};
    for (int i = 0; i < 8; i++) put_bits(rb, &bits[i]);
    if (rb->buf[0] != 0xFF) { FAIL("formed byte != 0xFF"); free(rb); return; }
    free(rb);
    PASS();
}

int main() {
    printf("=== Complete Tests: Ring Buffer (rb_bits) ===\n\n");

    printf("[rb_init]\n");
    test_rb_init_returns_non_null();
    test_rb_init_zero_initial_state();
    test_rb_init_clears_entire_buffer();

    printf("\n[put_bits - success]\n");
    test_put_bits_single_bit_0();
    test_put_bits_single_bit_1();
    test_put_bits_forms_complete_byte();
    test_put_bits_advances_head_after_8_bits();
    test_get_bits_lsb_first_ordering();

    printf("\n[put_bits - error/edge]\n");
    test_put_bits_full_buffer_returns_0();

    printf("\n[get_bits - success]\n");
    test_get_bits_reads_bit_correctly();
    test_get_bits_reads_16_bits_correctly();

    printf("\n[get_bits - error/edge]\n");
    test_get_bits_empty_returns_0();
    test_get_bits_underrun_after_full_consume();
    test_get_bits_underrun_mid_byte();

    printf("\n[integrity]\n");
    test_put_get_integrity_1000_bits();
    test_wraparound_full_cycles();
    test_buffer_empty_after_drain();

    printf("\n=== Summary: %d PASS, %d FAIL ===\n", tests_passed, tests_failed);
    return tests_failed > 0 ? 1 : 0;
}
