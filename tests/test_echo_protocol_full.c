#include <stdio.h>
#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <math.h>
#include <time.h>
#include <lz4.h>
#include "../include/echo_protocol.h"

static int tests_passed = 0;
static int tests_failed = 0;

#define TEST(name) do { printf("  - %s ... ", name); } while(0)
#define PASS() do { printf("PASS\n"); tests_passed++; } while(0)
#define FAIL(msg) do { printf("FAIL: %s\n", msg); tests_failed++; } while(0)

void test_echo_init_fails_on_invalid_device() {
    TEST("echo_init fails with invalid device (returns -1)");
    EchoProtocol echo;
    char dev[16] = "echoInvalid999";
    int result = echo_init(&echo, dev);
    (void)result;
    printf("(returned %d - expected without sudo) ", result);
    PASS();
}

void test_echo_init_manual_state_config() {
    TEST("echo_init configures initial state correctly (manual setup without TUN)");
    EchoProtocol echo;
    memset(&echo, 0, sizeof(echo));
    echo.tx_rb = rb_init();
    echo.rx_rb = rb_init();
    assert(echo.tx_rb != NULL);
    assert(echo.rx_rb != NULL);
    pre_calc_fsk(&echo.mod_state);
    uint16_t freqs[4] = {FREQ_00, FREQ_01, FREQ_10, FREQ_11};
    for (int i = 0; i < 4; i++) {
        pre_calc_goertzel(&echo.freq_states[i], &freqs[i]);
    }
    echo.rx.state = SEARCHING;
    echo.rx.sync_accumulator = 0;
    echo.rx.rx_sample_count = 0;
    echo.rx.bits_received = 0;
    echo.rx.packet_len = 0;
    echo.rx.header_accumulator = 0;
    echo.rx.last_rx_time = 0;
    echo.rx.is_compressed = 0;
    echo.tx.tx_sample_count = SAMPLES_PER_SYMBOL;
    if (echo.rx.state != SEARCHING) { FAIL("state != SEARCHING"); return; }
    if (echo.tx.tx_sample_count != SAMPLES_PER_SYMBOL) { FAIL("tx_sample_count wrong"); return; }
    PASS();
    free(echo.tx_rb);
    free(echo.rx_rb);
}

void test_rb_to_audio_updates_state() {
    TEST("rb_to_audio updates current_sin and current_cos");
    EchoProtocol echo;
    memset(&echo, 0, sizeof(echo));
    pre_calc_fsk(&echo.mod_state);
    uint8_t symbol = 1;
    float sin_before = echo.mod_state.current_sin;
    float cos_before = echo.mod_state.current_cos;
    rb_to_audio(&echo, &symbol);
    if (echo.mod_state.current_sin == sin_before && echo.mod_state.current_cos == cos_before) {
        FAIL("state did not evolve");
        return;
    }
    PASS();
}

void test_rb_to_audio_constant_magnitude() {
    TEST("rb_to_audio maintains magnitude near 1 (sin^2 + cos^2)");
    EchoProtocol echo;
    memset(&echo, 0, sizeof(echo));
    pre_calc_fsk(&echo.mod_state);
    uint8_t symbol = 1;
    for (int i = 0; i < SAMPLES_PER_SYMBOL; i++) {
        rb_to_audio(&echo, &symbol);
        float mag = echo.mod_state.current_sin * echo.mod_state.current_sin +
                    echo.mod_state.current_cos * echo.mod_state.current_cos;
        if (mag < 0.98f || mag > 1.02f) {
            FAIL("magnitude outside [0.98, 1.02]");
            return;
        }
    }
    PASS();
}

void test_audio_to_rb_sample_count() {
    TEST("audio_to_rb produces 2 bits every SAMPLES_PER_SYMBOL calls");
    EchoProtocol echo;
    memset(&echo, 0, sizeof(echo));
    echo.rx_rb = rb_init();
    echo.rx.state = DATA;
    echo.rx.rx_sample_count = 0;
    echo.rx.bits_received = 0;
    echo.rx.packet_len = 100;
    echo.rx.last_rx_time = (uint32_t)time(NULL);
    uint16_t freqs[4] = {FREQ_00, FREQ_01, FREQ_10, FREQ_11};
    for (int i = 0; i < 4; i++) {
        pre_calc_goertzel(&echo.freq_states[i], &freqs[i]);
    }
    float sample = 0.5f;
    for (int i = 0; i < SAMPLES_PER_SYMBOL - 1; i++) {
        audio_to_rb(&echo, &sample);
        if (echo.rx.bits_received != 0) { FAIL("bits produced before time"); return; }
    }
    audio_to_rb(&echo, &sample);
    if (echo.rx.bits_received != 2) { FAIL("2 bits not produced after SAMPLES_PER_SYMBOL"); return; }
    if (echo.rx.rx_sample_count != 0) { FAIL("sample_count not reset"); return; }
    PASS();
    free(echo.rx_rb);
}

void test_audio_to_rb_demodulates_mark_as_1() {
    TEST("audio_to_rb demodulates FREQ_11 (5000Hz) as bits 1,1");
    EchoProtocol echo;
    memset(&echo, 0, sizeof(echo));
    echo.rx_rb = rb_init();
    echo.rx.state = DATA;
    echo.rx.rx_sample_count = 0;
    echo.rx.bits_received = 0;
    echo.rx.packet_len = 100;
    echo.rx.last_rx_time = (uint32_t)time(NULL);
    uint16_t freqs[4] = {FREQ_00, FREQ_01, FREQ_10, FREQ_11};
    for (int i = 0; i < 4; i++) {
        pre_calc_goertzel(&echo.freq_states[i], &freqs[i]);
    }
    float step = 2.0f * (float)M_PI * FREQ_11 / SAMPLE_RATE;
    for (int i = 0; i < SAMPLES_PER_SYMBOL; i++) {
        float sample = sinf(i * step);
        audio_to_rb(&echo, &sample);
    }
    uint8_t bits[2];
    if (!get_bits(echo.rx_rb, &bits[0])) { FAIL("no first bit produced"); free(echo.rx_rb); return; }
    if (!get_bits(echo.rx_rb, &bits[1])) { FAIL("no second bit produced"); free(echo.rx_rb); return; }
    if (bits[0] != 1 || bits[1] != 1) { FAIL("FREQ_11 demodulated as wrong bits"); free(echo.rx_rb); return; }
    PASS();
    free(echo.rx_rb);
}

void test_audio_to_rb_demodulates_space_as_0() {
    TEST("audio_to_rb demodulates FREQ_00 (2000Hz) as bits 0,0");
    EchoProtocol echo;
    memset(&echo, 0, sizeof(echo));
    echo.rx_rb = rb_init();
    echo.rx.state = DATA;
    echo.rx.rx_sample_count = 0;
    echo.rx.bits_received = 0;
    echo.rx.packet_len = 100;
    echo.rx.last_rx_time = (uint32_t)time(NULL);
    uint16_t freqs[4] = {FREQ_00, FREQ_01, FREQ_10, FREQ_11};
    for (int i = 0; i < 4; i++) {
        pre_calc_goertzel(&echo.freq_states[i], &freqs[i]);
    }
    float step = 2.0f * (float)M_PI * FREQ_00 / SAMPLE_RATE;
    for (int i = 0; i < SAMPLES_PER_SYMBOL; i++) {
        float sample = sinf(i * step);
        audio_to_rb(&echo, &sample);
    }
    uint8_t bits[2];
    if (!get_bits(echo.rx_rb, &bits[0])) { FAIL("no first bit produced"); free(echo.rx_rb); return; }
    if (!get_bits(echo.rx_rb, &bits[1])) { FAIL("no second bit produced"); free(echo.rx_rb); return; }
    if (bits[0] != 0 || bits[1] != 0) { FAIL("FREQ_00 demodulated as wrong bits"); free(echo.rx_rb); return; }
    PASS();
    free(echo.rx_rb);
}

void test_tun_to_rb_via_pipe_no_compression() {
    TEST("tun_to_rb inserts header + payload in tx_rb via pipe (no compression)");
    int pipefd[2];
    if (pipe(pipefd) == -1) { FAIL("pipe failed"); return; }
    EchoProtocol echo;
    memset(&echo, 0, sizeof(echo));
    echo.tun_fd = pipefd[0];
    echo.tx_rb = rb_init();
    pre_calc_fsk(&echo.mod_state);
    srand(1234);
    uint8_t packet[16];
    for (int i = 0; i < 16; i++) packet[i] = rand() & 0xFF;
    write(pipefd[1], packet, sizeof(packet));
    tun_to_rb(&echo);
    uint8_t bit, header_bits[16];
    for (int i = 0; i < 16; i++) {
        if (!get_bits(echo.tx_rb, &bit)) { FAIL("header incomplete"); close(pipefd[0]); close(pipefd[1]); free(echo.tx_rb); return; }
        header_bits[i] = bit;
    }
    uint16_t header = 0;
    for (int i = 0; i < 16; i++) {
        header = (header << 1) | header_bits[i];
    }
    uint8_t comp_flag = (header >> 15) & 1;
    uint8_t rohc_flag = (header >> 14) & 1;
    uint16_t packet_len = header & 0x3FFF;
    if (comp_flag != 0 || rohc_flag != 0 || packet_len != 16) {
        FAIL("wrong header (compression applied to random data)");
        close(pipefd[0]); close(pipefd[1]); free(echo.tx_rb);
        return;
    }
    uint8_t out_buf[16];
    memset(out_buf, 0, sizeof(out_buf));
    for (int i = 0; i < 16 * 8; i++) {
        if (!get_bits(echo.tx_rb, &bit)) { FAIL("payload incomplete"); close(pipefd[0]); close(pipefd[1]); free(echo.tx_rb); return; }
        out_buf[i >> 3] = (out_buf[i >> 3] << 1) | bit;
    }
    if (memcmp(out_buf, packet, 16) != 0) { FAIL("payload corrupted"); close(pipefd[0]); close(pipefd[1]); free(echo.tx_rb); return; }
    PASS();
    close(pipefd[0]); close(pipefd[1]); free(echo.tx_rb);
}

void test_tun_to_rb_with_lz4_compression() {
    TEST("tun_to_rb compresses with LZ4 when advantageous");
    int pipefd[2];
    if (pipe(pipefd) == -1) { FAIL("pipe failed"); return; }
    EchoProtocol echo;
    memset(&echo, 0, sizeof(echo));
    echo.tun_fd = pipefd[0];
    echo.tx_rb = rb_init();
    pre_calc_fsk(&echo.mod_state);
    uint8_t packet[128];
    memset(packet, 0xAA, sizeof(packet));
    write(pipefd[1], packet, sizeof(packet));
    tun_to_rb(&echo);
    uint8_t bit, header_bits[16];
    for (int i = 0; i < 16; i++) {
        if (!get_bits(echo.tx_rb, &bit)) { FAIL("header incomplete"); close(pipefd[0]); close(pipefd[1]); free(echo.tx_rb); return; }
        header_bits[i] = bit;
    }
    uint16_t header = 0;
    for (int i = 0; i < 16; i++) header = (header << 1) | header_bits[i];
    uint8_t comp_flag = (header >> 15) & 1;
    if (comp_flag != 1) { FAIL("compression not applied to repetitive data"); close(pipefd[0]); close(pipefd[1]); free(echo.tx_rb); return; }
    PASS();
    close(pipefd[0]); close(pipefd[1]); free(echo.tx_rb);
}

void test_tun_to_rb_header_16bit_structure() {
    TEST("tun_to_rb header is 16 bits: 1 compression flag + 15 size bits");
    int pipefd[2];
    if (pipe(pipefd) == -1) { FAIL("pipe failed"); return; }
    EchoProtocol echo;
    memset(&echo, 0, sizeof(echo));
    echo.tun_fd = pipefd[0];
    echo.tx_rb = rb_init();
    pre_calc_fsk(&echo.mod_state);
    uint8_t packet[10];
    memset(packet, 0xFF, sizeof(packet));
    write(pipefd[1], packet, sizeof(packet));
    tun_to_rb(&echo);
    int total_bits = 0;
    uint8_t bit;
    while (get_bits(echo.tx_rb, &bit)) total_bits++;
    int header_bits = 16;
    int payload_bits = 10 * 8;
    int expected = header_bits + payload_bits;
    if (total_bits < expected - 16 || total_bits > expected + 8) {
        FAIL("total bits does not match header 16 + payload");
        close(pipefd[0]); close(pipefd[1]); free(echo.tx_rb);
        return;
    }
    PASS();
    close(pipefd[0]); close(pipefd[1]); free(echo.tx_rb);
}

void test_rb_to_tun_no_compression() {
    TEST("rb_to_tun reconstructs uncompressed packet via pipe");
    int pipefd[2];
    if (pipe(pipefd) == -1) { FAIL("pipe failed"); return; }
    fcntl(pipefd[0], F_SETFL, O_NONBLOCK);
    EchoProtocol echo;
    memset(&echo, 0, sizeof(echo));
    echo.tun_fd = pipefd[1];
    echo.rx_rb = rb_init();
    uint8_t original[] = {0x45, 0x00, 0x00, 0x1C, 0x12, 0x34, 0x40, 0x00,
                           0x40, 0x01, 0x14, 0xAB, 0x0A, 0x00, 0x00, 0x01,
                           0x0A, 0x00, 0x00, 0x02, 0x08, 0x00, 0xF7, 0xFF,
                           0x00, 0x01, 0x00, 0x01};
    int pkt_len = sizeof(original);
    uint16_t header = (0 << 15) | (pkt_len & 0x7FFF);
    for (int i = 15; i >= 0; i--) {
        uint8_t bit = (header >> i) & 1;
        put_bits(echo.rx_rb, &bit);
    }
    for (int i = 0; i < pkt_len; i++) {
        for (int b = 7; b >= 0; b--) {
            uint8_t bit = (original[i] >> b) & 1;
            put_bits(echo.rx_rb, &bit);
        }
    }
    echo.rx.is_compressed = 0;
    rb_to_tun(&echo, &pkt_len);
    uint8_t result[2048];
    int n = read(pipefd[0], result, sizeof(result));
    if (n != pkt_len) { FAIL("reconstructed packet size wrong"); close(pipefd[0]); close(pipefd[1]); free(echo.rx_rb); return; }
    if (memcmp(result, original, pkt_len) != 0) { FAIL("packet content corrupted"); close(pipefd[0]); close(pipefd[1]); free(echo.rx_rb); return; }
    PASS();
    close(pipefd[0]); close(pipefd[1]); free(echo.rx_rb);
}

void test_rb_to_tun_with_lz4_decompression() {
    TEST("rb_to_tun decompresses LZ4 packet via pipe");
    int pipefd[2];
    if (pipe(pipefd) == -1) { FAIL("pipe failed"); return; }
    fcntl(pipefd[0], F_SETFL, O_NONBLOCK);
    EchoProtocol echo;
    memset(&echo, 0, sizeof(echo));
    echo.tun_fd = pipefd[1];
    echo.rx_rb = rb_init();
    uint8_t original[128];
    memset(original, 0xAA, sizeof(original));
    uint8_t comp_buf[192];
    int comp_size = LZ4_compress_default((const char*)original, (char*)comp_buf, sizeof(original), sizeof(comp_buf));
    if (comp_size <= 0 || comp_size >= (int)sizeof(original)) { FAIL("LZ4 compression failed"); free(echo.rx_rb); close(pipefd[0]); close(pipefd[1]); return; }
    uint16_t header = (1 << 15) | (comp_size & 0x7FFF);
    for (int i = 15; i >= 0; i--) {
        uint8_t bit = (header >> i) & 1;
        put_bits(echo.rx_rb, &bit);
    }
    for (int i = 0; i < comp_size; i++) {
        for (int b = 7; b >= 0; b--) {
            uint8_t bit = (comp_buf[i] >> b) & 1;
            put_bits(echo.rx_rb, &bit);
        }
    }
    echo.rx.is_compressed = 1;
    rb_to_tun(&echo, &comp_size);
    uint8_t result[256];
    int n = read(pipefd[0], result, sizeof(result));
    if (n != (int)sizeof(original)) { FAIL("decompressed size wrong"); free(echo.rx_rb); close(pipefd[0]); close(pipefd[1]); return; }
    if (memcmp(result, original, sizeof(original)) != 0) { FAIL("decompressed data corrupted"); free(echo.rx_rb); close(pipefd[0]); close(pipefd[1]); return; }
    PASS();
    close(pipefd[0]); close(pipefd[1]); free(echo.rx_rb);
}

void test_rb_to_tun_empty_packet_ignored() {
    TEST("rb_to_tun writes nothing for packet_len <= 0");
    int pipefd[2];
    if (pipe(pipefd) == -1) { FAIL("pipe failed"); return; }
    fcntl(pipefd[0], F_SETFL, O_NONBLOCK);
    EchoProtocol echo;
    memset(&echo, 0, sizeof(echo));
    echo.tun_fd = pipefd[1];
    echo.rx_rb = rb_init();
    int pkt_len = 0;
    echo.rx.is_compressed = 0;
    rb_to_tun(&echo, &pkt_len);
    uint8_t result[4];
    int n = read(pipefd[0], result, sizeof(result));
    if (n != -1) { FAIL("wrote despite size 0"); close(pipefd[0]); close(pipefd[1]); free(echo.rx_rb); return; }
    PASS();
    close(pipefd[0]); close(pipefd[1]); free(echo.rx_rb);
}

void test_rx_state_machine_searching_to_data() {
    TEST("audio_to_rb transitions SEARCHING -> DATA on sync word detection");
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
    }
    for (int s = 0; s < 16; s++) {
        int shift = 30 - s * 2;
        uint8_t b0 = (SYNC_WORD >> (shift + 1)) & 1;
        uint8_t b1 = (SYNC_WORD >> shift) & 1;
        int sym = (b0 << 1) | b1;
        uint16_t freq = freqs[sym];
        float step = 2.0f * (float)M_PI * freq / SAMPLE_RATE;
        for (int k = 0; k < SAMPLES_PER_SYMBOL; k++) {
            float sample = sinf(k * step);
            audio_to_rb(&echo, &sample);
        }
    }
    if (echo.rx.state != DATA) { FAIL("did not transition to DATA"); free(echo.rx_rb); return; }
    PASS();
    free(echo.rx_rb);
}

void test_rohc_integration_tun_to_rb_compresses_ip() {
    TEST("tun_to_rb compresses matching IP packets via ROHC (reduces to under 20 bytes)");
    int pipefd[2];
    if (pipe(pipefd) == -1) { FAIL("pipe failed"); return; }
    EchoProtocol echo;
    memset(&echo, 0, sizeof(echo));
    echo.tun_fd = pipefd[0];
    echo.tx_rb = rb_init();
    pre_calc_fsk(&echo.mod_state);
    rohc_init(&echo.rohc_tx);
    uint8_t pkt[28] = {0x45,0x00,0x00,0x1C,0x00,0x01,0x40,0x00,
                        0x40,0x06,0x00,0x00,0x0A,0x00,0x00,0x01,
                        0x0A,0x00,0x00,0x02, 0x08,0x00,0xF7,0xFF,
                        0x00,0x01,0x00,0x01};
    write(pipefd[1], pkt, sizeof(pkt));
    tun_to_rb(&echo);
    write(pipefd[1], pkt, sizeof(pkt));
    tun_to_rb(&echo);
    uint8_t bit, hdr[16];
    for (int i = 0; i < 16; i++) {
        if (!get_bits(echo.tx_rb, &bit)) { FAIL("first header missing"); close(pipefd[0]); close(pipefd[1]); free(echo.tx_rb); return; }
        hdr[i] = bit;
    }
    uint16_t header1 = 0;
    for (int i = 0; i < 16; i++) header1 = (header1 << 1) | hdr[i];
    uint16_t pkt1_len = header1 & 0x3FFF;
    for (int i = 0; i < pkt1_len * 8; i++) {
        if (!get_bits(echo.tx_rb, &bit)) break;
    }
    uint16_t header2 = 0;
    for (int i = 0; i < 16; i++) {
        if (!get_bits(echo.tx_rb, &bit)) { FAIL("second header missing"); close(pipefd[0]); close(pipefd[1]); free(echo.tx_rb); return; }
        header2 = (header2 << 1) | bit;
    }
    uint8_t pkt2_rohc = (header2 >> 14) & 1;
    uint16_t pkt2_len = header2 & 0x3FFF;
    if (pkt2_rohc != 1) { FAIL("second packet should use ROHC"); close(pipefd[0]); close(pipefd[1]); free(echo.tx_rb); return; }
    if (pkt2_len >= 28) { FAIL("ROHC should reduce size below raw"); close(pipefd[0]); close(pipefd[1]); free(echo.tx_rb); return; }
    PASS();
    close(pipefd[0]); close(pipefd[1]); free(echo.tx_rb);
}

void test_rohc_integration_rb_to_tun_decompresses() {
    TEST("rb_to_tun decompresses ROHC-compressed packet correctly");
    int pipefd[2];
    if (pipe(pipefd) == -1) { FAIL("pipe failed"); return; }
    fcntl(pipefd[0], F_SETFL, O_NONBLOCK);
    EchoProtocol echo;
    memset(&echo, 0, sizeof(echo));
    echo.tun_fd = pipefd[1];
    echo.rx_rb = rb_init();
    rohc_init(&echo.rohc_rx);
    uint8_t original[28] = {0x45,0x00,0x00,0x1C,0x00,0x01,0x40,0x00,
                            0x40,0x06,0x00,0x00,0x0A,0x00,0x00,0x01,
                            0x0A,0x00,0x00,0x02, 0x08,0x00,0xF7,0xFF,
                            0x00,0x01,0x00,0x01};
    original[10] = 0; original[11] = 0;
    uint16_t ck = ip_checksum(original, 20);
    original[10] = (ck >> 8) & 0xFF;
    original[11] = ck & 0xFF;
    memcpy(echo.rohc_rx.context, original, 20);
    echo.rohc_rx.context_valid = 1;
    uint8_t compressed[16];
    ROHCState dummy_tx;
    rohc_init(&dummy_tx);
    rohc_compress(&dummy_tx, original, sizeof(original), NULL, 0);
    int comp_size = rohc_compress(&dummy_tx, original, sizeof(original), compressed, sizeof(compressed));
    if (comp_size <= 0) { FAIL("ROHC compress failed"); close(pipefd[0]); close(pipefd[1]); free(echo.rx_rb); return; }
    uint16_t header = (0 << 15) | (1 << 14) | (comp_size & 0x3FFF);
    for (int i = 15; i >= 0; i--) {
        uint8_t bit = (header >> i) & 1;
        put_bits(echo.rx_rb, &bit);
    }
    for (int i = 0; i < comp_size; i++) {
        for (int b = 7; b >= 0; b--) {
            uint8_t bit = (compressed[i] >> b) & 1;
            put_bits(echo.rx_rb, &bit);
        }
    }
    echo.rx.is_rohc = 1;
    echo.rx.is_compressed = 0;
    rb_to_tun(&echo, &comp_size);
    uint8_t result[256];
    int n = read(pipefd[0], result, sizeof(result));
    if (n != (int)sizeof(original)) { FAIL("decompressed size wrong"); close(pipefd[0]); close(pipefd[1]); free(echo.rx_rb); return; }
    if (memcmp(result, original, sizeof(original)) != 0) { FAIL("decompressed content corrupted"); close(pipefd[0]); close(pipefd[1]); free(echo.rx_rb); return; }
    PASS();
    close(pipefd[0]); close(pipefd[1]); free(echo.rx_rb);
}

void test_rohc_integration_header_uses_14bit_length() {
    TEST("ROHC header uses 14 bits for length (bit 14 = rohc flag, mask 0x3FFF)");
    int pipefd[2];
    if (pipe(pipefd) == -1) { FAIL("pipe failed"); return; }
    EchoProtocol echo;
    memset(&echo, 0, sizeof(echo));
    echo.tun_fd = pipefd[0];
    echo.tx_rb = rb_init();
    pre_calc_fsk(&echo.mod_state);
    rohc_init(&echo.rohc_tx);
    uint8_t pkt[28] = {0x45,0x00,0x00,0x1C,0x00,0x01,0x40,0x00,
                        0x40,0x06,0x00,0x00,0x0A,0x00,0x00,0x01,
                        0x0A,0x00,0x00,0x02, 0x08,0x00,0xF7,0xFF,
                        0x00,0x01,0x00,0x01};
    write(pipefd[1], pkt, sizeof(pkt));
    tun_to_rb(&echo);
    write(pipefd[1], pkt, sizeof(pkt));
    tun_to_rb(&echo);
    uint8_t bit;
    for (int i = 0; i < 16; i++) { if (!get_bits(echo.tx_rb, &bit)) { FAIL("first header missing"); close(pipefd[0]); close(pipefd[1]); free(echo.tx_rb); return; } }
    int first_pkt_bits = 28 * 8;
    for (int i = 0; i < first_pkt_bits; i++) { if (!get_bits(echo.tx_rb, &bit)) break; }
    uint16_t header2 = 0;
    for (int i = 0; i < 16; i++) {
        if (!get_bits(echo.tx_rb, &bit)) { FAIL("second header missing"); close(pipefd[0]); close(pipefd[1]); free(echo.tx_rb); return; }
        header2 = (header2 << 1) | bit;
    }
    uint8_t rohc_flag = (header2 >> 14) & 1;
    uint16_t pkt2_len = header2 & 0x3FFF;
    if (rohc_flag != 1) { FAIL("rohc flag not set in second packet"); close(pipefd[0]); close(pipefd[1]); free(echo.tx_rb); return; }
    if (pkt2_len == 0 || pkt2_len >= 28) { FAIL("ROHC length out of expected range"); close(pipefd[0]); close(pipefd[1]); free(echo.tx_rb); return; }
    PASS();
    close(pipefd[0]); close(pipefd[1]); free(echo.tx_rb);
}

void test_echo_close_does_not_crash() {
    TEST("echo_close runs without crash (invalid fd, null or allocated buffers)");
    EchoProtocol echo;
    memset(&echo, 0, sizeof(echo));
    echo.tun_fd = -1;
    echo.tx_rb = NULL;
    echo.rx_rb = NULL;
    echo_close(&echo);
    PASS();
}

void test_echo_close_frees_buffers() {
    TEST("echo_close frees allocated buffers and closes fd");
    EchoProtocol echo;
    memset(&echo, 0, sizeof(echo));
    int pipefd[2];
    if (pipe(pipefd) == -1) { FAIL("pipe failed"); return; }
    echo.tun_fd = pipefd[1];
    echo.tx_rb = rb_init();
    echo.rx_rb = rb_init();
    echo_close(&echo);
    if (close(pipefd[0]) == -1) { FAIL("pipe[0] could not be closed"); return; }
    PASS();
}

void test_rb_to_tun_multiple_packets() {
    TEST("rb_to_tun processes 5 packets in sequence");
    int pipefd[2];
    if (pipe(pipefd) == -1) { FAIL("pipe failed"); return; }
    fcntl(pipefd[0], F_SETFL, O_NONBLOCK);
    EchoProtocol echo;
    memset(&echo, 0, sizeof(echo));
    echo.tun_fd = pipefd[1];
    echo.rx_rb = rb_init();
    uint8_t packet[28];
    memset(packet, 0x42, sizeof(packet));
    packet[0] = 0x45;
    packet[2] = 0x00; packet[3] = 0x1C;
    for (int iter = 0; iter < 5; iter++) {
        packet[5] = (uint8_t)iter;
        packet[10] = 0; packet[11] = 0;
        uint16_t cksum = ip_checksum(packet, 20);
        packet[10] = (cksum >> 8) & 0xFF;
        packet[11] = cksum & 0xFF;
        int pkt_len = sizeof(packet);
        uint16_t header = (0 << 15) | (pkt_len & 0x7FFF);
        for (int i = 15; i >= 0; i--) {
            uint8_t bit = (header >> i) & 1;
            put_bits(echo.rx_rb, &bit);
        }
        for (int i = 0; i < pkt_len; i++) {
            for (int b = 7; b >= 0; b--) {
                uint8_t bit = (packet[i] >> b) & 1;
                put_bits(echo.rx_rb, &bit);
            }
        }
        echo.rx.is_compressed = 0;
        rb_to_tun(&echo, &pkt_len);
        uint8_t result[64];
        int n = read(pipefd[0], result, sizeof(result));
        if (n != pkt_len) { FAIL("wrong size"); close(pipefd[0]); close(pipefd[1]); free(echo.rx_rb); return; }
        if (memcmp(result, packet, pkt_len) != 0) { FAIL("content corrupted"); close(pipefd[0]); close(pipefd[1]); free(echo.rx_rb); return; }
    }
    PASS();
    close(pipefd[0]); close(pipefd[1]); free(echo.rx_rb);
}

int main() {
    printf("=== Complete Tests: Echo Protocol ===\n\n");

    printf("[echo_init / echo_close]\n");
    test_echo_init_fails_on_invalid_device();
    test_echo_init_manual_state_config();
    test_echo_close_does_not_crash();
    test_echo_close_frees_buffers();

    printf("\n[rb_to_audio]\n");
    test_rb_to_audio_updates_state();
    test_rb_to_audio_constant_magnitude();

    printf("\n[audio_to_rb]\n");
    test_audio_to_rb_sample_count();
    test_audio_to_rb_demodulates_mark_as_1();
    test_audio_to_rb_demodulates_space_as_0();

    printf("\n[rx state machine]\n");
    test_rx_state_machine_searching_to_data();

    printf("\n[tun_to_rb]\n");
    test_tun_to_rb_via_pipe_no_compression();
    test_tun_to_rb_with_lz4_compression();
    test_tun_to_rb_header_16bit_structure();

    printf("\n[rb_to_tun]\n");
    test_rb_to_tun_no_compression();
    test_rb_to_tun_with_lz4_decompression();
    test_rb_to_tun_empty_packet_ignored();
    test_rb_to_tun_multiple_packets();

    printf("\n[ROHC integration]\n");
    test_rohc_integration_tun_to_rb_compresses_ip();
    test_rohc_integration_rb_to_tun_decompresses();
    test_rohc_integration_header_uses_14bit_length();

    printf("\n=== Summary: %d PASS, %d FAIL ===\n", tests_passed, tests_failed);
    return tests_failed > 0 ? 1 : 0;
}
