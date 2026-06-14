#include "../include/echo_protocol.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <linux/if_tun.h>
#include <time.h>
#include <lz4.h>

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
    echo->rx.last_rx_time = 0;
    atomic_store(&echo->rx.packet_ready, 0);
    echo->rx.is_compressed = 0;

    echo->tx.tx_sample_count = SAMPLES_PER_BIT; 

    return 0;
}

void tun_to_rb(EchoProtocol *echo) {
    uint8_t raw_buf[SIZE_BUF];
    uint8_t comp_buf[SIZE_BUF + 64];

    int nread = tun_read(echo->tun_fd, raw_buf, sizeof(raw_buf));
    if (nread <= 0) return;

    int comp_size = LZ4_compress_default((const char*)raw_buf, (char*)comp_buf, nread, sizeof(comp_buf));
    
    uint8_t *final_ptr;
    uint16_t final_len;
    uint8_t comp_flag;

    if (comp_size > 0 && comp_size < nread) {
        final_ptr = comp_buf;
        final_len = (uint16_t)comp_size;
        comp_flag = 1;
    } else {
        final_ptr = raw_buf;
        final_len = (uint16_t)nread;
        comp_flag = 0;
    }

    printf("[TX] Pacote TUN: %d bytes -> Áudio: %d bytes (Comp: %d)\n", nread, final_len, comp_flag);

    uint16_t header = (comp_flag << 15) | (final_len & 0x7FFF);
    for (int i = 15; i >= 0; i--) {
        uint8_t bit = (header >> i) & 1;
        put_bits(echo->tx_rb, &bit);
    }

    int total_bits = final_len * SIZE_BYTE;
    for (int i = 0; i < total_bits; i++) {
        uint8_t bit = (final_ptr[i >> 3] >> (7 - (i & 7))) & 1;
        put_bits(echo->tx_rb, &bit);
    }
}

void rb_to_audio(EchoProtocol *echo, uint8_t *bit) {
    generate_afsk(&echo->mod_state, bit);
}

static void handle_data_state(EchoProtocol *echo, uint8_t bit) {
    put_bits(echo->rx_rb, &bit);
    echo->rx.bits_received++;

    if (echo->rx.bits_received <= 16) {
        echo->rx.header_accumulator = (echo->rx.header_accumulator << 1) | bit;
        
        if (echo->rx.bits_received == 16) {
            uint16_t header = (uint16_t)(echo->rx.header_accumulator & 0xFFFF);
            echo->rx.is_compressed = (header >> 15) & 1;
            echo->rx.packet_len = header & 0x7FFF;
        }
        return;
    }

    uint32_t payload_bits = (uint32_t)echo->rx.packet_len * 8;
    if (echo->rx.bits_received == (payload_bits + 16)) {
        echo->rx.state = SEARCHING;
        atomic_store(&echo->rx.packet_ready, 1);
    }
}

static void process_rx_bit(EchoProtocol *echo, uint8_t bit) {
    time_t now = time(NULL);

    if (echo->rx.state == DATA) {
        if (now - echo->rx.last_rx_time > 6) {
            echo->rx.state = SEARCHING;
            echo->rx.bits_received = 0;
            printf("[RX] Timeout de recepção! Resetando...\n");
            return;
        }
    }

    if (echo->rx.state == SEARCHING) {
        if (check_sync_word(&echo->rx.sync_accumulator, &bit) == 0) {
            echo->rx.state = DATA;
            echo->rx.bits_received = 0;
            echo->rx.packet_len = 0;
            echo->rx.header_accumulator = 0;
            echo->rx.last_rx_time = now;
            printf("[RX] Sync Word OK!\n");
        }
        return;
    }

    echo->rx.last_rx_time = now;
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
    uint8_t audio_payload[SIZE_BUF];
    uint8_t final_ip_packet[SIZE_BUF * 2]; // Buffer maior para descompressão
    memset(audio_payload, 0, sizeof(audio_payload));

    uint8_t bit;
    for(int i=0; i<16; i++) {
        if (get_bits(echo->rx_rb, &bit) == 0) return;
    }

    int total_bits = (*packet_len) * SIZE_BYTE;
    for (int i = 0; i < total_bits; i++) {
        if (get_bits(echo->rx_rb, &bit) == 0) return;
        audio_payload[i >> 3] = (audio_payload[i >> 3] << 1) | bit;
    }

    int out_size;
    if (echo->rx.is_compressed) {
        out_size = LZ4_decompress_safe((const char*)audio_payload, (char*)final_ip_packet, *packet_len, sizeof(final_ip_packet));
    } else {
        memcpy(final_ip_packet, audio_payload, *packet_len);
        out_size = *packet_len;
    }

    if (out_size > 0) {
        tun_write(echo->tun_fd, final_ip_packet, out_size);
        printf("[RX] Pacote entregue: %d bytes (era %d no áudio, Comp: %d)\n", out_size, *packet_len, echo->rx.is_compressed);
    }
}

void echo_close(EchoProtocol *echo) {
    close(echo->tun_fd);
    free(echo->tx_rb);
    free(echo->rx_rb);
}
