#ifndef ARQ_H
#define ARQ_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/* ARQ Selective Repeat - conforme nota arq-retransmissao-automatica.md:
 * - Janela N (configuravel), ACK individual por pacote
 * - Numeros de sequencia: 2N (evita ambiguidade novo/velho)
 * - Timer > RTT estimado; NAK acelera deteccao
 * - Transmissor reenvia so o perdido; receptor bufferiza fora de ordem */

#define ARQ_MAX_WINDOW       32
#define ARQ_MAX_SEQ          (ARQ_MAX_WINDOW * 2)
#define ARQ_DEFAULT_TIMEOUT_MS  500
#define ARQ_MAX_RETRIES         7

typedef enum {
    ARQ_FRAME_DATA = 0,
    ARQ_FRAME_ACK  = 1,
    ARQ_FRAME_NAK  = 2,
    ARQ_FRAME_INTRA_REQ = 3  /* Request for full state (delta sync) */
} ArqFrameType;

typedef struct {
    uint16_t seq_num;         /* Sequence number (mod 2N) */
    uint16_t ack_num;         /* For ACK/NAK: next expected sequence */
    ArqFrameType type;
    uint16_t payload_len;
    uint8_t  payload[256];    /* Max payload */
    uint32_t timestamp_ms;    /* For RTT estimation */
} ArqFrame;

/* Receiver state */
typedef struct {
    uint16_t window_size;
    uint16_t rcv_base;        /* Base of receive window */
    uint16_t rcv_next;        /* Next expected sequence */
    bool     recv_buffer[ARQ_MAX_SEQ];  /* Received packets */
    uint8_t  recv_data[ARQ_MAX_SEQ][256];
    uint16_t recv_len[ARQ_MAX_SEQ];
    uint64_t last_ack_time_ms;
} ArqReceiver;

/* Sender state */
typedef struct {
    uint16_t window_size;
    uint16_t snd_base;        /* Oldest unacked */
    uint16_t snd_next;        /* Next to send */
    bool     sent_buffer[ARQ_MAX_SEQ];
    uint8_t  sent_data[ARQ_MAX_SEQ][256];
    uint16_t sent_len[ARQ_MAX_SEQ];
    uint32_t sent_time[ARQ_MAX_SEQ];
    uint8_t  retry_count[ARQ_MAX_SEQ];
    uint32_t estimated_rtt_ms;
    uint32_t dev_rtt_ms;
    uint64_t last_send_time_ms;
} ArqSender;

/* Combined ARQ context */
typedef struct {
    ArqSender   sender;
    ArqReceiver receiver;
    uint16_t    window_size;
    uint32_t    timeout_ms;
    uint32_t    (*get_time_ms)(void);
} ArqContext;

/* Initialize ARQ context */
void arq_init(ArqContext *ctx, uint16_t window_size, uint32_t timeout_ms, 
              uint32_t (*get_time_ms)(void));

/* Sender API */
int  arq_send(ArqContext *ctx, const uint8_t *data, uint16_t len);
bool arq_has_pending_send(const ArqContext *ctx);
int  arq_get_next_frame(ArqContext *ctx, ArqFrame *out_frame);  /* Fill out_frame, return 1 if frame ready */
void arq_handle_ack(ArqContext *ctx, uint16_t ack_num);
void arq_handle_nak(ArqContext *ctx, uint16_t nak_num);
void arq_handle_intra_req(ArqContext *ctx, uint16_t base_seq);
void arq_timeout_check(ArqContext *ctx);  /* Call periodically */

/* Receiver API */
int  arq_receive(ArqContext *ctx, const ArqFrame *frame);
bool arq_has_delivered(const ArqContext *ctx);
int  arq_get_delivered(ArqContext *ctx, uint8_t *out, uint16_t max_len);
int  arq_gen_ack(ArqContext *ctx, uint16_t seq_num, ArqFrame *out_frame);
int  arq_gen_nak(ArqContext *ctx, uint16_t seq_num, ArqFrame *out_frame);
int  arq_gen_intra_req(ArqContext *ctx, uint16_t base_seq, ArqFrame *out_frame);

/* Statistics */
typedef struct {
    uint64_t frames_sent;
    uint64_t frames_retransmitted;
    uint64_t acks_sent;
    uint64_t naks_sent;
    uint64_t intra_reqs_sent;
    uint64_t frames_received;
    uint64_t frames_delivered;
    uint64_t frames_dropped_dup;
    uint64_t frames_dropped_oow;  /* Out of window */
    uint64_t timeouts;
} ArqStats;

void arq_get_stats(const ArqContext *ctx, ArqStats *stats);
void arq_reset_stats(ArqContext *ctx);

/* Utility */
uint16_t arq_seq_add(uint16_t a, uint16_t b, uint16_t mod);
bool     arq_seq_lt(uint16_t a, uint16_t b, uint16_t base, uint16_t mod);
bool     arq_in_window(uint16_t seq, uint16_t base, uint16_t window_size, uint16_t mod);

#endif