#include <stdio.h>
#include <stdint.h>
#include "../include/scrambler.h"

static int tests_passed = 0;
static int tests_failed = 0;

#define TEST(name) do { printf("  - %s ... ", name); } while(0)
#define PASS() do { printf("PASS\n"); tests_passed++; } while(0)
#define FAIL(fmt, ...) do { printf("FAIL: " fmt "\n", ##__VA_ARGS__); tests_failed++; } while(0)

void test_scrambler_init_nonzero_state() {
    TEST("scrambler_init sets state to 1 (non-zero)");
    Scrambler s;
    scrambler_init(&s);
    if (s.state != 1) { FAIL("state not 1"); return; }
    PASS();
}

void test_scrambler_reset_nonzero_state() {
    TEST("scrambler_reset sets state to 1");
    Scrambler s;
    s.state = 0xFFFF;
    scrambler_reset(&s);
    if (s.state != 1) { FAIL("state not 1 after reset"); return; }
    PASS();
}

void test_scrambler_process_deterministic() {
    TEST("scrambler_process is deterministic for same input/state");
    Scrambler s1, s2;
    scrambler_init(&s1);
    scrambler_init(&s2);
    for (int i = 0; i < 100; i++) {
        uint8_t out1 = scrambler_process(&s1, 0);
        uint8_t out2 = scrambler_process(&s2, 0);
        if (out1 != out2) { FAIL("outputs differ"); return; }
        out1 = scrambler_process(&s1, 1);
        out2 = scrambler_process(&s2, 1);
        if (out1 != out2) { FAIL("outputs differ for 1"); return; }
    }
    PASS();
}

void test_scrambler_self_synchronizing() {
    TEST("scrambler recovers after bit error (self-synchronizing property)");
    Scrambler tx, rx;
    scrambler_init(&tx);
    scrambler_init(&rx);
    
    /* TX and RX start in sync - perfect decode */
    for (int i = 0; i < 100; i++) {
        uint8_t bit = (i % 2);
        uint8_t tx_out = scrambler_process(&tx, bit);
        uint8_t rx_out = scrambler_process(&rx, tx_out);
        if (rx_out != bit) { FAIL("desync at bit %d", i); return; }
    }
    
    /* Now simulate a single bit error on channel - RX should recover within 17 bits */
    scrambler_init(&tx);
    scrambler_init(&rx);
    
    uint8_t bit;
    for (int i = 0; i < 50; i++) {
        bit = (i % 3 == 0) ? 1 : 0;
        uint8_t tx_out = scrambler_process(&tx, bit);
        /* Inject single bit error at i=10 */
        if (i == 10) tx_out ^= 1;
        uint8_t rx_out = scrambler_process(&rx, tx_out);
        if (i >= 27 && rx_out != bit) { FAIL("not recovered after error at bit %d", i); return; }
    }
    PASS();
}

void test_scrambler_decode_with_same_state() {
    TEST("scrambler decodes correctly when RX uses same state sequence as TX");
    Scrambler tx, rx;
    scrambler_init(&tx);
    scrambler_init(&rx);
    
    for (int i = 0; i < 200; i++) {
        uint8_t bit = (i % 7 == 0) ? 1 : 0;
        uint8_t tx_out = scrambler_process(&tx, bit);
        uint8_t rx_out = scrambler_process(&rx, tx_out);
        if (rx_out != bit) { FAIL("decode failed at bit %d", i); return; }
    }
    PASS();
}

void test_scrambler_state_evolution() {
    TEST("scrambler state evolves correctly (LFSR x^17 + x^12 + 1)");
    Scrambler s;
    scrambler_init(&s);
    
    /* With input 0, output = LSB of state after feedback */
    uint8_t bit = 0;
    for (int i = 0; i < 50; i++) {
        uint8_t out = scrambler_process(&s, bit);
        /* The feedback bit is bit 17 ^ bit 12 of OLD state */
        uint8_t feedback = ((s.state >> 17) & 1) ^ ((s.state >> 12) & 1);
        /* Actually state was already shifted, so check new state LSB */
        if (out != (s.state & 1)) { FAIL("output != state LSB"); return; }
    }
    PASS();
}

void test_scrambler_different_initial_states() {
    TEST("different initial states produce different sequences");
    Scrambler s1, s2;
    scrambler_init(&s1);
    s2.state = 2;  // different from 1
    
    int diff = 0;
    for (int i = 0; i < 50; i++) {
        uint8_t out1 = scrambler_process(&s1, 0);
        uint8_t out2 = scrambler_process(&s2, 0);
        if (out1 != out2) { diff = 1; break; }
    }
    if (!diff) { FAIL("sequences identical"); return; }
    PASS();
}

int main() {
    printf("=== Scrambler Tests ===\n\n");
    
    printf("[init/reset]\n");
    test_scrambler_init_nonzero_state();
    test_scrambler_reset_nonzero_state();
    
    printf("\n[process]\n");
    test_scrambler_process_deterministic();
    test_scrambler_self_synchronizing();
    test_scrambler_decode_with_same_state();
    test_scrambler_state_evolution();
    test_scrambler_different_initial_states();
    
    printf("\n=== Summary: %d PASS, %d FAIL ===\n", tests_passed, tests_failed);
    return tests_failed > 0 ? 1 : 0;
}