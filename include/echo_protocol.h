#ifndef ECHO_PROTOCOL_H
#define ECHO_PROTOCOL_H

#include "tun_tap.h"
#include "rb_bits.h"
#include "mod_fsk.h"
#include "demod_afsk.h"
#include "rohc.h"
#include <stdatomic.h>

#define SIZE_BUF 2048
#define SIZE_BYTE 8

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
} RxState;

typedef struct {
    uint64_t tx_packets;
    uint64_t tx_bytes;
    uint64_t rx_packets;
    uint64_t rx_bytes;
    uint64_t rx_corrupted;
    uint64_t rx_sync_found;
    uint64_t rx_timeouts;
} ProtocolStats;

typedef struct EchoProtocol_s {
    int tun_fd;
    Buffer *tx_rb;
    Buffer *rx_rb;
    StateFSK mod_state;
    StateGoertzel freq_states[4];
    RxState rx;
    TxState tx;
    ROHCState rohc_tx;
    ROHCState rohc_rx;
    ProtocolStats stats;
} EchoProtocol;

int echo_init(EchoProtocol *echo, char *dev_name);
void echo_close(EchoProtocol *echo);
void tun_to_rb(EchoProtocol *echo);
void rb_to_tun(EchoProtocol *echo, int *packet_len);
void rb_to_audio(EchoProtocol *echo, uint8_t *symbol);
void audio_to_rb(EchoProtocol *echo, float *sample);

#endif