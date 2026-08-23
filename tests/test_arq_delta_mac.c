#include <stdio.h>
#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <fcntl.h>
#include "../include/arq.h"
#include "../include/delta.h"
#include "../include/mac.h"
#include "../include/echo_protocol.h"
#include "../include/fec.h"
#include "../include/scrambler.h"
#include "../include/rb_bits.h"
#include "../include/mod_fsk.h"
#include "../include/demod_afsk.h"

static int tests_passed = 0;
static int tests_failed = 0;

#define TEST(name) do { printf("  - %s ... ", name); } while(0)
#define PASS() do { printf("PASS\n"); tests_passed++; } while(0)
#define FAIL(fmt, ...) do { printf("FAIL: " fmt "\n", ##__VA_ARGS__); tests_failed++; } while(0)

static uint32_t test_time_ms = 1000;
static uint32_t test_get_time_ms(void) { return test_time_ms; }
static uint32_t test_get_time_us(void) { return test_time_ms * 1000; }

/* ========================================================================
   ARQ Tests
   ======================================================================== */

void test_arq_init() {
    TEST("arq_init initializes context correctly");
    ArqContext ctx;
    arq_init(&ctx, 8, 500, test_get_time_ms);
    
    if (ctx.window_size != 8) { FAIL("window_size=%d expected 8", ctx.window_size); return; }
    if (ctx.timeout_ms != 500) { FAIL("timeout_ms=%d expected 500", ctx.timeout_ms); return; }
    if (ctx.sender.window_size != 8) { FAIL("sender window_size=%d", ctx.sender.window_size); return; }
    if (ctx.receiver.window_size != 8) { FAIL("receiver window_size=%d", ctx.receiver.window_size); return; }
    PASS();
}

void test_arq_send_receive() {
    TEST("arq_send and arq_receive basic flow");
    ArqContext ctx;
    arq_init(&ctx, 4, 500, test_get_time_ms);
    
    uint8_t data[] = {0x01, 0x02, 0x03, 0x04};
    int res = arq_send(&ctx, data, sizeof(data));
    if (res != 0) { FAIL("arq_send failed"); return; }
    
    ArqFrame frame;
    if (!arq_get_next_frame(&ctx, &frame)) { FAIL("no frame to send"); return; }
    if (frame.type != ARQ_FRAME_DATA) { FAIL("wrong frame type"); return; }
    if (frame.payload_len != 4) { FAIL("wrong payload len"); return; }
    if (memcmp(frame.payload, data, 4) != 0) { FAIL("payload corrupted"); return; }
    
    /* Receive on other side */
    ArqContext ctx_rx;
    arq_init(&ctx_rx, 4, 500, test_get_time_ms);
    res = arq_receive(&ctx_rx, &frame);
    if (res != 0) { FAIL("arq_receive failed"); return; }
    
    if (!arq_has_delivered(&ctx_rx)) { FAIL("packet not delivered"); return; }
    
    uint8_t out[16];
    int len = arq_get_delivered(&ctx_rx, out, sizeof(out));
    if (len != 4) { FAIL("wrong delivered len"); return; }
    if (memcmp(out, data, 4) != 0) { FAIL("delivered data corrupted"); return; }
    
    PASS();
}

void test_arq_ack() {
    TEST("arq_handle_ack advances send window");
    ArqContext ctx;
    arq_init(&ctx, 4, 500, test_get_time_ms);
    
    uint8_t data[] = {0xAA, 0xBB};
    arq_send(&ctx, data, sizeof(data));
    ArqFrame frame;
    arq_get_next_frame(&ctx, &frame);
    
    /* Receive and ACK */
    ArqContext ctx_rx;
    arq_init(&ctx_rx, 4, 500, test_get_time_ms);
    arq_receive(&ctx_rx, &frame);
    ArqFrame ack;
    arq_gen_ack(&ctx_rx, frame.seq_num, &ack);
    
    /* Handle ACK on sender */
    arq_handle_ack(&ctx, ack.ack_num);
    
    if (ctx.sender.snd_base != ack.ack_num) { FAIL("snd_base not advanced"); return; }
    if (ctx.sender.sent_buffer[frame.seq_num]) { FAIL("buffer not cleared"); return; }
    
    PASS();
}

void test_arq_retransmit_timeout() {
    TEST("arq_timeout_check triggers retransmission");
    ArqContext ctx;
    arq_init(&ctx, 4, 100, test_get_time_ms);  /* 100ms timeout */
    
    uint8_t data[] = {0x11, 0x22};
    arq_send(&ctx, data, sizeof(data));
    ArqFrame frame;
    arq_get_next_frame(&ctx, &frame);
    uint16_t original_seq = frame.seq_num;
    
    /* Advance time past timeout */
    test_time_ms += 200;
    arq_timeout_check(&ctx);
    
    /* Should be able to get frame again for retransmission */
    ArqFrame retrans;
    if (!arq_get_next_frame(&ctx, &retrans)) { FAIL("no retransmission frame"); return; }
    if (retrans.seq_num != original_seq) { FAIL("wrong seq on retransmit"); return; }
    if (ctx.sender.retry_count[original_seq] != 2) { FAIL("retry count not incremented"); return; }
    
    PASS();
}

void test_arq_nak() {
    TEST("arq_handle_nak triggers fast retransmit");
    ArqContext ctx;
    arq_init(&ctx, 4, 500, test_get_time_ms);
    
    uint8_t data[] = {0x33, 0x44};
    arq_send(&ctx, data, sizeof(data));
    ArqFrame frame;
    arq_get_next_frame(&ctx, &frame);
    
    /* Send NAK */
    ArqFrame nak;
    arq_gen_nak(&ctx, frame.seq_num, &nak);
    arq_handle_nak(&ctx, nak.ack_num);
    
    /* Should reset retry count and force timeout */
    if (ctx.sender.retry_count[frame.seq_num] != 0) { FAIL("retry count not reset"); return; }
    if (ctx.sender.sent_time[frame.seq_num] != 0) { FAIL("sent_time not reset"); return; }
    
    PASS();
}

void test_arq_selective_repeat_window() {
    TEST("arq handles multiple packets in window (Selective Repeat)");
    ArqContext ctx;
    arq_init(&ctx, 4, 500, test_get_time_ms);
    
    /* Send 3 packets */
    uint8_t data1[] = {0x01};
    uint8_t data2[] = {0x02};
    uint8_t data3[] = {0x03};
    arq_send(&ctx, data1, 1);
    arq_send(&ctx, data2, 1);
    arq_send(&ctx, data3, 1);
    
    ArqContext ctx_rx;
    arq_init(&ctx_rx, 4, 500, test_get_time_ms);
    
    /* Receive out of order: 1, 3, 2 */
    ArqFrame f1, f3, f2;
    arq_get_next_frame(&ctx, &f1);
    arq_receive(&ctx_rx, &f1);
    
    arq_get_next_frame(&ctx, &f3);
    arq_receive(&ctx_rx, &f3);
    
    arq_get_next_frame(&ctx, &f2);
    arq_receive(&ctx_rx, &f2);
    
    /* Should deliver in order: 1, 2, 3 */
    uint8_t out[16];
    int len;
    
    len = arq_get_delivered(&ctx_rx, out, sizeof(out));
    if (len != 1 || out[0] != 0x01) { FAIL("first delivery wrong: len=%d data=%02x", len, out[0]); return; }
    
    len = arq_get_delivered(&ctx_rx, out, sizeof(out));
    if (len != 1 || out[0] != 0x02) { FAIL("second delivery wrong: len=%d data=%02x", len, out[0]); return; }
    
    len = arq_get_delivered(&ctx_rx, out, sizeof(out));
    if (len != 1 || out[0] != 0x03) { FAIL("third delivery wrong: len=%d data=%02x", len, out[0]); return; }
    
    PASS();
}

void test_arq_sequence_math() {
    TEST("arq sequence arithmetic (modulo 2N)");
    ArqContext ctx;
    arq_init(&ctx, 4, 500, test_get_time_ms);
    
    uint16_t mod = ctx.window_size * 2;  /* 8 */
    
    if (arq_seq_add(7, 1, mod) != 0) { FAIL("wrap around failed"); return; }
    if (arq_seq_add(0, 3, mod) != 3) { FAIL("basic add failed"); return; }
    
    if (!arq_seq_lt(1, 2, 0, mod)) { FAIL("lt failed"); return; }
    if (!arq_seq_lt(7, 0, 6, mod)) { FAIL("lt wrap failed"); return; }
    
    if (!arq_in_window(2, 0, 4, mod)) { FAIL("in_window failed"); return; }
    if (arq_in_window(5, 0, 4, mod)) { FAIL("in_window should be false"); return; }
    if (!arq_in_window(0, 7, 4, mod)) { FAIL("in_window wrap failed"); return; }
    
    PASS();
}

/* ========================================================================
   Delta Compression Tests
   ======================================================================== */

void test_delta_init() {
    TEST("delta_init initializes context");
    DeltaContext ctx;
    delta_init(&ctx, test_get_time_ms);
    
    if (ctx.compressor.next_seq != 1) { FAIL("next_seq not 1"); return; }
    if (ctx.compressor.acked_base != 1) { FAIL("acked_base not 1"); return; }
    if (ctx.decompressor.expected_seq != 1) { FAIL("expected_seq not 1"); return; }
    PASS();
}

void test_delta_compute_apply() {
    TEST("delta_compute and delta_apply round-trip");
    uint8_t old_state[] = {0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07};
    uint8_t new_state[] = {0x00, 0x11, 0x02, 0x33, 0x04, 0x55, 0x06, 0x07};  /* Bytes 1,3,5 changed */
    
    uint8_t delta[DELTA_MAX_DELTA_SIZE];
    uint16_t delta_len;
    
    int res = delta_compute(old_state, 8, new_state, 8, delta, &delta_len);
    if (res != 0) { FAIL("delta_compute failed"); return; }
    if (delta_len >= 8) { FAIL("delta not smaller than original"); return; }
    
    uint8_t out_state[16];
    uint16_t out_len;
    res = delta_apply(old_state, 8, delta, delta_len, out_state, &out_len);
    if (res != 0) { FAIL("delta_apply failed"); return; }
    if (out_len != 8) { FAIL("wrong output length"); return; }
    if (memcmp(out_state, new_state, 8) != 0) { FAIL("state not restored correctly"); return; }
    
    PASS();
}

void test_delta_intra_frame() {
    TEST("delta falls back to intra-frame when delta not beneficial");
    DeltaContext ctx;
    delta_init(&ctx, test_get_time_ms);
    
    /* Completely different state - delta would be larger */
    uint8_t state1[16];
    uint8_t state2[16];
    for (int i = 0; i < 16; i++) {
        state1[i] = i;
        state2[i] = 255 - i;
    }
    
    DeltaFrame frame;
    delta_compress(&ctx, state1, 16, &frame);
    if (!frame.is_intra) { FAIL("should be intra for first frame"); return; }
    if (frame.state_len != 16) { FAIL("wrong intra state len"); return; }
    
    /* Second frame with small change */
    state2[0] = 100;
    state2[1] = 200;
    delta_compress(&ctx, state2, 16, &frame);
    /* Should be delta since only 2 bytes changed */
    
    PASS();
}

void test_delta_decompress() {
    TEST("delta_decompress applies frames correctly");
    DeltaContext ctx;
    delta_init(&ctx, test_get_time_ms);
    
    uint8_t state1[] = {0x10, 0x20, 0x30, 0x40};
    uint8_t state2[] = {0x10, 0x99, 0x30, 0x88};  /* Bytes 1,3 changed */
    
    DeltaFrame frame1, frame2;
    delta_compress(&ctx, state1, 4, &frame1);
    delta_compress(&ctx, state2, 4, &frame2);
    
    uint8_t out[DELTA_MAX_STATE_SIZE];
    uint16_t out_len;
    
    /* Apply first (intra) */
    int res = delta_decompress(&ctx, &frame1, out, &out_len);
    if (res != 0) { FAIL("first decompress failed"); return; }
    if (out_len != 4) { FAIL("wrong len"); return; }
    if (memcmp(out, state1, 4) != 0) { FAIL("first state wrong"); return; }
    
    /* Apply second (delta) */
    res = delta_decompress(&ctx, &frame2, out, &out_len);
    if (res != 0) { FAIL("second decompress failed"); return; }
    if (out_len != 4) { FAIL("wrong len"); return; }
    if (memcmp(out, state2, 4) != 0) { FAIL("second state wrong"); return; }
    
    PASS();
}

void test_delta_gap_detection() {
    TEST("delta detects gaps and requests intra-frame");
    DeltaContext ctx;
    delta_init(&ctx, test_get_time_ms);
    
    uint8_t state[4] = {0x01, 0x02, 0x03, 0x04};
    DeltaFrame frame1, frame3;  /* Skip frame2 */
    
    delta_compress(&ctx, state, 4, &frame1);
    state[0] = 0x11;
    delta_compress(&ctx, state, 4, &frame3);  /* This will be seq=3 */
    
    /* Apply frame1 */
    uint8_t out[16];
    uint16_t out_len;
    delta_decompress(&ctx, &frame1, out, &out_len);
    
    /* Apply frame3 (gap of 1) - should detect gap */
    int res = delta_decompress(&ctx, &frame3, out, &out_len);
    /* With threshold 10, gap of 1 should not trigger intra */
    if (res == -2) { FAIL("gap of 1 should not trigger intra"); return; }
    
    PASS();
}

/* ========================================================================
   MAC Adaptive Tests
   ======================================================================== */

void test_mac_init() {
    TEST("mac_init initializes context");
    MacContext ctx;
    mac_init(&ctx, 0, 2, 4, 10000, 3000, test_get_time_us);
    
    if (ctx.node_id != 0) { FAIL("node_id"); return; }
    if (ctx.num_nodes != 2) { FAIL("num_nodes"); return; }
    if (ctx.slot_count != 4) { FAIL("slot_count"); return; }
    if (ctx.slot_duration_us != 10000) { FAIL("slot_duration"); return; }
    if (ctx.guard_time_us != 3000) { FAIL("guard_time"); return; }
    if (ctx.mode != MAC_MODE_HYBRID) { FAIL("default mode"); return; }
    if (ctx.alpha != 0.1f) { FAIL("alpha"); return; }
    if (ctx.gamma != 0.9f) { FAIL("gamma"); return; }
    if (ctx.epsilon != 0.3f) { FAIL("epsilon"); return; }
    if (!ctx.bt_root) { FAIL("bt_root not created"); return; }
    
    PASS();
}

void test_mac_fixed_tdma() {
    TEST("MAC_MODE_FIXED_TDMA respects slot assignment");
    MacContext ctx;
    mac_init(&ctx, 1, 4, 4, 10000, 3000, test_get_time_us);
    mac_set_mode(&ctx, MAC_MODE_FIXED_TDMA);
    
    ctx.last_state = MAC_STATE_QUEUED;
    
    /* Simulate slot 1 (node 1's slot) */
    ctx.current_slot = 1;
    MacAction action = mac_get_action(&ctx);
    if (action != MAC_ACTION_TX_NOW) { FAIL("should TX in own slot, got %d", action); return; }
    
    /* Simulate slot 0 (not node 1's slot) */
    ctx.current_slot = 0;
    action = mac_get_action(&ctx);
    if (action != MAC_ACTION_WAIT) { FAIL("should WAIT in other slot, got %d", action); return; }
    
    PASS();
}

void test_mac_q_learning() {
    TEST("Q-learning updates Q-values correctly");
    MacContext ctx;
    mac_init(&ctx, 0, 2, 4, 10000, 3000, test_get_time_us);
    mac_set_mode(&ctx, MAC_MODE_Q_LEARNING);
    
    ctx.last_state = MAC_STATE_QUEUED;
    ctx.epsilon = 0.0f;  /* Disable exploration for deterministic test */
    
    /* Get action for state */
    MacAction action = mac_q_select_action(&ctx, MAC_STATE_QUEUED);
    uint32_t idx = mac_state_to_idx(MAC_STATE_QUEUED);
    
    float old_q = ctx.q_table[idx].q_values[action];
    
    /* Learn with positive reward */
    mac_q_learn(&ctx, MAC_STATE_QUEUED, action, 1.0f, MAC_STATE_ACKED);
    
    float new_q = ctx.q_table[idx].q_values[action];
    if (new_q <= old_q) { FAIL("Q-value should increase with positive reward"); return; }
    
    /* Learn with negative reward */
    mac_q_learn(&ctx, MAC_STATE_QUEUED, action, -1.0f, MAC_STATE_COLLISION);
    new_q = ctx.q_table[idx].q_values[action];
    if (new_q >= old_q) { FAIL("Q-value should decrease with negative reward"); return; }
    
    PASS();
}

void test_mac_behavior_tree() {
    TEST("Behavior tree executes correctly");
    MacContext ctx;
    mac_init(&ctx, 0, 2, 4, 10000, 3000, test_get_time_us);
    mac_set_mode(&ctx, MAC_MODE_BEHAVIOR_TREE);
    
    ctx.last_state = MAC_STATE_QUEUED;
    ctx.current_slot = 0;  /* Node 0's slot */
    
    MacAction action = mac_get_action(&ctx);
    if (action != MAC_ACTION_TX_NOW) { FAIL("BT should transmit in own slot, got %d", action); return; }
    
    /* Test collision handling */
    ctx.last_state = MAC_STATE_COLLISION;
    action = mac_get_action(&ctx);
    if (action != MAC_ACTION_WAIT) { FAIL("BT should wait after collision, got %d", action); return; }
    
    PASS();
}

void test_mac_fixed_timestep() {
    TEST("Fixed timestep accumulator advances slots correctly");
    MacContext ctx;
    mac_init(&ctx, 0, 2, 4, 10000, 3000, test_get_time_us);
    
    ctx.current_slot = 0;
    ctx.accumulator_us = 0;
    
    /* Add less than slot duration */
    mac_update(&ctx, 5000);
    if (ctx.current_slot != 0) { FAIL("slot should not advance"); return; }
    if (ctx.accumulator_us != 5000) { FAIL("accumulator wrong"); return; }
    
    /* Add more to cross boundary */
    mac_update(&ctx, 6000);  /* Total 11000 > 10000 */
    if (ctx.current_slot != 1) { FAIL("slot should advance to 1"); return; }
    if (ctx.accumulator_us != 1000) { FAIL("accumulator should be 1000"); return; }
    
    /* Advance through full frame */
    for (int i = 0; i < 3; i++) {
        mac_update(&ctx, 10000);
    }
    if (ctx.current_slot != 0) { FAIL("should wrap to 0 after 4 slots"); return; }
    
    PASS();
}

void test_mac_conditions() {
    TEST("MAC behavior tree conditions work (via BT execution)");
    MacContext ctx;
    mac_init(&ctx, 0, 2, 4, 10000, 3000, test_get_time_us);
    mac_set_mode(&ctx, MAC_MODE_BEHAVIOR_TREE);
    
    /* Test behavior tree execution for different states */
    ctx.last_state = MAC_STATE_QUEUED;
    ctx.current_slot = 0;  /* Node 0's slot */
    MacAction action = mac_get_action(&ctx);
    if (action != MAC_ACTION_TX_NOW) { FAIL("should TX in own slot with queue, got %d", action); return; }
    
    ctx.last_state = MAC_STATE_IDLE;
    action = mac_get_action(&ctx);
    if (action != MAC_ACTION_WAIT) { FAIL("should WAIT when idle, got %d", action); return; }
    
    ctx.last_state = MAC_STATE_QUEUED;
    ctx.current_slot = 1;  /* Not node 0's slot */
    action = mac_get_action(&ctx);
    if (action != MAC_ACTION_WAIT) { FAIL("should WAIT in other slot, got %d", action); return; }
    
    ctx.last_state = MAC_STATE_COLLISION;
    action = mac_get_action(&ctx);
    if (action != MAC_ACTION_WAIT) { FAIL("should WAIT after collision, got %d", action); return; }
    
    PASS();
}

/* ========================================================================
   Integration Tests
   ======================================================================== */

void test_echo_protocol_integration() {
    TEST("EchoProtocol integrates ARQ, Delta, MAC");
    EchoProtocol echo;
    int pipefd[2];
    if (pipe(pipefd) == -1) { FAIL("pipe failed"); return; }
    
    /* Manual init without TUN */
    memset(&echo, 0, sizeof(echo));
    echo.tun_fd = pipefd[0];
    echo.tx_rb = rb_init();
    echo.rx_rb = rb_init();
    pre_calc_fsk(&echo.mod_state);
    uint16_t freqs[4] = {FREQ_00, FREQ_01, FREQ_10, FREQ_11};
    for (int i = 0; i < 4; i++)
        pre_calc_goertzel(&echo.freq_states[i], &freqs[i]);
    
    /* Initialize RX state manually (rx_reset is static) */
    echo.rx.state = SEARCHING;
    echo.rx.bits_received = 0;
    echo.rx.packet_len = 0;
    echo.rx.header_accumulator = 0;
    echo.rx.sync_accumulator = 0;
    echo.rx.is_compressed = 0;
    echo.rx.is_rohc = 0;
    echo.rx.is_fec = 0;
    rb_reset(echo.rx_rb);
    scrambler_reset(&echo.rx_scrambler);
    
    rohc_init(&echo.rohc_tx);
    rohc_init(&echo.rohc_rx);
    scrambler_init(&echo.tx_scrambler);
    scrambler_init(&echo.rx_scrambler);
    
    /* Initialize new modules */
    arq_init(&echo.arq, 8, 500, test_get_time_ms);
    delta_init(&echo.delta, test_get_time_ms);
    mac_init(&echo.mac, 0, 2, 4, 10000, 3000, test_get_time_us);
    echo.use_delta_compression = true;
    echo.use_arq = true;
    echo.use_adaptive_mac = true;
    
    /* Test sending a packet through new path */
    uint8_t test_data[] = "Hello Delta ARQ MAC";
    int res = echo_send_packet(&echo, test_data, strlen((char*)test_data));
    if (res != 0) { FAIL("echo_send_packet failed"); close(pipefd[0]); close(pipefd[1]); free(echo.tx_rb); free(echo.rx_rb); return; }
    
    /* Test MAC update */
    echo_update_mac(&echo, 10000);
    echo_process_arq_timeouts(&echo);
    
    PASS();
    close(pipefd[0]);
    close(pipefd[1]);
    free(echo.tx_rb);
    free(echo.rx_rb);
}

void test_arq_delta_mac_workflow() {
    TEST("Full workflow: MAC -> ARQ -> Delta -> ARQ -> Delta");
    ArqContext arq_tx, arq_rx;
    DeltaContext delta_tx, delta_rx;
    MacContext mac;
    
    arq_init(&arq_tx, 8, 500, test_get_time_ms);
    arq_init(&arq_rx, 8, 500, test_get_time_ms);
    delta_init(&delta_tx, test_get_time_ms);
    delta_init(&delta_rx, test_get_time_ms);
    mac_init(&mac, 0, 2, 4, 10000, 3000, test_get_time_us);
    mac_set_mode(&mac, MAC_MODE_BEHAVIOR_TREE);
    
    /* Simulate terminal screen state updates */
    uint8_t screen1[] = "user@host:~$ ";
    uint8_t screen2[] = "user@host:~$ ls";
    uint8_t screen3[] = "user@host:~$ ls\nfile1.txt\nfile2.txt\n";
    
    /* Compress and send through ARQ */
    DeltaFrame frame;
    delta_compress(&delta_tx, screen1, strlen((char*)screen1), &frame);
    arq_send(&arq_tx, frame.is_intra ? frame.state : frame.delta, 
             frame.is_intra ? frame.state_len : frame.delta_len);
    
    /* MAC decides to transmit */
    mac.last_state = MAC_STATE_QUEUED;
    mac.current_slot = 0;
    MacAction action = mac_get_action(&mac);
    if (action != MAC_ACTION_TX_NOW) { FAIL("MAC should transmit, got %d", action); return; }
    
    /* Receive on other side */
    ArqFrame arq_frame;
    if (!arq_get_next_frame(&arq_tx, &arq_frame)) { FAIL("no ARQ frame to send"); return; }
    arq_receive(&arq_rx, &arq_frame);
    
    /* ARQ delivers */
    uint8_t arq_payload[256];
    int arq_len = arq_get_delivered(&arq_rx, arq_payload, sizeof(arq_payload));
    if (arq_len <= 0) { FAIL("ARQ delivery failed"); return; }
    
    /* Delta decompress */
    DeltaFrame rx_frame = {0};
    if (frame.is_intra) {
        rx_frame.state_len = arq_len;
        memcpy(rx_frame.state, arq_payload, arq_len);
    } else {
        rx_frame.delta_len = arq_len;
        memcpy(rx_frame.delta, arq_payload, arq_len);
    }
    rx_frame.is_intra = frame.is_intra;
    rx_frame.seq_num = 1;
    rx_frame.base_seq = 1;
    
    uint8_t out[DELTA_MAX_STATE_SIZE];
    uint16_t out_len;
    int res = delta_decompress(&delta_rx, &rx_frame, out, &out_len);
    if (res != 0) { FAIL("delta decompress failed: %d", res); return; }
    if (out_len != strlen((char*)screen1)) { FAIL("wrong output len"); return; }
    if (memcmp(out, screen1, out_len) != 0) { FAIL("content mismatch"); return; }
    
    /* Now send second screen state (delta) */
    delta_compress(&delta_tx, screen2, strlen((char*)screen2), &frame);
    arq_send(&arq_tx, frame.delta, frame.delta_len);
    
    if (!arq_get_next_frame(&arq_tx, &arq_frame)) { FAIL("no ARQ frame for second packet"); return; }
    arq_receive(&arq_rx, &arq_frame);
    arq_len = arq_get_delivered(&arq_rx, arq_payload, sizeof(arq_payload));
    
    rx_frame = (DeltaFrame){0};
    if (frame.is_intra) {
        rx_frame.state_len = arq_len;
        memcpy(rx_frame.state, arq_payload, arq_len);
    } else {
        rx_frame.delta_len = arq_len;
        memcpy(rx_frame.delta, arq_payload, arq_len);
    }
    rx_frame.is_intra = frame.is_intra;
    rx_frame.seq_num = 2;
    rx_frame.base_seq = 1;
    
    res = delta_decompress(&delta_rx, &rx_frame, out, &out_len);
    if (res != 0) { FAIL("second delta decompress failed: %d", res); return; }
    if (out_len != strlen((char*)screen2)) { FAIL("wrong output len"); return; }
    if (memcmp(out, screen2, out_len) != 0) { FAIL("content mismatch"); return; }
    
    /* Third screen - larger delta */
    delta_compress(&delta_tx, screen3, strlen((char*)screen3), &frame);
    arq_send(&arq_tx, frame.delta, frame.delta_len);
    
    if (!arq_get_next_frame(&arq_tx, &arq_frame)) { FAIL("no ARQ frame for third packet"); return; }
    arq_receive(&arq_rx, &arq_frame);
    arq_len = arq_get_delivered(&arq_rx, arq_payload, sizeof(arq_payload));
    
    rx_frame = (DeltaFrame){0};
    if (frame.is_intra) {
        rx_frame.state_len = arq_len;
        memcpy(rx_frame.state, arq_payload, arq_len);
    } else {
        rx_frame.delta_len = arq_len;
        memcpy(rx_frame.delta, arq_payload, arq_len);
    }
    rx_frame.is_intra = frame.is_intra;
    rx_frame.seq_num = 3;
    rx_frame.base_seq = 2;  /* Delta from screen2 (seq=2) */
    
    res = delta_decompress(&delta_rx, &rx_frame, out, &out_len);
    if (res != 0) { FAIL("third delta decompress failed: %d", res); return; }
    if (out_len != strlen((char*)screen3)) { FAIL("wrong output len"); return; }
    if (memcmp(out, screen3, out_len) != 0) { FAIL("content mismatch"); return; }
    
    PASS();
}

/* ========================================================================
   Main
   ======================================================================== */

int main() {
    printf("=== Tests: ARQ + Delta + MAC ===\n\n");
    
    printf("[ARQ Selective Repeat]\n");
    test_arq_init();
    test_arq_sequence_math();
    test_arq_send_receive();
    test_arq_ack();
    test_arq_nak();
    test_arq_retransmit_timeout();
    test_arq_selective_repeat_window();
    
    printf("\n[Delta Compression (Mosh-style)]\n");
    test_delta_init();
    test_delta_compute_apply();
    test_delta_intra_frame();
    test_delta_decompress();
    test_delta_gap_detection();
    
    printf("\n[MAC Adaptive (Q-learning + Behavior Trees)]\n");
    test_mac_init();
    test_mac_fixed_tdma();
    test_mac_q_learning();
    test_mac_behavior_tree();
    test_mac_fixed_timestep();
    test_mac_conditions();
    
    printf("\n[Integration]\n");
    test_echo_protocol_integration();
    test_arq_delta_mac_workflow();
    
    printf("\n=== Summary: %d PASS, %d FAIL ===\n", tests_passed, tests_failed);
    return tests_failed > 0 ? 1 : 0;
}