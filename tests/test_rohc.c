#include <stdio.h>
#include <assert.h>
#include <stdint.h>
#include <string.h>
#include "../include/rohc.h"

static int tests_passed = 0;
static int tests_failed = 0;

#define TEST(name) do { printf("  - %s ... ", name); } while(0)
#define PASS() do { printf("PASS\n"); tests_passed++; } while(0)
#define FAIL(msg) do { printf("FAIL: %s\n", msg); tests_failed++; } while(0)

static uint8_t sample_ip[20] = {
    0x45, 0x00, 0x00, 0x1C,
    0x12, 0x34, 0x40, 0x00,
    0x40, 0x01, 0x00, 0x00,
    0x0A, 0x00, 0x00, 0x01,
    0x0A, 0x00, 0x00, 0x02
};

static uint8_t payload[8] = {0x08, 0x00, 0xF7, 0xFF, 0x00, 0x01, 0x00, 0x01};

void test_rohc_init_clears_state() {
    TEST("rohc_init clears state");
    ROHCState state;
    state.context_valid = 1;
    memset(state.context, 0xFF, ROHC_CTX_SIZE);
    rohc_init(&state);
    if (state.context_valid != 0) { FAIL("context_valid not cleared"); return; }
    for (int i = 0; i < ROHC_CTX_SIZE; i++) {
        if (state.context[i] != 0) { FAIL("context not zeroed"); return; }
    }
    PASS();
}

void test_rohc_compress_creates_context_first_packet() {
    TEST("rohc_compress creates context on first packet (returns 0)");
    ROHCState state;
    rohc_init(&state);
    uint8_t packet[28];
    memcpy(packet, sample_ip, 20);
    memcpy(packet + 20, payload, 8);
    uint8_t out[256];
    int result = rohc_compress(&state, packet, sizeof(packet), out, sizeof(out));
    if (result != 0) { FAIL("expected 0 for first packet"); return; }
    if (state.context_valid != 1) { FAIL("context not marked valid"); return; }
    PASS();
}

void test_rohc_compress_second_packet_reduces_size() {
    TEST("rohc_compress compresses second matching IP packet");
    ROHCState state;
    rohc_init(&state);
    uint8_t packet[28];
    memcpy(packet, sample_ip, 20);
    memcpy(packet + 20, payload, 8);
    uint8_t out[256];
    rohc_compress(&state, packet, sizeof(packet), out, sizeof(out));
    int result = rohc_compress(&state, packet, sizeof(packet), out, sizeof(out));
    if (result <= 0) { FAIL("compression should produce output"); return; }
    if (result >= (int)sizeof(packet)) { FAIL("compression should reduce size"); return; }
    PASS();
}

void test_rohc_compress_different_ip_not_compressed() {
    TEST("rohc_compress recycles context on new flow and returns 0");
    ROHCState state;
    rohc_init(&state);
    uint8_t packet1[28];
    memcpy(packet1, sample_ip, 20);
    memcpy(packet1 + 20, payload, 8);
    rohc_compress(&state, packet1, sizeof(packet1), NULL, 0);
    uint8_t packet2[28];
    memcpy(packet2, sample_ip, 20);
    memcpy(packet2 + 20, payload, 8);
    packet2[15] = 0x03;
    uint8_t out[256];
    int result = rohc_compress(&state, packet2, sizeof(packet2), out, sizeof(out));
    if (result != 0) { FAIL("should return 0 for different IP"); return; }
    PASS();
}

void test_rohc_compress_new_flow_compresses_after_context_recycle() {
    TEST("rohc_compress compresses second packet of new flow after context recycle");
    ROHCState state;
    rohc_init(&state);
    uint8_t flow_a[28];
    memcpy(flow_a, sample_ip, 20);
    memcpy(flow_a + 20, payload, 8);
    rohc_compress(&state, flow_a, sizeof(flow_a), NULL, 0);
    uint8_t buf[256];
    int r1 = rohc_compress(&state, flow_a, sizeof(flow_a), buf, sizeof(buf));
    if (r1 <= 0) { FAIL("flow A second packet should compress"); return; }
    uint8_t flow_b[28];
    memcpy(flow_b, sample_ip, 20);
    memcpy(flow_b + 20, payload, 8);
    flow_b[15] = 0x03;
    int r2 = rohc_compress(&state, flow_b, sizeof(flow_b), buf, sizeof(buf));
    if (r2 != 0) { FAIL("flow B first packet should return 0 (context recycle)"); return; }
    int r3 = rohc_compress(&state, flow_b, sizeof(flow_b), buf, sizeof(buf));
    if (r3 <= 0) { FAIL("flow B second packet should compress"); return; }
    int r4 = rohc_compress(&state, flow_a, sizeof(flow_a), buf, sizeof(buf));
    if (r4 != 0) { FAIL("flow A again should recycle context"); return; }
    PASS();
}

void test_rohc_compress_ttl_change_included() {
    TEST("rohc_compress includes TTL when it changes");
    ROHCState state;
    rohc_init(&state);
    uint8_t packet[28];
    memcpy(packet, sample_ip, 20);
    memcpy(packet + 20, payload, 8);
    rohc_compress(&state, packet, sizeof(packet), NULL, 0);
    packet[8] = 0x80;
    uint8_t out[256];
    int result = rohc_compress(&state, packet, sizeof(packet), out, sizeof(out));
    if (result <= 0) { FAIL("should compress despite TTL change"); return; }
    if (!(out[1] & ROHC_FLAG_TTL)) { FAIL("TTL flag should be set"); return; }
    PASS();
}

void test_rohc_compress_ip_id_included() {
    TEST("rohc_compress sends IP ID in compressed output");
    ROHCState state;
    rohc_init(&state);
    uint8_t packet[28];
    memcpy(packet, sample_ip, 20);
    memcpy(packet + 20, payload, 8);
    rohc_compress(&state, packet, sizeof(packet), NULL, 0);
    packet[4] = 0x56; packet[5] = 0x78;
    uint8_t out[256];
    int result = rohc_compress(&state, packet, sizeof(packet), out, sizeof(out));
    if (result <= 0) { FAIL("should compress despite IP ID change"); return; }
    if (!(out[1] & ROHC_FLAG_IP_ID)) { FAIL("IP ID flag should be set"); return; }
    // CRC at byte 2, IP ID at bytes 3-4
    if (out[3] != 0x56 || out[4] != 0x78) { FAIL("IP ID value wrong"); return; }
    PASS();
}

void test_rohc_decompress_reconstructs_original() {
    TEST("rohc_decompress reconstructs original packet after ROHC");
    ROHCState send, recv;
    rohc_init(&send);
    rohc_init(&recv);
    uint8_t original[28];
    memcpy(original, sample_ip, 20);
    memcpy(original + 20, payload, 8);
    original[10] = 0; original[11] = 0;
    uint16_t ck = ip_checksum(original, 20);
    original[10] = (ck >> 8) & 0xFF;
    original[11] = ck & 0xFF;
    rohc_compress(&send, original, sizeof(original), NULL, 0);
    memcpy(recv.context, send.context, 20);
    recv.context_valid = 1;
    uint8_t comp[256];
    int comp_size = rohc_compress(&send, original, sizeof(original), comp, sizeof(comp));
    if (comp_size <= 0) { FAIL("compression failed"); return; }
    uint8_t decomp[256];
    int decomp_size = rohc_decompress(&recv, comp, comp_size, decomp, sizeof(decomp));
    if (decomp_size != (int)sizeof(original)) { FAIL("decompressed size wrong"); return; }
    if (memcmp(decomp, original, sizeof(original)) != 0) { FAIL("decompressed content differs"); return; }
    PASS();
}

void test_rohc_decompress_no_context_returns_error() {
    TEST("rohc_decompress returns -1 with no context");
    ROHCState state;
    rohc_init(&state);
    uint8_t compressed[4] = {0, 1, 0, 0};
    uint8_t out[256];
    int result = rohc_decompress(&state, compressed, sizeof(compressed), out, sizeof(out));
    if (result != -1) { FAIL("should return -1"); return; }
    PASS();
}

void test_rohc_decompress_too_short_returns_error() {
    TEST("rohc_decompress returns -1 for data < 4 bytes");
    ROHCState state;
    rohc_init(&state);
    state.context_valid = 1;
    uint8_t compressed[2] = {0, 0};
    uint8_t out[256];
    int result = rohc_decompress(&state, compressed, 2, out, sizeof(out));
    if (result != -1) { FAIL("should return -1"); return; }
    PASS();
}

void test_ip_checksum_correct() {
    TEST("ip_checksum computes correct value for sample header");
    uint8_t hdr[20];
    memcpy(hdr, sample_ip, 20);
    hdr[10] = 0; hdr[11] = 0;
    uint16_t cksum = ip_checksum(hdr, 20);
    uint16_t expected = 0x14AB;
    if (cksum != expected) { FAIL("checksum mismatch"); return; }
    PASS();
}

void test_rohc_compress_non_ip_rejected() {
    TEST("rohc_compress returns negative for non-IPv4 packet");
    ROHCState state;
    rohc_init(&state);
    uint8_t non_ip[20] = {0};
    uint8_t out[256];
    int result = rohc_compress(&state, non_ip, 20, out, sizeof(out));
    if (result >= 0) { FAIL("expected negative"); return; }
    PASS();
}

void test_rohc_compress_too_short_rejected() {
    TEST("rohc_compress returns negative for packet < 20 bytes");
    ROHCState state;
    rohc_init(&state);
    uint8_t short_pkt[10] = {0};
    uint8_t out[256];
    int result = rohc_compress(&state, short_pkt, 10, out, sizeof(out));
    if (result >= 0) { FAIL("expected negative"); return; }
    PASS();
}

void test_rohc_roundtrip_ttl_change() {
    TEST("ROHC roundtrip with TTL change reconstructs correctly");
    ROHCState send, recv;
    rohc_init(&send);
    rohc_init(&recv);
    uint8_t original[28];
    memcpy(original, sample_ip, 20);
    memcpy(original + 20, payload, 8);
    original[10] = 0; original[11] = 0;
    uint16_t ck = ip_checksum(original, 20);
    original[10] = (ck >> 8) & 0xFF;
    original[11] = ck & 0xFF;
    rohc_compress(&send, original, sizeof(original), NULL, 0);
    memcpy(recv.context, send.context, 20);
    recv.context_valid = 1;
    original[8] = 0x80;
    original[10] = 0; original[11] = 0;
    ck = ip_checksum(original, 20);
    original[10] = (ck >> 8) & 0xFF;
    original[11] = ck & 0xFF;
    uint8_t comp[256];
    int comp_size = rohc_compress(&send, original, sizeof(original), comp, sizeof(comp));
    if (comp_size <= 0) { FAIL("compression failed"); return; }
    uint8_t decomp[256];
    int decomp_size = rohc_decompress(&recv, comp, comp_size, decomp, sizeof(decomp));
    if (decomp_size != (int)sizeof(original)) { FAIL("size wrong"); return; }
    if (decomp[8] != 0x80) { FAIL("TTL not updated"); return; }
    if (memcmp(decomp, original, sizeof(original)) != 0) { FAIL("content mismatch"); return; }
    PASS();
}

void test_rohc_checksum_recomputed() {
    TEST("rohc_decompress recomputes IP checksum correctly");
    ROHCState send, recv;
    rohc_init(&send);
    rohc_init(&recv);
    uint8_t original[28];
    memcpy(original, sample_ip, 20);
    memcpy(original + 20, payload, 8);
    original[10] = 0; original[11] = 0;
    uint16_t orig_cksum = ip_checksum(original, 20);
    original[10] = (orig_cksum >> 8) & 0xFF;
    original[11] = orig_cksum & 0xFF;
    rohc_compress(&send, original, sizeof(original), NULL, 0);
    memcpy(recv.context, send.context, 20);
    recv.context_valid = 1;
    uint8_t comp[256];
    int comp_size = rohc_compress(&send, original, sizeof(original), comp, sizeof(comp));
    if (comp_size <= 0) { FAIL("compression failed"); return; }
    uint8_t decomp[300];
    int decomp_size = rohc_decompress(&recv, comp, comp_size, decomp, sizeof(decomp));
    if (decomp_size <= 0) { FAIL("decompression failed"); return; }
    uint16_t new_cksum = (decomp[10] << 8) | decomp[11];
    uint8_t verify[20];
    memcpy(verify, decomp, 20);
    verify[10] = 0; verify[11] = 0;
    uint16_t expected = ip_checksum(verify, 20);
    if (new_cksum != expected) { FAIL("checksum wrong after decompress"); return; }
    PASS();
}

void test_rohc_compress_tos_change_included() {
    TEST("rohc_compress includes ToS when it changes");
    ROHCState state;
    rohc_init(&state);
    uint8_t packet[28];
    memcpy(packet, sample_ip, 20);
    memcpy(packet + 20, payload, 8);
    rohc_compress(&state, packet, sizeof(packet), NULL, 0);
    packet[1] = 0xA0;
    uint8_t out[256];
    int result = rohc_compress(&state, packet, sizeof(packet), out, sizeof(out));
    if (result <= 0) { FAIL("should compress despite ToS change"); return; }
    if (!(out[1] & ROHC_FLAG_TOS)) { FAIL("ToS flag should be set"); return; }
    // CRC at byte 2, IP ID at 3-4, ToS at byte 5
    if (out[5] != 0xA0) { FAIL("ToS value wrong in compressed"); return; }
    PASS();
}

void test_rohc_compress_flags_change_included() {
    TEST("rohc_compress includes Flags/FragOffset when they change");
    ROHCState state;
    rohc_init(&state);
    uint8_t packet[28];
    memcpy(packet, sample_ip, 20);
    memcpy(packet + 20, payload, 8);
    rohc_compress(&state, packet, sizeof(packet), NULL, 0);
    packet[6] = 0x00; packet[7] = 0x01;
    uint8_t out[256];
    int result = rohc_compress(&state, packet, sizeof(packet), out, sizeof(out));
    if (result <= 0) { FAIL("should compress despite Flags change"); return; }
    if (!(out[1] & ROHC_FLAG_FLAGS)) { FAIL("Flags flag should be set"); return; }
    // CRC at byte 2, IP ID at 3-4
    int pos = 5;
    if (out[1] & ROHC_FLAG_TOS) pos++;
    if (out[pos] != 0x00 || out[pos+1] != 0x01) { FAIL("Flags value wrong"); return; }
    PASS();
}

void test_rohc_compress_tos_stays_compressed_when_unchanged() {
    TEST("rohc_compress keeps compressing when ToS is unchanged");
    ROHCState state;
    rohc_init(&state);
    uint8_t packet[28];
    memcpy(packet, sample_ip, 20);
    memcpy(packet + 20, payload, 8);
    rohc_compress(&state, packet, sizeof(packet), NULL, 0);
    uint8_t out[256];
    int r1 = rohc_compress(&state, packet, sizeof(packet), out, sizeof(out));
    if (r1 <= 0) { FAIL("first compressed packet failed"); return; }
    int r2 = rohc_compress(&state, packet, sizeof(packet), out, sizeof(out));
    if (r2 <= 0) { FAIL("second compressed packet failed"); return; }
    PASS();
}

void test_rohc_roundtrip_tos_change() {
    TEST("ROHC roundtrip with ToS change reconstructs correctly");
    ROHCState send, recv;
    rohc_init(&send);
    rohc_init(&recv);
    uint8_t original[28];
    memcpy(original, sample_ip, 20);
    memcpy(original + 20, payload, 8);
    original[10] = 0; original[11] = 0;
    uint16_t ck = ip_checksum(original, 20);
    original[10] = (ck >> 8) & 0xFF;
    original[11] = ck & 0xFF;
    rohc_compress(&send, original, sizeof(original), NULL, 0);
    memcpy(recv.context, send.context, 20);
    recv.context_valid = 1;
    original[1] = 0xA0;
    original[10] = 0; original[11] = 0;
    ck = ip_checksum(original, 20);
    original[10] = (ck >> 8) & 0xFF;
    original[11] = ck & 0xFF;
    uint8_t comp[256];
    int comp_size = rohc_compress(&send, original, sizeof(original), comp, sizeof(comp));
    if (comp_size <= 0) { FAIL("compression failed"); return; }
    uint8_t decomp[256];
    int decomp_size = rohc_decompress(&recv, comp, comp_size, decomp, sizeof(decomp));
    if (decomp_size != (int)sizeof(original)) { FAIL("size wrong"); return; }
    if (decomp[1] != 0xA0) { FAIL("ToS not updated"); return; }
    if (memcmp(decomp, original, sizeof(original)) != 0) { FAIL("content mismatch"); return; }
    PASS();
}

void test_rohc_roundtrip_flags_change() {
    TEST("ROHC roundtrip with Flags change reconstructs correctly");
    ROHCState send, recv;
    rohc_init(&send);
    rohc_init(&recv);
    uint8_t original[28];
    memcpy(original, sample_ip, 20);
    memcpy(original + 20, payload, 8);
    original[10] = 0; original[11] = 0;
    uint16_t ck = ip_checksum(original, 20);
    original[10] = (ck >> 8) & 0xFF;
    original[11] = ck & 0xFF;
    rohc_compress(&send, original, sizeof(original), NULL, 0);
    memcpy(recv.context, send.context, 20);
    recv.context_valid = 1;
    original[6] = 0x00; original[7] = 0x40;
    original[10] = 0; original[11] = 0;
    ck = ip_checksum(original, 20);
    original[10] = (ck >> 8) & 0xFF;
    original[11] = ck & 0xFF;
    uint8_t comp[256];
    int comp_size = rohc_compress(&send, original, sizeof(original), comp, sizeof(comp));
    if (comp_size <= 0) { FAIL("compression failed"); return; }
    uint8_t decomp[256];
    int decomp_size = rohc_decompress(&recv, comp, comp_size, decomp, sizeof(decomp));
    if (decomp_size != (int)sizeof(original)) { FAIL("size wrong"); return; }
    if (decomp[6] != 0x00 || decomp[7] != 0x40) { FAIL("Flags not updated"); return; }
    if (memcmp(decomp, original, sizeof(original)) != 0) { FAIL("content mismatch"); return; }
    PASS();
}

void test_rohc_roundtrip_all_mutable_changes() {
    TEST("ROHC roundtrip with ToS+Flags+TTL+IP ID all changed");
    ROHCState send, recv;
    rohc_init(&send);
    rohc_init(&recv);
    uint8_t original[28];
    memcpy(original, sample_ip, 20);
    memcpy(original + 20, payload, 8);
    original[10] = 0; original[11] = 0;
    uint16_t ck = ip_checksum(original, 20);
    original[10] = (ck >> 8) & 0xFF;
    original[11] = ck & 0xFF;
    rohc_compress(&send, original, sizeof(original), NULL, 0);
    memcpy(recv.context, send.context, 20);
    recv.context_valid = 1;
    original[1] = 0xB8;
    original[4] = 0xFE; original[5] = 0xDC;
    original[6] = 0x00; original[7] = 0x01;
    original[8] = 0x01;
    original[10] = 0; original[11] = 0;
    ck = ip_checksum(original, 20);
    original[10] = (ck >> 8) & 0xFF;
    original[11] = ck & 0xFF;
    uint8_t comp[256];
    int comp_size = rohc_compress(&send, original, sizeof(original), comp, sizeof(comp));
    if (comp_size <= 0) { FAIL("compression failed"); return; }
    uint8_t decomp[256];
    int decomp_size = rohc_decompress(&recv, comp, comp_size, decomp, sizeof(decomp));
    if (decomp_size != (int)sizeof(original)) { FAIL("size wrong"); return; }
    if (decomp[1] != 0xB8) { FAIL("ToS mismatch"); return; }
    if (decomp[4] != 0xFE || decomp[5] != 0xDC) { FAIL("IP ID mismatch"); return; }
    if (decomp[6] != 0x00 || decomp[7] != 0x01) { FAIL("Flags mismatch"); return; }
    if (decomp[8] != 0x01) { FAIL("TTL mismatch"); return; }
    if (memcmp(decomp, original, sizeof(original)) != 0) { FAIL("content mismatch"); return; }
    PASS();
}

void test_rohc_compress_then_tos_then_flags_continuous() {
    TEST("ROHC continuous compression with alternating ToS/Flags changes");
    ROHCState state;
    rohc_init(&state);
    uint8_t base[28];
    memcpy(base, sample_ip, 20);
    memcpy(base + 20, payload, 8);
    base[10] = 0; base[11] = 0;
    uint16_t ck = ip_checksum(base, 20);
    base[10] = (ck >> 8) & 0xFF;
    base[11] = ck & 0xFF;
    rohc_compress(&state, base, sizeof(base), NULL, 0);

    uint8_t pkt[28];
    uint8_t out[256];
    int res;

    memcpy(pkt, base, sizeof(pkt));
    pkt[1] = 0xC0;
    pkt[10] = 0; pkt[11] = 0;
    ck = ip_checksum(pkt, 20);
    pkt[10] = (ck >> 8) & 0xFF;
    pkt[11] = ck & 0xFF;
    res = rohc_compress(&state, pkt, sizeof(pkt), out, sizeof(out));
    if (res <= 0 || !(out[1] & ROHC_FLAG_TOS)) { FAIL("ToS change not detected"); return; }

    memcpy(pkt, base, sizeof(pkt));
    pkt[6] = 0x40; pkt[7] = 0x01;
    pkt[10] = 0; pkt[11] = 0;
    ck = ip_checksum(pkt, 20);
    pkt[10] = (ck >> 8) & 0xFF;
    pkt[11] = ck & 0xFF;
    res = rohc_compress(&state, pkt, sizeof(pkt), out, sizeof(out));
    if (res <= 0 || !(out[1] & ROHC_FLAG_FLAGS)) { FAIL("Flags change not detected"); return; }

    res = rohc_compress(&state, base, sizeof(base), out, sizeof(out));
    if (res <= 0) { FAIL("back to base should compress"); return; }
    PASS();
}

void test_rohc_sync_context_updates_on_new_flow() {
    TEST("rohc_sync_context updates context when flow changes");
    ROHCState state;
    rohc_init(&state);
    uint8_t flow_a[28];
    memcpy(flow_a, sample_ip, 20);
    memcpy(flow_a + 20, payload, 8);
    rohc_sync_context(&state, flow_a, sizeof(flow_a));
    if (!state.context_valid) { FAIL("context should be valid after sync"); return; }
    if (memcmp(state.context, flow_a, 20) != 0) { FAIL("context should match flow A"); return; }
    uint8_t flow_b[28];
    memcpy(flow_b, sample_ip, 20);
    memcpy(flow_b + 20, payload, 8);
    flow_b[15] = 0x05;
    rohc_sync_context(&state, flow_b, sizeof(flow_b));
    if (memcmp(state.context, flow_b, 20) != 0) { FAIL("context should update to flow B"); return; }
    rohc_sync_context(&state, flow_b, sizeof(flow_b));
    if (memcmp(state.context, flow_b, 20) != 0) { FAIL("same flow should not change context"); return; }
    PASS();
}

void test_rohc_sync_context_ignores_non_ip() {
    TEST("rohc_sync_context ignores non-IPv4 packets");
    ROHCState state;
    rohc_init(&state);
    uint8_t non_ip[20] = {0};
    rohc_sync_context(&state, non_ip, 20);
    if (state.context_valid) { FAIL("context should remain invalid"); return; }
    PASS();
}

void test_rohc_sync_context_ignores_short_packets() {
    TEST("rohc_sync_context ignores packets < 20 bytes");
    ROHCState state;
    rohc_init(&state);
    uint8_t short_pkt[10] = {0x45};
    rohc_sync_context(&state, short_pkt, 10);
    if (state.context_valid) { FAIL("context should remain invalid"); return; }
    PASS();
}

int main() {
    printf("=== ROHC Unit Tests ===\n\n");

    printf("[init]\n");
    test_rohc_init_clears_state();

    printf("\n[compress]\n");
    test_rohc_compress_creates_context_first_packet();
    test_rohc_compress_second_packet_reduces_size();
    test_rohc_compress_different_ip_not_compressed();
    test_rohc_compress_new_flow_compresses_after_context_recycle();
    test_rohc_compress_ttl_change_included();
    test_rohc_compress_ip_id_included();
    test_rohc_compress_tos_change_included();
    test_rohc_compress_flags_change_included();
    test_rohc_compress_tos_stays_compressed_when_unchanged();
    test_rohc_compress_non_ip_rejected();
    test_rohc_compress_too_short_rejected();

    printf("\n[decompress]\n");
    test_rohc_decompress_reconstructs_original();
    test_rohc_decompress_no_context_returns_error();
    test_rohc_decompress_too_short_returns_error();

    printf("\n[roundtrip]\n");
    test_rohc_roundtrip_ttl_change();
    test_rohc_roundtrip_tos_change();
    test_rohc_roundtrip_flags_change();
    test_rohc_roundtrip_all_mutable_changes();
    test_rohc_compress_then_tos_then_flags_continuous();
    test_rohc_checksum_recomputed();

    printf("\n[ip_checksum]\n");
    test_ip_checksum_correct();

    printf("\n[sync_context]\n");
    test_rohc_sync_context_updates_on_new_flow();
    test_rohc_sync_context_ignores_non_ip();
    test_rohc_sync_context_ignores_short_packets();

    printf("\n=== Summary: %d PASS, %d FAIL ===\n", tests_passed, tests_failed);
    return tests_failed > 0 ? 1 : 0;
}
