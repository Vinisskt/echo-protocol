#ifndef ECHO_PROTOCOL_H
#define ECHO_PROTOCOL_H

#include "tun_tap.h"
#include "rb_bits.h"
#include "mod_fsk.h"
#include "demod_afsk.h"
#include "coherent_demod.h"
#include "rohc.h"
#include "scrambler.h"
#include "bpfilter.h"
#include <stdatomic.h>

/* Forward declare para evitar include circular com agc.h */
typedef struct AGCState AGCState;

#define SIZE_BUF 2048
#define SIZE_BYTE 8
#define MAX_TX_FRAME_BYTES 2048

typedef enum {
    SEARCHING,
    DATA
} RxStatus;

typedef struct {
    uint16_t tx_sample_count;
} TxState;

typedef struct {
    RxStatus state;
    uint32_t sync_accumulator;
    uint16_t rx_sample_count;
    uint16_t bits_received;
    uint16_t packet_len;
    uint64_t header_accumulator;
    uint32_t last_rx_time;
    atomic_uint_fast8_t packet_ready;
    uint8_t is_compressed;
    uint8_t is_rohc;
    uint8_t is_fec;
} RxState;

typedef struct {
    uint64_t tx_packets;
    uint64_t tx_bytes;
    uint64_t rx_packets;
    uint64_t rx_bytes;
    uint64_t rx_corrupted;
    uint64_t rx_sync_found;
    uint64_t rx_timeouts;
    uint64_t val_failures;
    uint64_t noise_rejects;
} ProtocolStats;

typedef struct EchoProtocol_s {
    int tun_fd;
    Buffer *tx_rb;
    Buffer *rx_rb;
    StateFSK mod_state;

    /* Set A: 4 filtros principais (32 amostras, curta janela) */
    StateGoertzel freq_states[4];

    /* Set B: 4 filtros validadores (64 amostras, longa janela) */
    StateGoertzelLong freq_valid[4];
    float long_window_buf[4][SAMPLES_LONG_WINDOW];
    int long_buf_idx[4];

    /* Set C: 3 monitores de banda (32 amostras, ruído broadband) */
    StateGoertzel freq_mon[NUM_FREQ_MON];

    /* Pipeline de decisão */
    int pending_candidate;           /* -1 = nenhum, 0..3 = idx aguardando validação */
    uint8_t pending_bits[2];         /* bits do candidato aguardando */
    int block_counter;               /* par/ímpar para timing do Set B */
    int long_samples_count;          /* amostras acumuladas na janela longa (0..64) */

    RxState rx;
    TxState tx;
    ROHCState rohc_tx;
    ROHCState rohc_rx;
    Scrambler tx_scrambler;
    Scrambler rx_scrambler;
    BandpassBank bp_bank;    /* 4 filtros passa-banda (um por tom FSK) */
    CoherentDemodulator coh_demod;  /* Demodulador coerente com PLL + timing recovery + matched filter */
    ProtocolStats stats;
    AGCState *agc;
} EchoProtocol;

int echo_init(EchoProtocol *echo, char *dev_name);
void echo_close(EchoProtocol *echo);
void tun_to_rb(EchoProtocol *echo);
void rb_to_tun(EchoProtocol *echo, int *packet_len);
void audio_to_rb(EchoProtocol *echo, float *sample);

#ifdef ECHO_PROTOCOL_TEST
/* Acesso a funções static para testes unitários (compilado só com -DECHO_PROTOCOL_TEST). */
void echo_test_handle_data_state(EchoProtocol *echo, uint8_t bit);
#endif

#endif