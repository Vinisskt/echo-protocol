#ifndef ECHO_PROTOCOL_H
#define ECHO_PROTOCOL_H

#include "tun_tap.h"
#include "rb_bits.h"
#include "mod_afsk.h"
#include "demod_afsk.h"

typedef struct {
    int tun_fd;
    Buffer *tx_rb;
    Buffer *rx_rb;
    StateAFSK mod_state;
    StateGoertzel space_state;
    StateGoertzel mark_state;
    uint32_t sync_accumulator;
} EchoProtocol;

int echo_init(EchoProtocol *echo, char *dev_name);
void echo_close(EchoProtocol *echo);
void tun_to_rb(EchoProtocol *echo);
float rb_to_audio(EchoProtocol *echo);
void audio_to_rb(EchoProtocol *echo, float *sample);
void rb_to_tun(EchoProtocol *echo);

#endif
