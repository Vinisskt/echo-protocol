#ifndef ECHO_PROTOCOL_H
#define ECHO_PROTOCOL_H

#include "tun_tap.h"
#include "rb_bits.h"
#include "mod_afsk.h"
#include "demod_afsk.h"
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
} RxState;

typedef struct EchoProtocol_s {
    int tun_fd;
    Buffer *tx_rb;
    Buffer *rx_rb;
    StateAFSK mod_state;
    StateGoertzel space_state;
    StateGoertzel mark_state;
    RxState rx;
    TxState tx;
} EchoProtocol;

int echo_init(EchoProtocol *echo, char *dev_name);
void echo_close(EchoProtocol *echo);
void tun_to_rb(EchoProtocol *echo);
void rb_to_tun(EchoProtocol *echo, int *packet_len);
void rb_to_audio(EchoProtocol *echo, uint8_t *bit);
void audio_to_rb(EchoProtocol *echo, float *sample);

#endif
