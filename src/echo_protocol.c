#include "../include/echo_protocol.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <linux/if_tun.h>

int echo_init(EchoProtocol *echo, char *dev_name) {
    
    echo->tun_fd = tun_alloc(dev_name, IFF_TUN | IFF_NO_PI);
    if (echo->tun_fd < 0) {
        return -1;
    }

    echo->tx_rb = rb_init();
    echo->rx_rb = rb_init();

    pre_calc_afsk(&echo->mod_state);

    uint16_t freq_space = FREQ_SPACE;
    uint16_t freq_mark = FREQ_MARK;
    
    pre_calc_goertzel(&echo->space_state, &freq_space);
    pre_calc_goertzel(&echo->mark_state, &freq_mark);

    echo->rx.state = SEARCHING;
    echo->rx.sync_accumulator = 0;
    echo->rx.rx_sample_count = 0;
    echo->rx.bits_received = 0;
    echo->rx.packet_len = 0;
    echo->rx.header_accumulator = 0;

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

static uint16_t extract_ip_len(uint64_t header, uint16_t bits) {
    if (bits < 32) return 0;

    uint8_t version = (header >> (bits - 4)) & 0xF;

    if (version == 4) {
        return (header >> (bits - 32)) & 0xFFFF;
    }

    if (version == 6 && bits >= 48) {
        return ((header >> (bits - 48)) & 0xFFFF) + 40;
    }

    return 0;
}

static void handle_data_state(EchoProtocol *echo, uint8_t bit) {
    put_bits(echo->rx_rb, &bit);
    echo->rx.bits_received++;

    if (echo->rx.bits_received <= 64) {
        echo->rx.header_accumulator = (echo->rx.header_accumulator << 1) | bit;
    }

    if (echo->rx.packet_len == 0) {
        echo->rx.packet_len = extract_ip_len(echo->rx.header_accumulator, echo->rx.bits_received);
    }

    uint32_t total_bits = (uint32_t)echo->rx.packet_len * 8;
    if (echo->rx.packet_len > 0 && echo->rx.bits_received == total_bits) {
        echo->rx.state = SEARCHING;
    }
}

static void process_rx_bit(EchoProtocol *echo, uint8_t bit) {
    if (echo->rx.state == SEARCHING) {
        if (check_sync_word(&echo->rx.sync_accumulator, &bit) == 0) {
            echo->rx.state = DATA;
            echo->rx.bits_received = 0;
            echo->rx.packet_len = 0;
            echo->rx.header_accumulator = 0;
        }
        return;
    }

    handle_data_state(echo, bit);
}

void audio_to_rb(EchoProtocol *echo, float *sample) {
    float mag_space = process_goertzel(&echo->space_state, sample);
    float mag_mark = process_goertzel(&echo->mark_state, sample);

    if (++echo->rx.rx_sample_count < SAMPLES_PER_BIT) {
        return;
    }

    uint8_t bit = (mag_mark > mag_space) ? 1 : 0;

    reset_state(&echo->space_state);
    reset_state(&echo->mark_state);
    echo->rx.rx_sample_count = 0;

    process_rx_bit(echo, bit);
}

void rb_to_tun(EchoProtocol *echo, int *packet_len) {
    uint8_t buf[SIZE_BUF];
    memset(buf, 0, sizeof(buf));

    int total_bits = (*packet_len) * SIZE_BYTE;
    uint8_t bit;

    for (int i = 0; i < total_bits; i++) {
        if (get_bits(echo->rx_rb, &bit) == 0) {
            // Abortar se o buffer esvaziar prematuramente
            return;
        }
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




