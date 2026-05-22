#include "../include/echo_protocol.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <linux/if_tun.h>

int echo_init(EchoProtocol *echo, char *dev_name) {
    
    echo->tun_fd = tun_alloc(dev_name, IFF_TUN | IFF_NO_PI);
    if (echo->tun_fd < 0) {
        return -1;
    }

    echo->tx_rb = rb_init();
    echo->rx_rb = rb_init();
    if (check_rb(echo->rx_rb) || check_rb(echo->tx_rb)) {
        return -1;
    }

    pre_calc_afsk(&echo->mod_state);

    uint16_t freq_space = FREQ_SPACE;
    uint16_t freq_mark = FREQ_MARK;
    
    pre_calc_goertzel(&echo->space_state, &freq_space);
    pre_calc_goertzel(&echo->mark_state, &freq_mark);

    echo->sync_accumulator = 0;
    echo->rx_sample_count = 0;

    return 0;
}

void tun_to_rb(EchoProtocol *echo) {

    uint8_t buf[SIZE_BUF];

    int nread = tun_read(echo->tun_fd, buf, sizeof(buf));
    if (nread <= 0) {
        return;
    }

    int total_bits = nread * SIZE_BYTE;
    
    for (int i = 0; i < total_bits; i++) {
        uint8_t bit = (buf[i >> 3] >> (7 - (i & 7))) & 1;
        put_bits(echo->tx_rb, &bit);
    }

    return;
}

void rb_to_audio(EchoProtocol *echo, uint8_t *bit) {
    generate_afsk(&echo->mod_state, bit);
}

void audio_to_rb(EchoProtocol *echo, float *sample) {
    float mag_space = process_goertzel(&echo->space_state, sample);
    float mag_mark = process_goertzel(&echo->mark_state, sample);

    if (echo->rx_sample_count == SAMPLES_PER_BIT - 1) {
        uint8_t bit = (mag_mark > mag_space) ? 1 : 0;
        put_bits(echo->rx_rb, &bit);
    }

    return;
}

void rb_to_tun(EchoProtocol *echo, int *packet_len) {
    uint8_t buf[SIZE_BUF];

    int total_bits = (*packet_len) * SIZE_BYTE;
    uint8_t bit;

    for (int i = 0; i < total_bits; i++) {
        get_bits(echo->rx_rb, &bit);
        buf[i >> 3] = (buf[i >> 3] << 1) | bit;
    }

    tun_write(echo->tun_fd, buf, *packet_len);
    return;
}

void echo_close(EchoProtocol *echo) {
    close(echo->tun_fd);
    free(echo->tx_rb);
    free(echo->rx_rb);
}




