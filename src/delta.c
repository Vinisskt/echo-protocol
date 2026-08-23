#include "../include/delta.h"
#include <string.h>
#include <stdlib.h>
#include <time.h>

static uint32_t default_get_time_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

void delta_init(DeltaContext *ctx, uint32_t (*get_time_ms)(void)) {
    memset(ctx, 0, sizeof(DeltaContext));
    
    ctx->get_time_ms = get_time_ms ? get_time_ms : default_get_time_ms;
    ctx->compressor.get_time_ms = ctx->get_time_ms;
    ctx->decompressor.get_time_ms = ctx->get_time_ms;
    
    ctx->compressor.next_seq = 1;  /* Start at 1, 0 = invalid */
    ctx->compressor.acked_base = 1;
    ctx->decompressor.expected_seq = 1;
    ctx->decompressor.last_acked = 1;
    ctx->decompressor.state_valid = false;
}

/* Compute delta between old and new state using simple bitmask */
int delta_compute(const uint8_t *old_state, uint16_t old_len,
                  const uint8_t *new_state, uint16_t new_len,
                  uint8_t *delta_out, uint16_t *delta_len) {
    uint16_t max_len = old_len > new_len ? old_len : new_len;
    if (max_len > DELTA_MAX_STATE_SIZE) max_len = DELTA_MAX_STATE_SIZE;
    
    /* Delta format:
     * byte 0: flags (bit 0 = length changed, bit 1 = truncation)
     * byte 1-2: new length (uint16_t, big-endian)
     * byte 3+: bitmask of changed bytes (1 bit per byte, up to max_len bits)
     * followed by: changed byte values in order
     */
    
    uint8_t flags = 0;
    uint16_t changed_count = 0;
    uint16_t bitmask_bytes = (max_len + 7) / 8;
    
    if (new_len != old_len) {
        flags |= 0x01;  /* Length changed */
        if (new_len < old_len) flags |= 0x02;  /* Truncation */
    }
    
    uint8_t bitmask[bitmask_bytes];
    memset(bitmask, 0, bitmask_bytes);
    
    uint8_t changed_values[max_len];
    
    for (uint16_t i = 0; i < max_len; i++) {
        uint8_t old_val = (i < old_len) ? old_state[i] : 0;
        uint8_t new_val = (i < new_len) ? new_state[i] : 0;
        
        if (old_val != new_val) {
            bitmask[i / 8] |= (1 << (i % 8));
            changed_values[changed_count++] = new_val;
        }
    }
    
    /* Check if delta is worth it (smaller than full state) */
    uint16_t delta_size = 1 + 2 + bitmask_bytes + changed_count;
    if (delta_size >= new_len + 3) {
        /* Not worth it, send intra-frame instead */
        return -1;
    }
    
    uint16_t out_idx = 0;
    delta_out[out_idx++] = flags;
    delta_out[out_idx++] = (new_len >> 8) & 0xFF;
    delta_out[out_idx++] = new_len & 0xFF;
    memcpy(delta_out + out_idx, bitmask, bitmask_bytes);
    out_idx += bitmask_bytes;
    memcpy(delta_out + out_idx, changed_values, changed_count);
    out_idx += changed_count;
    
    *delta_len = out_idx;
    return 0;
}

/* Apply delta to base state */
int delta_apply(const uint8_t *base_state, uint16_t base_len,
                const uint8_t *delta, uint16_t delta_len,
                uint8_t *out_state, uint16_t *out_len) {
    if (delta_len < 4) return -1;  /* Minimum: flags + 2-byte len + 1 bitmask byte */
    
    uint8_t flags = delta[0];
    uint16_t new_len = (delta[1] << 8) | delta[2];
    uint16_t max_len = base_len > new_len ? base_len : new_len;
    uint16_t bitmask_bytes = (max_len + 7) / 8;
    
    if (delta_len < 3 + bitmask_bytes) return -1;
    
    const uint8_t *bitmask = delta + 3;
    const uint8_t *changed_values = delta + 3 + bitmask_bytes;
    uint16_t changed_idx = 0;
    
    if (flags & 0x01) {
        /* Length changed - resize */
        if (new_len > DELTA_MAX_STATE_SIZE) return -1;
    }
    
    /* Start with base state (or zero if truncation) */
    uint16_t copy_len = (flags & 0x02) ? 0 : (base_len < new_len ? base_len : new_len);
    memcpy(out_state, base_state, copy_len);
    if (new_len > copy_len) {
        memset(out_state + copy_len, 0, new_len - copy_len);
    }
    
    /* Apply changes from bitmask */
    for (uint16_t i = 0; i < max_len && i < new_len; i++) {
        if (bitmask[i / 8] & (1 << (i % 8))) {
            if (changed_idx < delta_len - 3 - bitmask_bytes) {
                out_state[i] = changed_values[changed_idx++];
            }
        }
    }
    
    *out_len = new_len;
    return 0;
}

/* Store state in history */
static void store_state(DeltaStateEntry *history, uint16_t *head, uint16_t *count,
                        uint16_t seq_num, const uint8_t *state, uint16_t state_len,
                        uint32_t timestamp) {
    uint16_t idx = *head % DELTA_MAX_HISTORY;
    history[idx].seq_num = seq_num;
    history[idx].state_len = state_len;
    memcpy(history[idx].state, state, state_len);
    history[idx].timestamp_ms = timestamp;
    history[idx].valid = true;
    *head = idx + 1;
    if (*count < DELTA_MAX_HISTORY) (*count)++;
}

/* Find state in history by sequence number */
static DeltaStateEntry* find_state(DeltaStateEntry *history, uint16_t count, uint16_t seq_num) {
    for (uint16_t i = 0; i < count; i++) {
        uint16_t idx = i % DELTA_MAX_HISTORY;
        if (history[idx].valid && history[idx].seq_num == seq_num) {
            return &history[idx];
        }
    }
    return NULL;
}

/* Compressor: create delta frame from new state */
int delta_compress(DeltaContext *ctx, const uint8_t *new_state, uint16_t new_state_len,
                   DeltaFrame *out_frame) {
    DeltaCompressor *comp = &ctx->compressor;
    uint32_t now = ctx->get_time_ms();
    
    if (new_state_len > DELTA_MAX_STATE_SIZE) new_state_len = DELTA_MAX_STATE_SIZE;
    
    uint16_t seq = comp->next_seq++;
    if (comp->next_seq == 0) comp->next_seq = 1;  /* Wrap around, skip 0 */
    
    /* Find base state in history */
    DeltaStateEntry *base = find_state(comp->history, comp->history_count, comp->acked_base);
    
    uint8_t delta[DELTA_MAX_DELTA_SIZE];
    uint16_t delta_len = 0;
    bool is_intra = false;
    
    if (base && comp->acked_base != seq) {
        /* Try to compute delta from base */
        int res = delta_compute(base->state, base->state_len, new_state, new_state_len,
                                delta, &delta_len);
        if (res < 0) {
            is_intra = true;
        }
    } else {
        is_intra = true;
    }
    
    if (is_intra) {
        /* Send full state as intra-frame */
        delta_len = new_state_len;
        memcpy(delta, new_state, new_state_len);
    }
    
    /* Build frame */
    out_frame->seq_num = seq;
    out_frame->base_seq = comp->acked_base;
    out_frame->state_len = new_state_len;
    memcpy(out_frame->state, new_state, new_state_len);
    out_frame->delta_len = delta_len;
    memcpy(out_frame->delta, delta, delta_len);
    out_frame->timestamp_ms = now;
    out_frame->is_intra = is_intra;
    
    /* Store in history */
    store_state(comp->history, &comp->history_head, &comp->history_count,
                seq, new_state, new_state_len, now);
    
    return 0;
}

bool delta_has_pending(const DeltaContext *ctx) {
    return ctx->compressor.next_seq != ctx->compressor.acked_base;
}

/* Handle ACK from receiver - advance base sequence */
void delta_handle_ack(DeltaContext *ctx, uint16_t acked_seq) {
    DeltaCompressor *comp = &ctx->compressor;
    
    /* Advance acked_base to acked_seq + 1 */
    while (comp->acked_base != (uint16_t)(acked_seq + 1)) {
        uint16_t next = comp->acked_base + 1;
        if (next == 0) next = 1;  /* Wrap around, skip 0 */
        if (comp->acked_base == acked_seq) {
            comp->acked_base = next;
            break;
        }
        comp->acked_base = next;
    }
}

/* Handle intra-frame request from receiver */
void delta_handle_intra_req(DeltaContext *ctx, uint16_t base_seq) {
    /* Force next frame to be intra-frame relative to base_seq */
    ctx->compressor.acked_base = base_seq;
}

/* Decompressor: apply delta frame */
int delta_decompress(DeltaContext *ctx, const DeltaFrame *frame,
                     uint8_t *out_state, uint16_t *out_len) {
    DeltaDecompressor *decomp = &ctx->decompressor;
    
    if (frame->seq_num == 0) return -1;  /* Invalid sequence */
    
    uint16_t expected = decomp->expected_seq;
    
    if (frame->seq_num != expected) {
        /* Gap detected */
        if (frame->seq_num > expected) {
            uint16_t gap = frame->seq_num - expected;
            if (gap > DELTA_INTRA_THRESHOLD) {
                decomp->pending_intra_req = true;
                decomp->intra_req_base = expected;
                return -2;  /* Need intra-frame request */
            }
        }
        /* For now, accept out-of-order but don't deliver */
        return -3;
    }
    
    /* Valid next frame */
    if (frame->is_intra) {
        /* Full state */
        memcpy(decomp->current_state, frame->state, frame->state_len);
        decomp->current_state_len = frame->state_len;
        decomp->state_valid = true;
    } else {
        /* Delta from base */
        if (!decomp->state_valid) {
            return -4;  /* No base state */
        }
        
        DeltaStateEntry *base = find_state(decomp->history, decomp->history_count, frame->base_seq);
        if (!base) {
            decomp->pending_intra_req = true;
            decomp->intra_req_base = decomp->expected_seq;
            return -2;  /* Base not found, need intra */
        }
        
        int res = delta_apply(base->state, base->state_len,
                              frame->delta, frame->delta_len,
                              decomp->current_state, &decomp->current_state_len);
        if (res < 0) {
            decomp->pending_intra_req = true;
            decomp->intra_req_base = decomp->expected_seq;
            return -2;
        }
        decomp->state_valid = true;
    }
    
    /* Store in history */
    store_state(decomp->history, &decomp->history_head, &decomp->history_count,
                frame->seq_num, decomp->current_state, decomp->current_state_len,
                frame->timestamp_ms);
    
    /* Deliver */
    memcpy(out_state, decomp->current_state, decomp->current_state_len);
    *out_len = decomp->current_state_len;
    
    decomp->expected_seq++;
    if (decomp->expected_seq == 0) decomp->expected_seq = 1;
    decomp->last_acked = frame->seq_num;
    
    return 0;
}

bool delta_needs_intra(const DeltaContext *ctx) {
    return ctx->decompressor.pending_intra_req;
}

uint16_t delta_get_intra_base(const DeltaContext *ctx) {
    return ctx->decompressor.intra_req_base;
}

void delta_get_stats(const DeltaContext *ctx, DeltaStats *stats) {
    memset(stats, 0, sizeof(DeltaStats));
    (void)ctx;
}

void delta_reset_stats(DeltaContext *ctx) {
    (void)ctx;
}

/* Simple echo prediction: predict next state after a keypress */
int delta_predict_echo(DeltaContext *ctx, uint8_t key,
                       uint8_t *out_state, uint16_t *out_len) {
    DeltaDecompressor *decomp = &ctx->decompressor;
    
    if (!decomp->state_valid) return -1;
    
    /* Simple prediction: append key to current state if it looks like a buffer */
    if (decomp->current_state_len < DELTA_MAX_STATE_SIZE - 1) {
        memcpy(out_state, decomp->current_state, decomp->current_state_len);
        out_state[decomp->current_state_len] = key;
        *out_len = decomp->current_state_len + 1;
        return 0;
    }
    
    return -1;
}