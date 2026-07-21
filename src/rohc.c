#include "../include/rohc.h"
#include <string.h>
#include <stdio.h>

void rohc_init(ROHCState *state) {
    memset(state->context, 0, sizeof(state->context));
    state->context_valid = 0;
}

uint16_t ip_checksum(uint8_t *header, int len) {
    uint32_t sum = 0;
    for (int i = 0; i < len; i += 2) {
        sum += (header[i] << 8) | header[i + 1];
        if (sum & 0xFFFF0000) {
            sum = (sum & 0xFFFF) + (sum >> 16);
        }
    }
    return (uint16_t)~(sum & 0xFFFF);
}

static int compress_ipv4(ROHCState *state, uint8_t *packet, int packet_len, uint8_t *out, int out_len) {
    if (packet_len < 20) return -2;

    int ihl = (packet[0] & 0x0F) * 4;
    if (ihl < 20 || packet_len < ihl) return -2;

    if (!state->context_valid) {
        int copy = ihl < ROHC_CTX_SIZE ? ihl : ROHC_CTX_SIZE;
        memcpy(state->context, packet, copy);
        state->context_valid = 1;
        return 0;
    }

    uint8_t *ctx = state->context;

    if ((packet[0] & 0xF0) != (ctx[0] & 0xF0) ||
        packet[9] != ctx[9] ||
        memcmp(packet + 12, ctx + 12, 8) != 0) {
        int copy = ihl < ROHC_CTX_SIZE ? ihl : ROHC_CTX_SIZE;
        memcpy(ctx, packet, copy);
        return 0;
    }

    if ((packet[0] & 0x0F) != (ctx[0] & 0x0F)) {
        int copy = ihl < ROHC_CTX_SIZE ? ihl : ROHC_CTX_SIZE;
        memcpy(ctx, packet, copy);
        return 0;
    }

    uint8_t flags = ROHC_FLAG_IP_ID;

    if (packet[1] != ctx[1]) {
        flags |= ROHC_FLAG_TOS;
    }
    if (packet[6] != ctx[6] || packet[7] != ctx[7]) {
        flags |= ROHC_FLAG_FLAGS;
    }
    if (packet[8] != ctx[8]) {
        flags |= ROHC_FLAG_TTL;
    }

    int payload_len = packet_len - ihl;
    int needed = 1 + 1 + 2 +
        ((flags & ROHC_FLAG_TOS)   ? 1 : 0) +
        ((flags & ROHC_FLAG_FLAGS) ? 2 : 0) +
        ((flags & ROHC_FLAG_TTL)   ? 1 : 0) +
        payload_len;
    if (needed > out_len) return -3;

    int pos = 0;
    out[pos++] = 0;
    out[pos++] = flags;
    out[pos++] = packet[4];
    out[pos++] = packet[5];
    if (flags & ROHC_FLAG_TOS)   out[pos++] = packet[1];
    if (flags & ROHC_FLAG_FLAGS) { out[pos++] = packet[6]; out[pos++] = packet[7]; }
    if (flags & ROHC_FLAG_TTL)   out[pos++] = packet[8];
    memcpy(out + pos, packet + ihl, payload_len);

    ctx[1] = packet[1];
    ctx[4] = packet[4]; ctx[5] = packet[5];
    ctx[6] = packet[6]; ctx[7] = packet[7];
    ctx[8] = packet[8];

    return pos + payload_len;
}

static int compress_ipv6(ROHCState *state, uint8_t *packet, int packet_len, uint8_t *out, int out_len) {
    if (packet_len < 40) return -2;

    if (!state->context_valid) {
        memcpy(state->context, packet, ROHC_CTX_SIZE);
        state->context_valid = 1;
        return 0;
    }

    uint8_t *ctx = state->context;

    if ((packet[0] & 0xF0) != (ctx[0] & 0xF0) ||
        packet[6] != ctx[6] ||
        memcmp(packet + 8, ctx + 8, 32) != 0) {
        memcpy(ctx, packet, ROHC_CTX_SIZE);
        return 0;
    }

    uint8_t flags = 0;

    if (packet[6] != ctx[6]) {
        flags |= ROHC_V6_FLAG_NEXT_HDR;
    }
    if (packet[7] != ctx[7]) {
        flags |= ROHC_V6_FLAG_HOP_LIMIT;
    }
    if ((packet[0] & 0x0F) != (ctx[0] & 0x0F) ||
        (packet[1] >> 4) != (ctx[1] >> 4)) {
        flags |= ROHC_V6_FLAG_TC;
    }

    int payload_len = packet_len - 40;
    int needed = 1 + 1 + 2 +
        ((flags & ROHC_V6_FLAG_NEXT_HDR)  ? 1 : 0) +
        ((flags & ROHC_V6_FLAG_HOP_LIMIT) ? 1 : 0) +
        ((flags & ROHC_V6_FLAG_TC)        ? 1 : 0) +
        payload_len;
    if (needed > out_len) return -3;

    int pos = 0;
    out[pos++] = ROHC_VERSION_V6;
    out[pos++] = flags;
    out[pos++] = packet[4];
    out[pos++] = packet[5];

    if (flags & ROHC_V6_FLAG_NEXT_HDR)  out[pos++] = packet[6];
    if (flags & ROHC_V6_FLAG_HOP_LIMIT) out[pos++] = packet[7];
    if (flags & ROHC_V6_FLAG_TC)        out[pos++] = packet[0] & 0x0F;

    memcpy(out + pos, packet + 40, payload_len);

    ctx[0] = (ctx[0] & 0xF0) | (packet[0] & 0x0F);
    ctx[1] = (packet[0] & 0x0F) << 4 | (ctx[1] & 0x0F);
    ctx[4] = packet[4]; ctx[5] = packet[5];
    ctx[6] = packet[6];
    ctx[7] = packet[7];

    return pos + payload_len;
}

int rohc_compress(ROHCState *state, uint8_t *packet, int packet_len, uint8_t *out, int out_len) {
    if (packet_len < 20) return -2;

    uint8_t version = packet[0] & 0xF0;

    if (version == 0x40) {
        return compress_ipv4(state, packet, packet_len, out, out_len);
    }

    if (version == 0x60) {
        return compress_ipv6(state, packet, packet_len, out, out_len);
    }

    return -1;
}

static int rohc_decompress_ipv6(ROHCState *state, uint8_t *compressed, int comp_len, uint8_t *out, int out_len);

int rohc_decompress(ROHCState *state, uint8_t *compressed, int comp_len, uint8_t *out, int out_len) {
    if (!state->context_valid) {
        fprintf(stderr, "[ROHC] No context available for decompression\n");
        return -1;
    }
    if (comp_len < 4) {
        fprintf(stderr, "[ROHC] Compressed data too short (%d)\n", comp_len);
        return -1;
    }

    if (compressed[0] & ROHC_VERSION_V6) {
        return rohc_decompress_ipv6(state, compressed, comp_len, out, out_len);
    }

    uint8_t flags = compressed[1];
    int pos = 2;

    int ctx_header_size = (state->context[0] & 0x0F) * 4;
    if (ctx_header_size < 20) {
        fprintf(stderr, "[ROHC] Invalid context header size\n");
        return -1;
    }

    uint8_t rebuilt[64];
    memcpy(rebuilt, state->context, ctx_header_size);

    if (flags & ROHC_FLAG_IP_ID) {
        if (pos + 2 > comp_len) return -1;
        rebuilt[4] = compressed[pos++];
        rebuilt[5] = compressed[pos++];
    }

    if (flags & ROHC_FLAG_TOS) {
        if (pos + 1 > comp_len) return -1;
        rebuilt[1] = compressed[pos++];
    }

    if (flags & ROHC_FLAG_FLAGS) {
        if (pos + 2 > comp_len) return -1;
        rebuilt[6] = compressed[pos++];
        rebuilt[7] = compressed[pos++];
    }

    if (flags & ROHC_FLAG_TTL) {
        if (pos + 1 > comp_len) return -1;
        rebuilt[8] = compressed[pos++];
    }

    int payload_len = comp_len - pos;
    if (payload_len < 0) return -1;

    int total_len = ctx_header_size + payload_len;
    rebuilt[2] = (total_len >> 8) & 0xFF;
    rebuilt[3] = total_len & 0xFF;
    rebuilt[10] = 0;
    rebuilt[11] = 0;

    uint16_t cksum = ip_checksum(rebuilt, ctx_header_size);
    rebuilt[10] = (cksum >> 8) & 0xFF;
    rebuilt[11] = cksum & 0xFF;

    if (total_len > out_len) return -1;
    memcpy(out, rebuilt, ctx_header_size);

    if (payload_len > 0) {
        memcpy(out + ctx_header_size, compressed + pos, payload_len);
    }

    state->context[1] = rebuilt[1];
    state->context[4] = rebuilt[4];
    state->context[5] = rebuilt[5];
    state->context[6] = rebuilt[6];
    state->context[7] = rebuilt[7];
    state->context[8] = rebuilt[8];

    return total_len;
}

static int rohc_decompress_ipv6(ROHCState *state, uint8_t *compressed, int comp_len, uint8_t *out, int out_len) {
    uint8_t flags = compressed[1];
    int pos = 2;

    uint8_t rebuilt[40];
    memcpy(rebuilt, state->context, 40);

    if (pos + 2 > comp_len) return -1;
    rebuilt[4] = compressed[pos++];
    rebuilt[5] = compressed[pos++];

    if (flags & ROHC_V6_FLAG_NEXT_HDR) {
        if (pos + 1 > comp_len) return -1;
        rebuilt[6] = compressed[pos++];
    }

    if (flags & ROHC_V6_FLAG_HOP_LIMIT) {
        if (pos + 1 > comp_len) return -1;
        rebuilt[7] = compressed[pos++];
    }

    if (flags & ROHC_V6_FLAG_TC) {
        if (pos + 1 > comp_len) return -1;
        uint8_t tc_lo = compressed[pos++];
        rebuilt[0] = 0x60 | tc_lo;
        rebuilt[1] = (tc_lo << 4) | (rebuilt[1] & 0x0F);
    }

    int payload_len = comp_len - pos;
    if (payload_len < 0) return -1;

    int total_len = 40 + payload_len;
    if (total_len > out_len) return -1;

    memcpy(out, rebuilt, 40);
    if (payload_len > 0) {
        memcpy(out + 40, compressed + pos, payload_len);
    }

    state->context[0] = rebuilt[0];
    state->context[1] = rebuilt[1];
    state->context[4] = rebuilt[4]; state->context[5] = rebuilt[5];
    state->context[6] = rebuilt[6];
    state->context[7] = rebuilt[7];

    return total_len;
}

void rohc_reset(ROHCState *state) {
    state->context_valid = 0;
}

void rohc_sync_context(ROHCState *state, uint8_t *packet, int packet_len) {
    uint8_t version = packet[0] & 0xF0;

    if (version == 0x40) {
        if (packet_len < 20) return;
        int ihl = (packet[0] & 0x0F) * 4;
        if (ihl < 20 || packet_len < ihl) return;
        int copy_len = ihl < ROHC_CTX_SIZE ? ihl : ROHC_CTX_SIZE;
        if (!state->context_valid ||
            (packet[0] & 0xF0) != (state->context[0] & 0xF0) ||
            packet[9] != state->context[9] ||
            memcmp(packet + 12, state->context + 12, 8) != 0) {
            memcpy(state->context, packet, copy_len);
            state->context_valid = 1;
        }
        return;
    }

    if (version == 0x60) {
        if (packet_len < 40) return;
        if (!state->context_valid ||
            (packet[0] & 0xF0) != (state->context[0] & 0xF0) ||
            packet[6] != state->context[6] ||
            memcmp(packet + 8, state->context + 8, 32) != 0) {
            memcpy(state->context, packet, ROHC_CTX_SIZE);
            state->context_valid = 1;
        }
    }
}
