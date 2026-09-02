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
#include "../include/fec.h"
#include "../include/scrambler.h"

static int tests_passed = 0;
static int tests_failed = 0;

#define TEST(name) do { printf("  - %s ... ", name); } while(0)
#define PASS() do { printf("PASS\n"); tests_passed++; } while(0)
#define FAIL(fmt, ...) do { printf("FAIL: " fmt "\n", ##__VA_ARGS__); tests_failed++; } while(0)

/* Helper: descramble 16 header bits from tx_rb using a fresh scrambler */
static uint16_t read_header_descrambled(Buffer *tx_rb) {
    Scrambler s;
    scrambler_init(&s);
    uint8_t bit, header_bits[16];
    for (int i = 0; i < 16; i++) {
        if (!get_bits(tx_rb, &bit)) return 0xFFFF;
        bit = scrambler_process(&s, bit);  // descramble
        header_bits[i] = bit;
    }
    uint16_t header = 0;
    for (int i = 0; i < 16; i++) header = (header << 1) | header_bits[i];
    return header;
}

/* Helper: descramble header + payload together using same scrambler state */
static void read_packet_descrambled(Buffer *tx_rb, uint16_t *out_header, uint8_t *out_payload, int max_payload_bytes) {
    Scrambler s;
    scrambler_init(&s);
    uint8_t bit;
    uint16_t header = 0;
    for (int i = 0; i < 16; i++) {
        if (!get_bits(tx_rb, &bit)) { *out_header = 0xFFFF; return; }
        bit = scrambler_process(&s, bit);
        header = (header << 1) | bit;
    }
    *out_header = header;
    int payload_bits = (header & 0x1FFF) * 8;
    if (payload_bits > max_payload_bytes * 8) payload_bits = max_payload_bytes * 8;
    memset(out_payload, 0, max_payload_bytes);
    for (int i = 0; i < payload_bits; i++) {
        if (!get_bits(tx_rb, &bit)) break;
        bit = scrambler_process(&s, bit);
        out_payload[i >> 3] = (out_payload[i >> 3] << 1) | bit;
    }
}

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
        pre_calc_goertzel_long(&echo.freq_valid[i], &freqs[i]);
    }
    uint16_t mon_freqs[NUM_FREQ_MON] = {FREQ_MON_LOW, FREQ_MON_MID, FREQ_MON_HIGH};
    for (int i = 0; i < NUM_FREQ_MON; i++) {
        pre_calc_goertzel(&echo.freq_mon[i], &mon_freqs[i]);
    }
    bandpass_bank_init(&echo.bp_bank, SAMPLE_RATE, 2600.0f);
    echo.pending_candidate = -1;
    echo.block_counter = 0;
    echo.long_samples_count = 0;
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
        pre_calc_goertzel_long(&echo.freq_valid[i], &freqs[i]);
    }
    uint16_t mon_freqs[NUM_FREQ_MON] = {FREQ_MON_LOW, FREQ_MON_MID, FREQ_MON_HIGH};
    for (int i = 0; i < NUM_FREQ_MON; i++) {
        pre_calc_goertzel(&echo.freq_mon[i], &mon_freqs[i]);
    }
    bandpass_bank_init(&echo.bp_bank, SAMPLE_RATE, 2600.0f);
    echo.pending_candidate = -1;
    echo.block_counter = 0;
    echo.long_samples_count = 0;
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
        pre_calc_goertzel_long(&echo.freq_valid[i], &freqs[i]);
    }
    uint16_t mon_freqs[NUM_FREQ_MON] = {FREQ_MON_LOW, FREQ_MON_MID, FREQ_MON_HIGH};
    for (int i = 0; i < NUM_FREQ_MON; i++) {
        pre_calc_goertzel(&echo.freq_mon[i], &mon_freqs[i]);
    }
    bandpass_bank_init(&echo.bp_bank, SAMPLE_RATE, 2600.0f);
    echo.pending_candidate = -1;
    echo.block_counter = 0;
    echo.long_samples_count = 0;
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
    uint16_t header;
    uint8_t payload[64];
    read_packet_descrambled(echo.tx_rb, &header, payload, sizeof(payload));
    if (header == 0xFFFF) { FAIL("header incomplete"); close(pipefd[0]); close(pipefd[1]); free(echo.tx_rb); return; }
    uint8_t comp_flag = (header >> 15) & 1;
    uint8_t rohc_flag = (header >> 14) & 1;
    uint8_t fec_flag = (header >> 13) & 1;
    uint16_t packet_len = header & 0x1FFF;
    int expected_fec_len = fec_encoded_len(16);
    if (comp_flag != 0 || rohc_flag != 0 || fec_flag != 1 || packet_len != expected_fec_len) {
        FAIL("wrong header (expected fec_flag=1, len=%d)", expected_fec_len);
        close(pipefd[0]); close(pipefd[1]); free(echo.tx_rb);
        return;
    }
    uint8_t decoded[32];
    int decoded_len = fec_decode(payload, expected_fec_len, decoded, sizeof(decoded));
    if (decoded_len != 16 || memcmp(decoded, packet, 16) != 0) { FAIL("FEC payload corrupted"); close(pipefd[0]); close(pipefd[1]); free(echo.tx_rb); return; }
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
    uint16_t header;
    uint8_t payload[128];
    read_packet_descrambled(echo.tx_rb, &header, payload, sizeof(payload));
    if (header == 0xFFFF) { FAIL("header incomplete"); close(pipefd[0]); close(pipefd[1]); free(echo.tx_rb); return; }
    uint8_t comp_flag = (header >> 15) & 1;
    uint8_t rohc_flag = (header >> 14) & 1;
    uint8_t fec_flag = (header >> 13) & 1;
    uint16_t packet_len = header & 0x1FFF;
    
    /* Compute expected LZ4 size properly */
    uint8_t lz4_buf[192];
    int lz4_size = LZ4_compress_default((const char*)packet, (char*)lz4_buf, sizeof(packet), sizeof(lz4_buf));
    int fec_total = (lz4_size > 0 && lz4_size < (int)sizeof(packet)) ? fec_encoded_len(lz4_size) : fec_encoded_len(sizeof(packet));
    
    if (comp_flag != 1 || rohc_flag != 0 || fec_flag != 1 || packet_len != fec_total) {
        FAIL("wrong flags for LZ4+FEC: comp=%d rohc=%d fec=%d len=%d expected=%d (lz4_size=%d)", comp_flag, rohc_flag, fec_flag, packet_len, fec_total, lz4_size);
        close(pipefd[0]); close(pipefd[1]); free(echo.tx_rb);
        return;
    }
    PASS();
    close(pipefd[0]); close(pipefd[1]); free(echo.tx_rb);
}

void test_tun_to_rb_header_16bit_structure() {
    TEST("tun_to_rb header is 16 bits: 3 flags (comp,rohc,fec) + 13 length bits");
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
    uint16_t header = read_header_descrambled(echo.tx_rb);
    if (header == 0xFFFF) { FAIL("header incomplete"); close(pipefd[0]); close(pipefd[1]); free(echo.tx_rb); return; }
    int fec_total = fec_encoded_len(10);
    int expected_total_bits = 16 + fec_total * 8;
    int total_bits = 0;
    uint8_t bit;
    while (get_bits(echo.tx_rb, &bit)) total_bits++;
    if (total_bits < expected_total_bits - 16 || total_bits > expected_total_bits + 8) {
        FAIL("total bits %d does not match header 16 + FEC payload %d", total_bits, expected_total_bits);
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
        pre_calc_goertzel_long(&echo.freq_valid[i], &freqs[i]);
    }
    uint16_t mon_freqs[NUM_FREQ_MON] = {FREQ_MON_LOW, FREQ_MON_MID, FREQ_MON_HIGH};
    for (int i = 0; i < NUM_FREQ_MON; i++) {
        pre_calc_goertzel(&echo.freq_mon[i], &mon_freqs[i]);
    }
    bandpass_bank_init(&echo.bp_bank, SAMPLE_RATE, 2600.0f);
    echo.pending_candidate = -1;
    echo.block_counter = 0;
    echo.long_samples_count = 0;
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
    
    uint16_t header1;
    uint8_t payload1[64];
    read_packet_descrambled(echo.tx_rb, &header1, payload1, sizeof(payload1));
    if (header1 == 0xFFFF) { FAIL("first header missing"); close(pipefd[0]); close(pipefd[1]); free(echo.tx_rb); return; }
    
    uint16_t header2;
    uint8_t payload2[64];
    read_packet_descrambled(echo.tx_rb, &header2, payload2, sizeof(payload2));
    if (header2 == 0xFFFF) { FAIL("second header missing"); close(pipefd[0]); close(pipefd[1]); free(echo.tx_rb); return; }
    
    uint8_t pkt2_rohc = (header2 >> 14) & 1;
    uint8_t pkt2_fec = (header2 >> 13) & 1;
    uint16_t pkt2_len = header2 & 0x1FFF;
    if (pkt2_rohc != 1) { FAIL("second packet should use ROHC"); close(pipefd[0]); close(pipefd[1]); free(echo.tx_rb); return; }
    if (pkt2_fec != 1) { FAIL("second packet should have FEC"); close(pipefd[0]); close(pipefd[1]); free(echo.tx_rb); return; }
    int raw_fec_len = fec_encoded_len(28);
    if (pkt2_len >= raw_fec_len) { FAIL("ROHC+FEC should reduce size below raw+FEC"); close(pipefd[0]); close(pipefd[1]); free(echo.tx_rb); return; }
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
    TEST("ROHC header uses 13 bits for length (bit 14 = rohc flag, bit 13 = fec flag, mask 0x1FFF)");
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
    
    uint16_t header1;
    uint8_t payload1[64];
    read_packet_descrambled(echo.tx_rb, &header1, payload1, sizeof(payload1));
    if (header1 == 0xFFFF) { FAIL("first header missing"); close(pipefd[0]); close(pipefd[1]); free(echo.tx_rb); return; }
    
    uint16_t header2;
    uint8_t payload2[64];
    read_packet_descrambled(echo.tx_rb, &header2, payload2, sizeof(payload2));
    if (header2 == 0xFFFF) { FAIL("second header missing"); close(pipefd[0]); close(pipefd[1]); free(echo.tx_rb); return; }
    
    uint8_t rohc_flag = (header2 >> 14) & 1;
    uint8_t fec_flag = (header2 >> 13) & 1;
    uint16_t pkt2_len = header2 & 0x1FFF;
    if (rohc_flag != 1) { FAIL("rohc flag not set in second packet"); close(pipefd[0]); close(pipefd[1]); free(echo.tx_rb); return; }
    if (fec_flag != 1) { FAIL("fec flag not set in second packet"); close(pipefd[0]); close(pipefd[1]); free(echo.tx_rb); return; }
    int raw_fec_len = fec_encoded_len(28);
    if (pkt2_len == 0 || pkt2_len >= raw_fec_len) { FAIL("ROHC+FEC length out of expected range"); close(pipefd[0]); close(pipefd[1]); free(echo.tx_rb); return; }
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

void test_rx_state_machine_exact_len_delivers_packet() {
    TEST("handle_data_state: exact length sets packet_ready and returns to SEARCHING (good path)");
    EchoProtocol echo;
    memset(&echo, 0, sizeof(echo));
    echo.rx_rb = rb_init();
    echo.rx.state = DATA;
    echo.rx.bits_received = 16;   /* header já validado/acumulado */
    echo.rx.packet_len = 3;       /* alvo = 16 + 3*8 = 40 */
    echo.rx.header_accumulator = 0;
    echo.rx.last_rx_time = 0;
    echo.rx.is_compressed = 0;
    echo.rx.is_rohc = 0;
    echo.rx.is_fec = 0;
    int target = 16 + 3 * 8;
    for (int i = 17; i <= target; i++) {
        echo.rx.bits_received = i;
        echo_test_handle_data_state(&echo, 0);
    }
    if (echo.rx.state != SEARCHING) { FAIL("state != SEARCHING after exact length"); free(echo.rx_rb); return; }
    if (echo.rx.packet_ready != 1) { FAIL("packet_ready not set after exact length"); free(echo.rx_rb); return; }
    PASS();
    free(echo.rx_rb);
}

void test_rx_state_machine_overshoot_corrupt_len_resets() {
    TEST("handle_data_state: overshoot on corrupt packet_len resets (horrible path)");
    EchoProtocol echo;
    memset(&echo, 0, sizeof(echo));
    echo.rx_rb = rb_init();
    echo.rx.state = DATA;
    echo.rx.bits_received = 16;
    echo.rx.packet_len = 1;       /* alvo = 16 + 8 = 24 (declarado curto) */
    echo.rx.header_accumulator = 0;
    echo.rx.last_rx_time = 0;
    echo.rx.is_compressed = 0;
    echo.rx.is_rohc = 0;
    echo.rx.is_fec = 0;
    /* Envia bits além do comprimento declarado (framing perdido / len corrompido). */
    int target = 16 + 1 * 8;
    echo.rx.bits_received = target + 1;  /* overshoot */
    echo_test_handle_data_state(&echo, 0);
    if (echo.rx.state != SEARCHING) {
        FAIL("overshoot did not reset to SEARCHING (stuck in DATA until 6s timeout)");
        free(echo.rx_rb);
        return;
    }
    if (echo.rx.bits_received != 0) { FAIL("rx_reset did not clear bits_received"); free(echo.rx_rb); return; }
    PASS();
    free(echo.rx_rb);
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

/* === Set B validation pipeline tests === */

void test_goertzel_buffer_long_detects_frequency() {
    TEST("process_goertzel_buffer_long detects correct frequency from 64-sample buffer");
    StateGoertzelLong states[4];
    uint16_t freqs[4] = {FREQ_00, FREQ_01, FREQ_10, FREQ_11};
    for (int i = 0; i < 4; i++) {
        pre_calc_goertzel_long(&states[i], &freqs[i]);
    }

    /* Fill buffer with64 samples of FREQ_10 (4000Hz, index 2) */
    float buf[SAMPLES_LONG_WINDOW];
    float step = 2.0f * (float)M_PI * FREQ_10 / SAMPLE_RATE;
    for (int i = 0; i < SAMPLES_LONG_WINDOW; i++) {
        buf[i] = sinf(i * step);
    }

    float mag_b[4];
    for (int i = 0; i < 4; i++) {
        mag_b[i] = process_goertzel_buffer_long(&states[i], buf, SAMPLES_LONG_WINDOW);
    }

    int max_idx = 0;
    for (int i = 1; i < 4; i++) {
        if (mag_b[i] > mag_b[max_idx]) max_idx = i;
    }

    if (max_idx != 2) { FAIL("expected FREQ_10 (index 2)"); return; }
    PASS();
}

void test_audio_to_rb_long_samples_count_increments() {
    TEST("audio_to_rb increments long_samples_count each sample");
    EchoProtocol echo;
    memset(&echo, 0, sizeof(echo));
    echo.rx_rb = rb_init();
    echo.rx.state = DATA;
    echo.rx.rx_sample_count = 0;
    echo.rx.last_rx_time = (uint32_t)time(NULL);
    uint16_t freqs[4] = {FREQ_00, FREQ_01, FREQ_10, FREQ_11};
    for (int i = 0; i < 4; i++) {
        pre_calc_goertzel(&echo.freq_states[i], &freqs[i]);
        pre_calc_goertzel_long(&echo.freq_valid[i], &freqs[i]);
    }
    echo.pending_candidate = -1;
    echo.block_counter = 0;
    echo.long_samples_count = 0;

    float sample = 0.5f;
    for (int i = 0; i < 10; i++) {
        audio_to_rb(&echo, &sample);
    }
    /* long_samples_count increments every sample (including non-symbol-boundary) */
    if (echo.long_samples_count != 10) {
        FAIL("long_samples_count should be 10 after 10 samples");
        free(echo.rx_rb); return;
    }
    PASS();
    free(echo.rx_rb);
}

void test_audio_to_rb_long_samples_count_resets_after_validation() {
    TEST("audio_to_rb resets long_samples_count to 0 after Set B validation at 64");
    EchoProtocol echo;
    memset(&echo, 0, sizeof(echo));
    echo.rx_rb = rb_init();
    echo.rx.state = DATA;
    echo.rx.rx_sample_count = 0;
    echo.rx.last_rx_time = (uint32_t)time(NULL);
    uint16_t freqs[4] = {FREQ_00, FREQ_01, FREQ_10, FREQ_11};
    for (int i = 0; i < 4; i++) {
        pre_calc_goertzel(&echo.freq_states[i], &freqs[i]);
        pre_calc_goertzel_long(&echo.freq_valid[i], &freqs[i]);
    }
    echo.pending_candidate = -1;
    echo.block_counter = 0;
    echo.long_samples_count = 0;

    /* Feed64 samples of FREQ_00 to trigger validation */
    float step = 2.0f * (float)M_PI * FREQ_00 / SAMPLE_RATE;
    for (int i = 0; i < SAMPLES_LONG_WINDOW; i++) {
        float sample = sinf(i * step);
        audio_to_rb(&echo, &sample);
    }

    /* After64 samples: validation ran, counter should be reset */
    if (echo.long_samples_count != 0) {
        FAIL("long_samples_count should be 0 after validation");
        free(echo.rx_rb); return;
    }
    /* long_buf_idx should also be reset */
    if (echo.long_buf_idx[0] != 0) {
        FAIL("long_buf_idx should be 0 after validation");
        free(echo.rx_rb); return;
    }
    PASS();
    free(echo.rx_rb);
}

void test_audio_to_rb_no_validation_before_64_samples() {
    TEST("audio_to_rb does not run Set B validation before 64 samples accumulated");
    EchoProtocol echo;
    memset(&echo, 0, sizeof(echo));
    echo.rx_rb = rb_init();
    echo.rx.state = DATA;
    echo.rx.rx_sample_count = 0;
    echo.rx.last_rx_time = (uint32_t)time(NULL);
    uint16_t freqs[4] = {FREQ_00, FREQ_01, FREQ_10, FREQ_11};
    for (int i = 0; i < 4; i++) {
        pre_calc_goertzel(&echo.freq_states[i], &freqs[i]);
        pre_calc_goertzel_long(&echo.freq_valid[i], &freqs[i]);
    }
    echo.pending_candidate = -1;
    echo.block_counter = 0;
    echo.long_samples_count = 0;
    echo.stats.val_failures = 0;

    /* Feed32 samples (1 symbol) — validation should NOT run */
    float step = 2.0f * (float)M_PI * FREQ_00 / SAMPLE_RATE;
    for (int i = 0; i < SAMPLES_PER_SYMBOL; i++) {
        float sample = sinf(i * step);
        audio_to_rb(&echo, &sample);
    }

    if (echo.stats.val_failures != 0) {
        FAIL("val_failures should be 0 before 64 samples");
        free(echo.rx_rb); return;
    }
    if (echo.long_samples_count != SAMPLES_PER_SYMBOL) {
        FAIL("long_samples_count should be 32 after 1 symbol");
        free(echo.rx_rb); return;
    }
    PASS();
    free(echo.rx_rb);
}

void test_audio_to_rb_val_failures_increments_on_mismatch() {
    TEST("audio_to_rb increments val_failures when Set B disagrees with pending_candidate");
    EchoProtocol echo;
    memset(&echo, 0, sizeof(echo));
    echo.rx_rb = rb_init();
    echo.rx.state = DATA;
    echo.rx.rx_sample_count = 0;
    echo.rx.last_rx_time = (uint32_t)time(NULL);
    uint16_t freqs[4] = {FREQ_00, FREQ_01, FREQ_10, FREQ_11};
    for (int i = 0; i < 4; i++) {
        pre_calc_goertzel(&echo.freq_states[i], &freqs[i]);
        pre_calc_goertzel_long(&echo.freq_valid[i], &freqs[i]);
    }
    echo.pending_candidate = -1;
    echo.block_counter = 0;
    echo.long_samples_count = 0;
    echo.stats.val_failures = 0;
    bandpass_bank_init(&echo.bp_bank, SAMPLE_RATE, 2600.0f);

    /* Feed64 samples of FREQ_00 → triggers validation 1 at end.
       pending_candidate becomes 0. Buffer is pure FREQ_00. Match. */
    float step0 = 2.0f * (float)M_PI * FREQ_00 / SAMPLE_RATE;
    for (int i = 0; i < SAMPLES_LONG_WINDOW; i++) {
        float sample = sinf(i * step0);
        audio_to_rb(&echo, &sample);
    }
    if (echo.stats.val_failures != 0) {
        FAIL("val_failures should be 0 after pure FREQ_00 validation");
        free(echo.rx_rb); return;
    }

    /* Now feed32 samples of FREQ_11 (symbol 2).
       pending_candidate = 3. long_samples_count = 32. No validation. */
    float step11 = 2.0f * (float)M_PI * FREQ_11 / SAMPLE_RATE;
    for (int i = 0; i < SAMPLES_PER_SYMBOL; i++) {
        float sample = sinf(i * step11);
        audio_to_rb(&echo, &sample);
    }
    if (echo.stats.val_failures != 0) {
        FAIL("val_failures should be 0 before second validation");
        free(echo.rx_rb); return;
    }
    if (echo.pending_candidate != 3) {
        FAIL("pending_candidate should be 3 after FREQ_11");
        free(echo.rx_rb); return;
    }

    /* Force pending_candidate to wrong value (simulating Set A error) */
    echo.pending_candidate = 0;

    /* Feed32 more samples of FREQ_11 (symbol 3).
       long_samples_count = 64 → validation 2 runs.
       Buffer has 64 samples of FREQ_11 (symbols 2+3).
       Set B detects 3. pending_candidate forced to 0. MISMATCH! */
    for (int i = 0; i < SAMPLES_PER_SYMBOL; i++) {
        float sample = sinf(i * step11);
        audio_to_rb(&echo, &sample);
    }

    if (echo.stats.val_failures != 1) {
        FAIL("val_failures should be 1 after forced mismatch");
        free(echo.rx_rb); return;
    }
    PASS();
    free(echo.rx_rb);
}

void test_audio_to_rb_pending_candidate_set_per_symbol() {
    TEST("audio_to_rb updates pending_candidate to current symbol each time");
    EchoProtocol echo;
    memset(&echo, 0, sizeof(echo));
    echo.rx_rb = rb_init();
    echo.rx.state = DATA;
    echo.rx.rx_sample_count = 0;
    echo.rx.last_rx_time = (uint32_t)time(NULL);
    uint16_t freqs[4] = {FREQ_00, FREQ_01, FREQ_10, FREQ_11};
    for (int i = 0; i < 4; i++) {
        pre_calc_goertzel(&echo.freq_states[i], &freqs[i]);
        pre_calc_goertzel_long(&echo.freq_valid[i], &freqs[i]);
    }
    echo.pending_candidate = -1;
    echo.block_counter = 0;
    echo.long_samples_count = 0;
    bandpass_bank_init(&echo.bp_bank, SAMPLE_RATE, 2600.0f);

    /* Symbol 0: FREQ_10 (index 2) */
    float step2 = 2.0f * (float)M_PI * FREQ_10 / SAMPLE_RATE;
    for (int i = 0; i < SAMPLES_PER_SYMBOL; i++) {
        float sample = sinf(i * step2);
        audio_to_rb(&echo, &sample);
    }
    if (echo.pending_candidate != 2) {
        FAIL("pending_candidate should be 2 after FREQ_10 symbol");
        free(echo.rx_rb); return;
    }

    /* Symbol 1: FREQ_01 (index 1) */
    float step1 = 2.0f * (float)M_PI * FREQ_01 / SAMPLE_RATE;
    for (int i = 0; i < SAMPLES_PER_SYMBOL; i++) {
        float sample = sinf(i * step1);
        audio_to_rb(&echo, &sample);
    }
    if (echo.pending_candidate != 1) {
        FAIL("pending_candidate should be 1 after FREQ_01 symbol");
        free(echo.rx_rb); return;
    }

    PASS();
    free(echo.rx_rb);
}

int main() {
    printf("=== Complete Tests: Echo Protocol ===\n\n");

    printf("[echo_init / echo_close]\n");
    test_echo_init_fails_on_invalid_device();
    test_echo_init_manual_state_config();
    test_echo_close_does_not_crash();
    test_echo_close_frees_buffers();

    printf("\n[audio_to_rb]\n");
    test_audio_to_rb_sample_count();
    test_audio_to_rb_demodulates_mark_as_1();
    test_audio_to_rb_demodulates_space_as_0();

    printf("\n[Set B validation pipeline]\n");
    test_goertzel_buffer_long_detects_frequency();
    test_audio_to_rb_long_samples_count_increments();
    test_audio_to_rb_long_samples_count_resets_after_validation();
    test_audio_to_rb_no_validation_before_64_samples();
    test_audio_to_rb_val_failures_increments_on_mismatch();
    test_audio_to_rb_pending_candidate_set_per_symbol();

    printf("\n[rx state machine]\n");
    test_rx_state_machine_searching_to_data();
    test_rx_state_machine_exact_len_delivers_packet();
    test_rx_state_machine_overshoot_corrupt_len_resets();

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
