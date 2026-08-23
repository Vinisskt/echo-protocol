#ifndef DELTA_H
#define DELTA_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/* Delta Compression - Mosh/SSP style state synchronization
 * Conforme ssh-sobre-enlace-acustico.md e mac-adaptativo-aprendizado.md:
 * - Estado numerado: cada frame tem seq_num + base_seq (ultimo confirmado)
 * - Delta payload: bitmask de mudancas desde base_seq
 * - Intra-frame request: se receptor detecta gap > threshold, pede REQ_INTRA(base_seq)
 * - Idempotencia: retransmissao nao duplica efeito (como AES-128-OCB3 do SSP)
 * 
 * Modelo de jogo: diff logico entre estados numerados (studyraid "Delta Compression for State Syncing") */

#define DELTA_MAX_STATE_SIZE    1024
#define DELTA_MAX_HISTORY       64
#define DELTA_MAX_DELTA_SIZE    512
#define DELTA_INTRA_THRESHOLD   10  /* Max gap before requesting intra-frame */

typedef struct {
    uint16_t seq_num;           /* Absolute sequence number */
    uint16_t base_seq;          /* Base sequence this delta is relative to */
    uint16_t state_len;         /* Current state length */
    uint8_t  state[DELTA_MAX_STATE_SIZE];  /* Full state (for intra-frame) */
    uint8_t  delta[DELTA_MAX_DELTA_SIZE];  /* Delta payload */
    uint16_t delta_len;         /* Delta length */
    uint32_t timestamp_ms;
    bool     is_intra;          /* True = full state (intra-frame) */
} DeltaFrame;

/* State history entry */
typedef struct {
    uint16_t seq_num;
    uint16_t state_len;
    uint8_t  state[DELTA_MAX_STATE_SIZE];
    uint32_t timestamp_ms;
    bool     valid;
} DeltaStateEntry;

/* Delta compressor (sender side) */
typedef struct {
    DeltaStateEntry history[DELTA_MAX_HISTORY];
    uint16_t        next_seq;
    uint16_t        acked_base;      /* Base sequence acknowledged by peer */
    uint16_t        history_head;    /* Circular buffer head */
    uint16_t        history_count;   /* Valid entries in history */
    uint32_t        (*get_time_ms)(void);
} DeltaCompressor;

/* Delta decompressor (receiver side) */
typedef struct {
    DeltaStateEntry history[DELTA_MAX_HISTORY];
    uint16_t        expected_seq;    /* Next expected sequence */
    uint16_t        last_acked;      /* Last acknowledged sequence */
    uint16_t        history_head;
    uint16_t        history_count;
    uint8_t         current_state[DELTA_MAX_STATE_SIZE];
    uint16_t        current_state_len;
    bool            state_valid;
    uint32_t        (*get_time_ms)(void);
    /* For intra-frame requests */
    bool            pending_intra_req;
    uint16_t        intra_req_base;
} DeltaDecompressor;

/* Combined delta context */
typedef struct {
    DeltaCompressor compressor;
    DeltaDecompressor decompressor;
    uint32_t (*get_time_ms)(void);
} DeltaContext;

/* Statistics */
typedef struct {
    uint64_t frames_sent;
    uint64_t deltas_sent;
    uint64_t intra_sent;
    uint64_t frames_received;
    uint64_t deltas_applied;
    uint64_t intra_received;
    uint64_t intra_requested;
    uint64_t gaps_detected;
    uint64_t bytes_saved;
} DeltaStats;

/* Initialize delta context */
void delta_init(DeltaContext *ctx, uint32_t (*get_time_ms)(void));

/* Compressor (sender) API */
int  delta_compress(DeltaContext *ctx, const uint8_t *new_state, uint16_t new_state_len, 
                    DeltaFrame *out_frame);
bool delta_has_pending(const DeltaContext *ctx);
void delta_handle_ack(DeltaContext *ctx, uint16_t acked_seq);
void delta_handle_intra_req(DeltaContext *ctx, uint16_t base_seq);

/* Decompressor (receiver) API */
int  delta_decompress(DeltaContext *ctx, const DeltaFrame *frame, 
                      uint8_t *out_state, uint16_t *out_len);
bool delta_needs_intra(const DeltaContext *ctx);
uint16_t delta_get_intra_base(const DeltaContext *ctx);

/* Statistics */
void delta_get_stats(const DeltaContext *ctx, DeltaStats *stats);
void delta_reset_stats(DeltaContext *ctx);

/* Utility: compute delta between two states (bitmask of changed bytes) */
int delta_compute(const uint8_t *old_state, uint16_t old_len,
                  const uint8_t *new_state, uint16_t new_len,
                  uint8_t *delta_out, uint16_t *delta_len);

/* Utility: apply delta to base state */
int delta_apply(const uint8_t *base_state, uint16_t base_len,
                const uint8_t *delta, uint16_t delta_len,
                uint8_t *out_state, uint16_t *out_len);

/* State prediction (echo prediction) - for local echo */
int delta_predict_echo(DeltaContext *ctx, uint8_t key, 
                       uint8_t *out_state, uint16_t *out_len);

#endif