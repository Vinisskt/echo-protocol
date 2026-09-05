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
#include "../include/scrambler.h"
#include "../include/crc16.h"

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
    /* consome o trailer CRC16 (16 bits) para alinhar leituras sequenciais de frames */
    for (int i = 0; i < 16; i++) {
        if (!get_bits(tx_rb, &bit)) break;
        scrambler_process(&s, bit);
    }
}

/* Helper: empurra bytes no rb, bit MSB primeiro (mesma ordem do TX). */
static void push_bytes_bits(Buffer *rb, const uint8_t *bytes, int len) {
    for (int i = 0; i < len; i++) {
        for (int b = 7; b >= 0; b--) {
            uint8_t bit = (bytes[i] >> b) & 1;
            put_bits(rb, &bit);
        }
    }
}

/* Helper: empurra header(16b) + payload(N bytes) + crc16(16b, end-to-end)
 * no rx_rb — formato exato do frame no ar. */
static void put_frame_bits(Buffer *rb, uint16_t header, const uint8_t *payload, int payload_len) {
    uint8_t hdr[2] = { (uint8_t)(header >> 8), (uint8_t)(header & 0xFF) };
    push_bytes_bits(rb, hdr, 2);
    push_bytes_bits(rb, payload, payload_len);
    uint16_t crc = frame_crc(header, payload, payload_len);
    uint8_t crc_b[2] = { (uint8_t)(crc >> 8), (uint8_t)(crc & 0xFF) };
    push_bytes_bits(rb, crc_b, 2);
}

/* Helper: cria um RX EchoProtocol com pipe não-bloqueante para a TUN. */
static void rx_echo_init(EchoProtocol *echo, int *pipefd, int payload_capacity) {
    if (pipe(pipefd) == -1) { printf("FAIL: pipe failed\n"); exit(1); }
    fcntl(pipefd[0], F_SETFL, O_NONBLOCK);
    memset(echo, 0, sizeof(*echo));
    echo->tun_fd = pipefd[1];
    echo->rx_rb = rb_init();
    echo->rx.packet_len = (uint16_t)payload_capacity;
    echo->rx.is_compressed = 0;
    echo->rx.is_rohc = 0;
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
    bandpass_bank_init(&echo.bp_bank, SAMPLE_RATE, 2000.0f);
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
    bandpass_bank_init(&echo.bp_bank, SAMPLE_RATE, 2000.0f);
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
    bandpass_bank_init(&echo.bp_bank, SAMPLE_RATE, 2000.0f);
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
    if (comp_flag != 0 || rohc_flag != 0 || fec_flag != 0 || packet_len != 16) {
        FAIL("wrong header (expected no flags, len=16), got fec=%d len=%d", fec_flag, packet_len);
        close(pipefd[0]); close(pipefd[1]); free(echo.tx_rb);
        return;
    }
    if (memcmp(payload, packet, 16) != 0) { FAIL("payload corrupted"); close(pipefd[0]); close(pipefd[1]); free(echo.tx_rb); return; }
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

    if (comp_flag != 1 || rohc_flag != 0 || fec_flag != 0 || packet_len != lz4_size) {
        FAIL("wrong flags for LZ4: comp=%d rohc=%d fec=%d len=%d expected=%d (lz4_size=%d)", comp_flag, rohc_flag, fec_flag, packet_len, lz4_size, lz4_size);
        close(pipefd[0]); close(pipefd[1]); free(echo.tx_rb);
        return;
    }
    PASS();
    close(pipefd[0]); close(pipefd[1]); free(echo.tx_rb);
}

void test_tun_to_rb_header_16bit_structure() {
    TEST("tun_to_rb header is 16 bits: 2 flags (comp,rohc) + 14 length bits");
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
    /* o header já foi consumido; restam 80 (payload) + 16 (crc16) = 96 bits */
    int expected_total_bits = 10 * 8 + 16;
    int total_bits = 0;
    uint8_t bit;
    while (get_bits(echo.tx_rb, &bit)) total_bits++;
    if (total_bits != expected_total_bits) {
        FAIL("total bits %d != payload 80 + crc16 16 = %d", total_bits, expected_total_bits);
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
    put_frame_bits(echo.rx_rb, header, original, pkt_len);
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
    put_frame_bits(echo.rx_rb, header, comp_buf, comp_size);
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
    bandpass_bank_init(&echo.bp_bank, SAMPLE_RATE, 2000.0f);
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
    if (pkt2_fec != 0) { FAIL("FEC should be disabled"); close(pipefd[0]); close(pipefd[1]); free(echo.tx_rb); return; }
    if (pkt2_len >= 28) { FAIL("ROHC should reduce size below raw 28 bytes"); close(pipefd[0]); close(pipefd[1]); free(echo.tx_rb); return; }
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
    put_frame_bits(echo.rx_rb, header, compressed, comp_size);
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
    TEST("ROHC header uses 14 bits for length (bit 15 = comp, bit 14 = rohc, mask 0x1FFF)");
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
    if (fec_flag != 0) { FAIL("fec flag should never be set"); close(pipefd[0]); close(pipefd[1]); free(echo.tx_rb); return; }
    if (pkt2_len == 0 || pkt2_len >= 28) { FAIL("ROHC length out of expected range (< 28 raw)"); close(pipefd[0]); close(pipefd[1]); free(echo.tx_rb); return; }
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
    /* frame = 16 (header) + 3*8 (payload) + 16 (crc16) = 56 bits */
    int target = 16 + 3 * 8 + 16;
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
    echo.rx.packet_len = 1;       /* alvo = 16 + 8 + 16 = 40 (declarado curto) */
    echo.rx.header_accumulator = 0;
    echo.rx.last_rx_time = 0;
    echo.rx.is_compressed = 0;
    echo.rx.is_rohc = 0;
    /* Envia bits além do comprimento declarado (framing perdido / len corrompido). */
    int target = 16 + 1 * 8 + 16;
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
        put_frame_bits(echo.rx_rb, header, packet, pkt_len);
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
    bandpass_bank_init(&echo.bp_bank, SAMPLE_RATE, 2000.0f);

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
    bandpass_bank_init(&echo.bp_bank, SAMPLE_RATE, 2000.0f);

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

/* === Frame CRC16 end-to-end === */

void test_frame_crc_tx_rx_roundtrip() {
    TEST("tun_to_rb a rb_to_tun: roundtrip completo com CRC16 (frame de 160 bits exatos)");
    int in_pipe[2], out_pipe[2];
    if (pipe(in_pipe) == -1) { FAIL("in pipe failed"); return; }
    if (pipe(out_pipe) == -1) { FAIL("out pipe failed"); close(in_pipe[0]); close(in_pipe[1]); return; }
    fcntl(out_pipe[0], F_SETFL, O_NONBLOCK);

    EchoProtocol tx;
    memset(&tx, 0, sizeof(tx));
    tx.tun_fd = in_pipe[0];
    tx.tx_rb = rb_init();
    pre_calc_fsk(&tx.mod_state);

    uint8_t packet[16];
    packet[0] = 0xDE; packet[1] = 0xAD; packet[2] = 0xBE; packet[3] = 0xEF;
    for (int i = 4; i < 16; i++) packet[i] = (uint8_t)((i * 37 + 11) & 0xFF);
    if (write(in_pipe[1], packet, sizeof(packet)) != (ssize_t)sizeof(packet)) { FAIL("write"); return; }
    tun_to_rb(&tx);

    /* extrai os bits descrambrados EXATOS (header+payload+crc) que o TX gerou */
    Scrambler s;
    scrambler_init(&s);
    uint8_t bit;
    uint8_t frame_bytes[32];
    memset(frame_bytes, 0, sizeof(frame_bytes));
    int total_bits = 0;
    while (get_bits(tx.tx_rb, &bit)) {
        bit = scrambler_process(&s, bit);
        frame_bytes[total_bits >> 3] = (frame_bytes[total_bits >> 3] << 1) | bit;
        total_bits++;
    }
    free(tx.tx_rb);
    close(in_pipe[0]); close(in_pipe[1]);
    if (total_bits != 160) { FAIL("total bits %d != 160 (16 hdr + 128 payload + 16 crc)", total_bits); return; }

    uint16_t header = (uint16_t)((frame_bytes[0] << 8) | frame_bytes[1]);
    uint16_t len = header & 0x1FFF;
    if (len != 16) { FAIL("len=%d != 16", len); return; }

    /* RX: alimenta exatamente o que o TX transmitiu (inclui o crc do TX) */
    EchoProtocol rx;
    memset(&rx, 0, sizeof(rx));
    rx.tun_fd = out_pipe[1];
    rx.rx_rb = rb_init();
    rx.rx.is_compressed = 0;
    rx.rx.is_rohc = 0;
    push_bytes_bits(rx.rx_rb, frame_bytes, total_bits / 8);
    int pkt_len = 16;
    rb_to_tun(&rx, &pkt_len);
    uint8_t result[64];
    ssize_t n = read(out_pipe[0], result, sizeof(result));
    if (n != (ssize_t)sizeof(packet)) { FAIL("entregou %zd bytes, esperado 16", n); free(rx.rx_rb); close(out_pipe[0]); close(out_pipe[1]); return; }
    if (memcmp(result, packet, sizeof(packet)) != 0) { FAIL("conteudo corrompido no roundtrip"); free(rx.rx_rb); close(out_pipe[0]); close(out_pipe[1]); return; }
    PASS();
    free(rx.rx_rb);
    close(out_pipe[0]); close(out_pipe[1]);
}

void test_frame_crc_rejects_every_single_bit_error() {
    TEST("frame CRC: TODA troca de 1 bit (hdr+payload+crc) e rejeitada");
    uint8_t payload[16];
    for (int i = 0; i < 16; i++) payload[i] = (uint8_t)((i * 13 + 7) & 0xFF);
    uint16_t header = 0x0010;  /* len=16, sem flags */
    uint8_t frame_bytes[32];
    frame_bytes[0] = (uint8_t)(header >> 8);
    frame_bytes[1] = (uint8_t)(header & 0xFF);
    memcpy(frame_bytes + 2, payload, 16);
    uint16_t crc = frame_crc(header, payload, 16);
    frame_bytes[18] = (uint8_t)(crc >> 8);
    frame_bytes[19] = (uint8_t)(crc & 0xFF);
    int fb_len = 20;

    /* controle: frame integro entrega os 16 bytes */
    int ctl[2];
    EchoProtocol c;
    rx_echo_init(&c, ctl, 16);
    push_bytes_bits(c.rx_rb, frame_bytes, fb_len);
    int cl = 16;
    rb_to_tun(&c, &cl);
    uint8_t cbuf[64];
    ssize_t cn = read(ctl[0], cbuf, sizeof(cbuf));
    if (cn != 16) { FAIL("controle nao entregou (n=%zd)", cn); free(c.rx_rb); close(ctl[0]); close(ctl[1]); return; }
    free(c.rx_rb); close(ctl[0]); close(ctl[1]);

    /* exaustivo: flip de cada um dos 160 bits */
    int rejected = 0, detected = 0;
    for (int bitpos = 0; bitpos < fb_len * 8; bitpos++) {
        uint8_t fb[32];
        memcpy(fb, frame_bytes, fb_len);
        fb[bitpos >> 3] ^= (uint8_t)(1u << (7 - (bitpos & 7)));
        int pipefd[2];
        EchoProtocol echo;
        rx_echo_init(&echo, pipefd, 16);
        push_bytes_bits(echo.rx_rb, fb, fb_len);
        uint64_t before = echo.stats.rx_corrupted;
        int l = 16;
        rb_to_tun(&echo, &l);
        uint8_t r[64];
        ssize_t n = read(pipefd[0], r, sizeof(r));
        if (n != -1) {
            FAIL("bit %d entregou %zd bytes — corrupcao passou!", bitpos, n);
            free(echo.rx_rb); close(pipefd[0]); close(pipefd[1]);
            return;
        }
        rejected++;
        if (echo.stats.rx_corrupted > before) detected++;
        free(echo.rx_rb);
        close(pipefd[0]); close(pipefd[1]);
    }
    printf("(rejeitadas %d/%d, contabilizadas como rx_corrupted %d) ", rejected, fb_len * 8, detected);
    PASS();
}

void test_frame_crc_rohc_payload_corruption_rejected() {
    TEST("ROHC: corrupcao em qualquer bit do payload comprimido e detectada (BUG silencioso)");
    uint8_t original[28] = {0x45,0x00,0x00,0x1C,0x00,0x01,0x40,0x00,
                            0x40,0x06,0x00,0x00,0x0A,0x00,0x00,0x01,
                            0x0A,0x00,0x00,0x02, 0x08,0x00,0xF7,0xFF,
                            0x00,0x01,0x00,0x01};
    original[10] = 0; original[11] = 0;
    uint16_t ck = ip_checksum(original, 20);
    original[10] = (uint8_t)(ck >> 8);
    original[11] = (uint8_t)(ck & 0xFF);

    ROHCState send;
    rohc_init(&send);
    rohc_compress(&send, original, sizeof(original), NULL, 0);
    uint8_t compressed[16];
    int comp_size = rohc_compress(&send, original, sizeof(original), compressed, sizeof(compressed));
    if (comp_size <= 0) { FAIL("rohc compress falhou (%d)", comp_size); return; }
    uint16_t header = (0 << 15) | (1 << 14) | (comp_size & 0x3FFF);

    /* controle: frame integro descomprime e entrega o original */
    int ctl[2];
    EchoProtocol ctx;
    rx_echo_init(&ctx, ctl, comp_size);
    rohc_init(&ctx.rohc_rx);
    memcpy(ctx.rohc_rx.context, original, 20);
    ctx.rohc_rx.context_valid = 1;
    put_frame_bits(ctx.rx_rb, header, compressed, comp_size);
    ctx.rx.is_rohc = 1;
    int cl = comp_size;
    rb_to_tun(&ctx, &cl);
    uint8_t cbuf[64];
    ssize_t cn = read(ctl[0], cbuf, sizeof(cbuf));
    if (cn != 28 || memcmp(cbuf, original, 28) != 0) { FAIL("controle ROHC falhou (n=%zd)", cn); free(ctx.rx_rb); close(ctl[0]); close(ctl[1]); return; }
    free(ctx.rx_rb); close(ctl[0]); close(ctl[1]);

    uint8_t fb[32];
    fb[0] = (uint8_t)(header >> 8);
    fb[1] = (uint8_t)(header & 0xFF);
    memcpy(fb + 2, compressed, comp_size);
    uint16_t crc = frame_crc(header, compressed, comp_size);
    fb[2 + comp_size] = (uint8_t)(crc >> 8);
    fb[2 + comp_size + 1] = (uint8_t)(crc & 0xFF);
    int fb_len = 2 + comp_size + 2;

    int rejected = 0, detected = 0;
    for (int bitpos = 0; bitpos < fb_len * 8; bitpos++) {
        uint8_t fb2[32];
        memcpy(fb2, fb, fb_len);
        fb2[bitpos >> 3] ^= (uint8_t)(1u << (7 - (bitpos & 7)));
        int pipefd[2];
        EchoProtocol echo;
        rx_echo_init(&echo, pipefd, comp_size);
        rohc_init(&echo.rohc_rx);
        memcpy(echo.rohc_rx.context, original, 20);
        echo.rohc_rx.context_valid = 1;
        push_bytes_bits(echo.rx_rb, fb2, fb_len);
        echo.rx.is_rohc = 1;
        uint64_t before = echo.stats.rx_corrupted;
        int l = comp_size;
        rb_to_tun(&echo, &l);
        uint8_t r[64];
        ssize_t n = read(pipefd[0], r, sizeof(r));
        if (n != -1) {
            FAIL("ROHC bit %d entregou %zd bytes — payload corrompido passou silencioso!", bitpos, n);
            free(echo.rx_rb); close(pipefd[0]); close(pipefd[1]);
            return;
        }
        rejected++;
        if (echo.stats.rx_corrupted > before) detected++;
        free(echo.rx_rb);
        close(pipefd[0]); close(pipefd[1]);
    }
    printf("(rejeitadas %d/%d, rx_corrupted %d) ", rejected, fb_len * 8, detected);
    PASS();
}

void test_frame_crc_garbage_frame_no_crash() {
    TEST("frame CRC: 30 frames de lixo (header/payload/crc aleatorios) nunca crasham nem entregam");
    srand(99);
    for (int iter = 0; iter < 30; iter++) {
        int len = rand() % 18;
        int pipefd[2];
        EchoProtocol echo;
        rx_echo_init(&echo, pipefd, len);
        uint16_t h = (uint16_t)(rand() & 0xFFFF);
        uint8_t hdr[2] = { (uint8_t)(h >> 8), (uint8_t)(h & 0xFF) };
        push_bytes_bits(echo.rx_rb, hdr, 2);
        uint8_t payload[18];
        for (int i = 0; i < len; i++) payload[i] = (uint8_t)(rand() & 0xFF);
        push_bytes_bits(echo.rx_rb, payload, len);
        uint8_t junk_crc[2];
        for (int i = 0; i < 2; i++) junk_crc[i] = (uint8_t)(rand() & 0xFF);
        push_bytes_bits(echo.rx_rb, junk_crc, 2);
        int l = len;
        rb_to_tun(&echo, &l);
        uint8_t r[64];
        ssize_t n = read(pipefd[0], r, sizeof(r));
        if (n != -1) {
            FAIL("iter %d: frame de lixo entregou %zd bytes", iter, n);
            free(echo.rx_rb); close(pipefd[0]); close(pipefd[1]);
            return;
        }
        free(echo.rx_rb);
        close(pipefd[0]); close(pipefd[1]);
    }
    PASS();
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

    printf("\n[frame CRC16 end-to-end]\n");
    test_frame_crc_tx_rx_roundtrip();
    test_frame_crc_rejects_every_single_bit_error();
    test_frame_crc_rohc_payload_corruption_rejected();
    test_frame_crc_garbage_frame_no_crash();

    printf("\n=== Summary: %d PASS, %d FAIL ===\n", tests_passed, tests_failed);
    return tests_failed > 0 ? 1 : 0;
}
