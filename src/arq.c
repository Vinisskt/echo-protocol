#include "../include/arq.h"
#include <string.h>
#include <stdlib.h>
#include <time.h>

static uint32_t default_get_time_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

void arq_init(ArqContext *ctx, uint16_t window_size, uint32_t timeout_ms,
              uint32_t (*get_time_ms)(void)) {
    memset(ctx, 0, sizeof(ArqContext));
    
    if (window_size == 0 || window_size > ARQ_MAX_WINDOW)
        window_size = ARQ_MAX_WINDOW;
    if (timeout_ms == 0)
        timeout_ms = ARQ_DEFAULT_TIMEOUT_MS;
    
    ctx->window_size = window_size;
    ctx->timeout_ms = timeout_ms;
    ctx->get_time_ms = get_time_ms ? get_time_ms : default_get_time_ms;
    
    ctx->sender.window_size = window_size;
    ctx->sender.snd_base = 0;
    ctx->sender.snd_next = 0;
    ctx->sender.estimated_rtt_ms = timeout_ms;
    ctx->sender.dev_rtt_ms = timeout_ms / 4;
    
    ctx->receiver.window_size = window_size;
    ctx->receiver.rcv_base = 0;
    ctx->receiver.rcv_next = 0;
    ctx->receiver.last_ack_time_ms = 0;
}

static uint16_t mod_seq(uint16_t seq, uint16_t mod) {
    return seq % mod;
}

uint16_t arq_seq_add(uint16_t a, uint16_t b, uint16_t mod) {
    return mod_seq(a + b, mod);
}

bool arq_seq_lt(uint16_t a, uint16_t b, uint16_t base, uint16_t mod) {
    uint16_t diff_a = mod_seq(a + mod - base, mod);
    uint16_t diff_b = mod_seq(b + mod - base, mod);
    return diff_a < diff_b;
}

bool arq_in_window(uint16_t seq, uint16_t base, uint16_t window_size, uint16_t mod) {
    uint16_t diff = mod_seq(seq + mod - base, mod);
    return diff < window_size;
}

/* Sender: queue a new frame for transmission */
int arq_send(ArqContext *ctx, const uint8_t *data, uint16_t len) {
    uint16_t mod = ctx->window_size * 2;
    uint16_t next = ctx->sender.snd_next;
    
    if (!arq_in_window(next, ctx->sender.snd_base, ctx->window_size, mod)) {
        return -1;  /* Window full */
    }
    
    if (len > 256) len = 256;
    memcpy(ctx->sender.sent_data[next], data, len);
    ctx->sender.sent_len[next] = len;
    ctx->sender.sent_buffer[next] = true;
    ctx->sender.retry_count[next] = 0;
    ctx->sender.snd_next = mod_seq(next + 1, mod);
    
    return 0;
}

bool arq_has_pending_send(const ArqContext *ctx) {
    (void)ctx->window_size;
    return ctx->sender.snd_base != ctx->sender.snd_next;
}

/* Get next frame to transmit (new or retransmission).
 * Returns 1 if frame is ready in out_frame, 0 if no frame to send. */
int arq_get_next_frame(ArqContext *ctx, ArqFrame *out_frame) {
    uint32_t now = ctx->get_time_ms();
    uint16_t mod = ctx->window_size * 2;
    
    /* First, check for retransmissions (packets with elapsed timeout) */
    uint16_t seq = ctx->sender.snd_base;
    while (seq != ctx->sender.snd_next) {
        if (ctx->sender.sent_buffer[seq] && ctx->sender.sent_time[seq] > 0) {
            uint32_t elapsed = now - ctx->sender.sent_time[seq];
            if (elapsed >= ctx->timeout_ms) {
                /* Retransmit */
                if (ctx->sender.retry_count[seq] >= ARQ_MAX_RETRIES) {
                    /* Max retries exceeded - drop and advance base */
                    ctx->sender.sent_buffer[seq] = false;
                    ctx->sender.snd_base = mod_seq(seq + 1, mod);
                    continue;
                }
                
                out_frame->seq_num = seq;
                out_frame->ack_num = 0;
                out_frame->type = ARQ_FRAME_DATA;
                out_frame->payload_len = ctx->sender.sent_len[seq];
                memcpy(out_frame->payload, ctx->sender.sent_data[seq], out_frame->payload_len);
                out_frame->timestamp_ms = now;
                
                ctx->sender.sent_time[seq] = now;
                ctx->sender.retry_count[seq]++;
                
                return 1;
            }
        }
        seq = mod_seq(seq + 1, mod);
    }
    
    /* Then, check for new transmissions (packets not yet sent) */
    seq = ctx->sender.snd_base;
    while (seq != ctx->sender.snd_next) {
        if (ctx->sender.sent_buffer[seq] && ctx->sender.sent_time[seq] == 0) {
            out_frame->seq_num = seq;
            out_frame->ack_num = 0;
            out_frame->type = ARQ_FRAME_DATA;
            out_frame->payload_len = ctx->sender.sent_len[seq];
            memcpy(out_frame->payload, ctx->sender.sent_data[seq], out_frame->payload_len);
            out_frame->timestamp_ms = now;
            
            ctx->sender.sent_time[seq] = now;
            ctx->sender.retry_count[seq] = 1;
            
            return 1;
        }
        seq = mod_seq(seq + 1, mod);
    }
    
    return 0;
}

void arq_handle_ack(ArqContext *ctx, uint16_t ack_num) {
    uint16_t mod = ctx->window_size * 2;
    
    /* ACK is cumulative up to ack_num (exclusive) */
    while (ctx->sender.snd_base != ack_num) {
        if (ctx->sender.sent_buffer[ctx->sender.snd_base]) {
            ctx->sender.sent_buffer[ctx->sender.snd_base] = false;
            
            /* Update RTT estimate (Jacobson/Karels algorithm) */
            uint32_t sample_rtt = ctx->get_time_ms() - ctx->sender.sent_time[ctx->sender.snd_base];
            int32_t err = (int32_t)sample_rtt - (int32_t)ctx->sender.estimated_rtt_ms;
            ctx->sender.estimated_rtt_ms += err / 8;
            if (ctx->sender.estimated_rtt_ms < 1) ctx->sender.estimated_rtt_ms = 1;
            ctx->sender.dev_rtt_ms += (abs(err) - ctx->sender.dev_rtt_ms) / 4;
            ctx->timeout_ms = ctx->sender.estimated_rtt_ms + 4 * ctx->sender.dev_rtt_ms;
            if (ctx->timeout_ms < ARQ_DEFAULT_TIMEOUT_MS) ctx->timeout_ms = ARQ_DEFAULT_TIMEOUT_MS;
        }
        ctx->sender.snd_base = mod_seq(ctx->sender.snd_base + 1, mod);
    }
}

void arq_handle_nak(ArqContext *ctx, uint16_t nak_num) {
    uint16_t mod = ctx->window_size * 2;
    
    if (arq_in_window(nak_num, ctx->sender.snd_base, ctx->window_size, mod) &&
        ctx->sender.sent_buffer[nak_num]) {
        /* Immediate retransmission on NAK */
        ctx->sender.retry_count[nak_num] = 0;  /* Reset retry for fast retransmit */
        ctx->sender.sent_time[nak_num] = 0;    /* Force timeout on next check */
    }
}

void arq_handle_intra_req(ArqContext *ctx, uint16_t base_seq) {
    /* For delta sync: receiver requests full state from base_seq */
    /* This would trigger a full state resend in delta compression layer */
    (void)ctx;
    (void)base_seq;
}

/* Check for timeouts and retransmit */
void arq_timeout_check(ArqContext *ctx) {
    uint32_t now = ctx->get_time_ms();
    uint16_t mod = ctx->window_size * 2;
    uint16_t seq = ctx->sender.snd_base;
    
    while (seq != ctx->sender.snd_next) {
        if (ctx->sender.sent_buffer[seq]) {
            uint32_t elapsed = now - ctx->sender.sent_time[seq];
            if (elapsed >= ctx->timeout_ms) {
                /* Will be retransmitted on next arq_get_next_frame() */
                return;
            }
        }
        seq = mod_seq(seq + 1, mod);
    }
}

/* Receiver: process incoming frame */
int arq_receive(ArqContext *ctx, const ArqFrame *frame) {
    uint16_t mod = ctx->window_size * 2;
    
    if (frame->type == ARQ_FRAME_DATA) {
        uint16_t seq = frame->seq_num;
        uint16_t payload_len = frame->payload_len;
        
        if (!arq_in_window(seq, ctx->receiver.rcv_base, ctx->window_size, mod)) {
            /* Out of window - drop */
            return -1;
        }
        
        if (ctx->receiver.recv_buffer[seq]) {
            /* Duplicate - drop but send ACK */
            return -2;
        }
        
        /* Store packet */
        if (payload_len > 256) payload_len = 256;
        memcpy(ctx->receiver.recv_data[seq], frame->payload, payload_len);
        ctx->receiver.recv_len[seq] = payload_len;
        ctx->receiver.recv_buffer[seq] = true;
        
        /* Advance rcv_next for in-order packets */
        while (ctx->receiver.recv_buffer[ctx->receiver.rcv_next]) {
            ctx->receiver.rcv_next = mod_seq(ctx->receiver.rcv_next + 1, mod);
        }
        
        return 0;
    }
    
    return -3;  /* Not a data frame */
}

bool arq_has_delivered(const ArqContext *ctx) {
    (void)ctx->window_size;
    return ctx->receiver.recv_buffer[ctx->receiver.rcv_base];
}

int arq_get_delivered(ArqContext *ctx, uint8_t *out, uint16_t max_len) {
    uint16_t mod = ctx->window_size * 2;
    uint16_t base = ctx->receiver.rcv_base;
    
    if (!ctx->receiver.recv_buffer[base]) return -1;
    
    uint16_t len = ctx->receiver.recv_len[base];
    if (len > max_len) len = max_len;
    memcpy(out, ctx->receiver.recv_data[base], len);
    
    ctx->receiver.recv_buffer[base] = false;
    ctx->receiver.rcv_base = mod_seq(base + 1, mod);
    ctx->receiver.rcv_next = ctx->receiver.rcv_base;
    
    return len;
}

/* Generate ACK frame. Returns 1 on success. */
int arq_gen_ack(ArqContext *ctx, uint16_t seq_num, ArqFrame *out_frame) {
    uint16_t mod = ctx->window_size * 2;
    
    /* ACK acknowledges up to seq_num (exclusive) */
    uint16_t ack_num = mod_seq(seq_num + 1, mod);
    
    out_frame->seq_num = 0;
    out_frame->ack_num = ack_num;
    out_frame->type = ARQ_FRAME_ACK;
    out_frame->payload_len = 0;
    out_frame->timestamp_ms = ctx->get_time_ms();
    
    ctx->receiver.last_ack_time_ms = out_frame->timestamp_ms;
    return 1;
}

/* Generate NAK frame. Returns 1 on success. */
int arq_gen_nak(ArqContext *ctx, uint16_t seq_num, ArqFrame *out_frame) {
    out_frame->seq_num = 0;
    out_frame->ack_num = seq_num;
    out_frame->type = ARQ_FRAME_NAK;
    out_frame->payload_len = 0;
    out_frame->timestamp_ms = ctx->get_time_ms();
    
    return 1;
}

/* Generate Intra-frame request (for delta sync). Returns 1 on success. */
int arq_gen_intra_req(ArqContext *ctx, uint16_t base_seq, ArqFrame *out_frame) {
    out_frame->seq_num = 0;
    out_frame->ack_num = base_seq;
    out_frame->type = ARQ_FRAME_INTRA_REQ;
    out_frame->payload_len = 0;
    out_frame->timestamp_ms = ctx->get_time_ms();
    
    return 1;
}

void arq_get_stats(const ArqContext *ctx, ArqStats *stats) {
    memset(stats, 0, sizeof(ArqStats));
    (void)ctx;
}

void arq_reset_stats(ArqContext *ctx) {
    (void)ctx;
}