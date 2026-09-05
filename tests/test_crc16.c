#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "../include/crc16.h"

static int tests_passed = 0;
static int tests_failed = 0;

#define TEST(name) do { printf("  - %s ... ", name); } while(0)
#define PASS() do { printf("PASS\n"); tests_passed++; } while(0)
#define FAIL(fmt, ...) do { printf("FAIL: " fmt "\n", ##__VA_ARGS__); tests_failed++; } while(0)

/* PRNG determinístico (xorshift32) — testes reproduzíveis sem rand(). */
static uint32_t rng_state = 0x1BAD5EEDu;
static uint32_t rng_next(void) {
    uint32_t x = rng_state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    rng_state = x;
    return x;
}
static void fill_random(uint8_t *buf, int len) {
    for (int i = 0; i < len; i++) buf[i] = (uint8_t)(rng_next() & 0xFF);
}

static uint16_t frame_crc_local(uint16_t header, const uint8_t *payload, int payload_len) {
    uint8_t hdr[2] = { (uint8_t)(header >> 8), (uint8_t)(header & 0xFF) };
    uint16_t crc = crc16_ccitt(hdr, 2);
    return crc16_ccitt_carry(crc, payload, payload_len);
}

void test_crc16_golden_ccitt_vector() {
    TEST("crc16_ccitt('123456789') == 0x29B1 (vetor ouro CRC-16/CCITT-FALSE)");
    uint16_t got = crc16_ccitt((const uint8_t *)"123456789", 9);
    if (got != 0x29B1u) { FAIL("got 0x%04X", got); return; }
    PASS();
}

void test_crc16_empty_input() {
    TEST("crc16_ccitt de entrada vazia == 0xFFFF (init)");
    uint16_t got = crc16_ccitt(NULL, 0);
    if (got != 0xFFFFu) { FAIL("got 0x%04X, esperado 0xFFFF", got); return; }
    PASS();
}

void test_crc16_single_byte_vectors() {
    TEST("crc16_ccitt de 1 byte: 0x00->0xE1F0, 0xFF->0xFF00");
    uint16_t a = crc16_ccitt((const uint8_t *)"\x00", 1);
    uint16_t b = crc16_ccitt((const uint8_t *)"\xFF", 1);
    if (a != 0xE1F0u) { FAIL("0x00 -> 0x%04X, esperado 0xE1F0", a); return; }
    if (b != 0xFF00u) { FAIL("0xFF -> 0x%04X, esperado 0xFF00", b); return; }
    if (a == b) { FAIL("0x00 e 0xFF colidem"); return; }
    PASS();
}

void test_crc16_deterministic() {
    TEST("crc16_ccitt é determinístico para a mesma entrada");
    uint8_t buf[64];
    fill_random(buf, sizeof(buf));
    uint16_t a = crc16_ccitt(buf, sizeof(buf));
    uint16_t b = crc16_ccitt(buf, sizeof(buf));
    if (a != b) { FAIL("0x%04X != 0x%04X", a, b); return; }
    PASS();
}

void test_crc16_carry_equivalent_to_contiguous() {
    TEST("crc16_ccitt_carry em k segmentos == crc16_ccitt do buffer contíguo");
    uint8_t hdr[2] = { 0x40, 0x1C };
    uint8_t payload[37];
    fill_random(payload, sizeof(payload));

    uint8_t full[2 + 37];
    memcpy(full, hdr, 2);
    memcpy(full + 2, payload, 37);

    uint16_t direct = crc16_ccitt(full, sizeof(full));

    uint16_t two_seg = crc16_ccitt(hdr, 2);
    two_seg = crc16_ccitt_carry(two_seg, payload, 37);

    uint16_t three_seg = crc16_ccitt_carry(0xFFFFu, hdr, 1);
    three_seg = crc16_ccitt_carry(three_seg, hdr + 1, 1);
    three_seg = crc16_ccitt_carry(three_seg, payload, 37);

    if (direct != two_seg) { FAIL("2 segmentos: 0x%04X != 0x%04X", two_seg, direct); return; }
    if (direct != three_seg) { FAIL("3 segmentos: 0x%04X != 0x%04X", three_seg, direct); return; }
    PASS();
}

void test_crc16_all_single_bit_flips_detected() {
    TEST("crc16 detecta TODA troca de 1 bit (exaustivo p/ tamanhos 1..1000 B)");
    const int sizes[] = {1, 2, 8, 16, 32, 64, 128, 256, 1000};
    for (size_t s = 0; s < sizeof(sizes) / sizeof(sizes[0]); s++) {
        uint8_t buf[1000];
        fill_random(buf, sizes[s]);
        uint16_t base = crc16_ccitt(buf, sizes[s]);
        for (int bit = 0; bit < sizes[s] * 8; bit++) {
            uint8_t copy[1000];
            memcpy(copy, buf, sizes[s]);
            copy[bit >> 3] ^= (uint8_t)(1u << (7 - (bit & 7)));
            uint16_t got = crc16_ccitt(copy, sizes[s]);
            if (got == base) {
                FAIL("tamanho %d: flip do bit %d não detectado (0x%04X)", sizes[s], bit, base);
                return;
            }
        }
    }
    PASS();
}

void test_crc16_all_double_bit_flips_detected_small() {
    TEST("crc16 detecta TODA troca de 2 bits (exaustivo p/ tamanhos 1..8 B)");
    const int sizes[] = {1, 2, 4, 8};
    for (size_t s = 0; s < sizeof(sizes) / sizeof(sizes[0]); s++) {
        uint8_t buf[8];
        fill_random(buf, sizes[s]);
        uint16_t base = crc16_ccitt(buf, sizes[s]);
        int nb = sizes[s] * 8;
        for (int i = 0; i < nb; i++) {
            for (int j = i + 1; j < nb; j++) {
                uint8_t copy[8];
                memcpy(copy, buf, sizes[s]);
                copy[i >> 3] ^= (uint8_t)(1u << (7 - (i & 7)));
                copy[j >> 3] ^= (uint8_t)(1u << (7 - (j & 7)));
                uint16_t got = crc16_ccitt(copy, sizes[s]);
                if (got == base) {
                    FAIL("tamanho %d: par de bits (%d,%d) não detectado", sizes[s], i, j);
                    return;
                }
            }
        }
    }
    PASS();
}

void test_crc16_double_bit_flips_detected_sampled_big() {
    TEST("crc16 detecta troca de 2 bits em frames grandes (amostra 400 pares p/ 32/128/512 B)");
    const int sizes[] = {32, 128, 512};
    for (size_t s = 0; s < sizeof(sizes) / sizeof(sizes[0]); s++) {
        uint8_t buf[512];
        fill_random(buf, sizes[s]);
        uint16_t base = crc16_ccitt(buf, sizes[s]);
        int nb = sizes[s] * 8;
        for (int k = 0; k < 400; k++) {
            int i = (int)(rng_next() % nb);
            int j = (int)(rng_next() % nb);
            if (j == i) j = (j + 1) % nb;
            uint8_t copy[512];
            memcpy(copy, buf, sizes[s]);
            copy[i >> 3] ^= (uint8_t)(1u << (7 - (i & 7)));
            copy[j >> 3] ^= (uint8_t)(1u << (7 - (j & 7)));
            uint16_t got = crc16_ccitt(copy, sizes[s]);
            if (got == base) {
                FAIL("tamanho %d: par (%d,%d) colide", sizes[s], i, j);
                return;
            }
        }
    }
    PASS();
}

void test_crc16_all_bursts_up_to_16_bits_detected() {
    TEST("crc16 detecta TODO burst contíguo de 1..16 bits (frame de 16 B)");
    uint8_t buf[16];
    fill_random(buf, sizeof(buf));
    uint16_t base = crc16_ccitt(buf, sizeof(buf));
    int nb = sizeof(buf) * 8;
    for (int blen = 1; blen <= 16; blen++) {
        for (int start = 0; start + blen <= nb; start++) {
            uint8_t copy[16];
            memcpy(copy, buf, sizeof(buf));
            for (int k = 0; k < blen; k++) {
                int pos = start + k;
                copy[pos >> 3] ^= (uint8_t)(1u << (7 - (pos & 7)));
            }
            uint16_t got = crc16_ccitt(copy, sizeof(buf));
            if (got == base) {
                FAIL("burst len %d @ %d não detectado", blen, start);
                return;
            }
        }
    }
    PASS();
}

void test_crc16_byte_order_sensitive() {
    TEST("crc16 é sensível à ordem dos bytes (reverso/rotacionado)");
    uint8_t a4[4] = {1, 2, 3, 4};
    uint8_t rev[4] = {4, 3, 2, 1};
    uint8_t swapped[4] = {2, 1, 4, 3};
    uint16_t ca = crc16_ccitt(a4, 4);
    uint16_t cr = crc16_ccitt(rev, 4);
    uint16_t cs = crc16_ccitt(swapped, 4);
    if (ca == cr || ca == cs) { FAIL("colisão de ordem"); return; }
    PASS();
}

void test_crc16_avalanche_distinct() {
    TEST("crc16 gera CRC distintos para 1000 inputs aleatórios de 64 B (>= 985 únicos; ~993 esperado)");
    uint16_t seen[1000];
    for (int k = 0; k < 1000; k++) {
        uint8_t buf[64];
        fill_random(buf, sizeof(buf));
        seen[k] = crc16_ccitt(buf, sizeof(buf));
    }
    int distinct = 0;
    for (int i = 0; i < 1000; i++) {
        int dup = 0;
        for (int j = 0; j < i; j++) {
            if (seen[j] == seen[i]) { dup = 1; break; }
        }
        if (!dup) distinct++;
    }
    if (distinct < 985) { FAIL("só %d únicos de 1000", distinct); return; }
    PASS();
}

void test_frame_crc_composition() {
    TEST("frame_crc (header 2B + payload) compõe corretamente com crc16_ccitt");
    uint16_t header = 0x401C;
    uint8_t payload[21];
    fill_random(payload, sizeof(payload));
    uint16_t a = frame_crc_local(header, payload, sizeof(payload));
    uint8_t full[2 + 21];
    full[0] = (uint8_t)(header >> 8);
    full[1] = (uint8_t)(header & 0xFF);
    memcpy(full + 2, payload, sizeof(payload));
    uint16_t b = crc16_ccitt(full, sizeof(full));
    if (a != b) { FAIL("0x%04X != 0x%04X", a, b); return; }
    PASS();
}

int main(void) {
    printf("=== CRC16 (CCITT-FALSE) ===\n\n");

    printf("[vetores de referência]\n");
    test_crc16_golden_ccitt_vector();
    test_crc16_empty_input();
    test_crc16_single_byte_vectors();
    test_crc16_deterministic();

    printf("\n[composição / API]\n");
    test_crc16_carry_equivalent_to_contiguous();
    test_frame_crc_composition();
    test_crc16_byte_order_sensitive();

    printf("\n[detecção de erros — propriedades]\n");
    test_crc16_all_single_bit_flips_detected();
    test_crc16_all_double_bit_flips_detected_small();
    test_crc16_double_bit_flips_detected_sampled_big();
    test_crc16_all_bursts_up_to_16_bits_detected();
    test_crc16_avalanche_distinct();

    printf("\n=== Summary: %d PASS, %d FAIL ===\n", tests_passed, tests_failed);
    return tests_failed > 0 ? 1 : 0;
}