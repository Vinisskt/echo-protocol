#include "../include/echo_protocol.h"
#include "../include/log.h"
#include <stdint.h>
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

    pre_calc_fsk(&echo->mod_state);

    uint16_t freqs[4] = {FREQ_00, FREQ_01, FREQ_10, FREQ_11};
    for (int i = 0; i < 4; i++) {
        pre_calc_goertzel(&echo->freq_states[i], &freqs[i]);
    }

    echo->rx.state = SEARCHING;
    echo->rx.sync_accumulator = 0;
    echo->rx.rx_sample_count = 0;
    echo->rx.bits_received = 0;
    echo->rx.packet_len = 0;
    echo->rx.header_accumulator = 0;
    echo->rx.last_rx_time = 0;
    atomic_store(&echo->rx.packet_ready, 0);
    echo->rx.is_compressed = 0;
    echo->rx.is_rohc = 0;

    rohc_init(&echo->rohc_tx);
    rohc_init(&echo->rohc_rx);

    scrambler_init(&echo->tx_scrambler);
    scrambler_init(&echo->rx_scrambler);

    echo->tx.tx_sample_count = SAMPLES_PER_SYMBOL; 

    memset(&echo->stats, 0, sizeof(echo->stats));

    return 0;
}

void tun_to_rb(EchoProtocol *echo) {
    uint8_t raw_buf[SIZE_BUF];
    uint8_t rohc_buf[ROHC_MAX_COMPRESSED];
    uint8_t lz4_buf[SIZE_BUF + 64];

    int nread = tun_read(echo->tun_fd, raw_buf, sizeof(raw_buf));
    if (nread <= 0) return;

    uint8_t *final_ptr = raw_buf;
    uint16_t final_len = (uint16_t)nread;
    uint8_t comp_flag = 0;
    uint8_t rohc_flag = 0;

    int rohc_result = rohc_compress(&echo->rohc_tx, raw_buf, nread, rohc_buf, sizeof(rohc_buf));
    if (rohc_result > 0 && rohc_result < nread) {
        final_ptr = rohc_buf;
        final_len = (uint16_t)rohc_result;
        rohc_flag = 1;
    }

    if (rohc_result < 0) {
        int lz4_size = LZ4_compress_default((const char*)raw_buf, (char*)lz4_buf, nread, sizeof(lz4_buf));
        if (lz4_size > 0 && lz4_size < nread) {
            final_ptr = lz4_buf;
            final_len = (uint16_t)lz4_size;
            comp_flag = 1;
        }
    }

    log_debug("TX | tun=%d | air=%d | rohc=%d | lz4=%d", nread, final_len, rohc_flag, comp_flag);

    echo->stats.tx_packets++;
    echo->stats.tx_bytes += final_len;

    /* Force raw packet every N packets to resync ROHC context on RX (U-mode) */
    static int tx_raw_counter = 0;
    if (++tx_raw_counter >= 10) {
        tx_raw_counter = 0;
        comp_flag = 0;
        rohc_flag = 0;
        final_ptr = raw_buf;
        final_len = (uint16_t)nread;
        log_debug("TX force raw for ROHC resync");
    }

    scrambler_reset(&echo->tx_scrambler);

    uint16_t header = (comp_flag << 15) | (rohc_flag << 14) | (final_len & 0x3FFF);
    for (int i = 15; i >= 0; i--) {
        uint8_t bit = (header >> i) & 1;
        bit = scrambler_process(&echo->tx_scrambler, bit);
        if (!put_bits(echo->tx_rb, &bit)) {
            log_warn("TX buffer full during header, dropping packet");
            return;
        }
    }

    int total_bits = final_len * SIZE_BYTE;
    for (int i = 0; i < total_bits; i++) {
        uint8_t bit = (final_ptr[i >> 3] >> (7 - (i & 7))) & 1;
        bit = scrambler_process(&echo->tx_scrambler, bit);
        if (!put_bits(echo->tx_rb, &bit)) {
            log_warn("TX buffer full during payload, dropping packet");
            return;
        }
    }
}

void rb_to_audio(EchoProtocol *echo, uint8_t *symbol) {
    generate_fsk(&echo->mod_state, symbol);
}

static void handle_data_state(EchoProtocol *echo, uint8_t bit) {
    // Descrambling (self-synchronizing, recovers after ~17 bits)
    bit = scrambler_process(&echo->rx_scrambler, bit);

    put_bits(echo->rx_rb, &bit);
    echo->rx.bits_received++;

    if (echo->rx.bits_received > 16) {
        uint32_t payload_bits = (uint32_t)echo->rx.packet_len * 8;
        if (echo->rx.bits_received == (payload_bits + 16)) {
            echo->rx.state = SEARCHING;
            atomic_store(&echo->rx.packet_ready, 1);
        }
        return;
    }

    echo->rx.header_accumulator = (echo->rx.header_accumulator << 1) | bit;
    if (echo->rx.bits_received != 16) return;

    uint16_t header = (uint16_t)(echo->rx.header_accumulator & 0xFFFF);
    echo->rx.is_compressed = (header >> 15) & 1;
    echo->rx.is_rohc = (header >> 14) & 1;
    echo->rx.packet_len = header & 0x3FFF;
}

static void process_rx_bit(EchoProtocol *echo, uint8_t bit) {
    time_t now = time(NULL);

    if (echo->rx.state == DATA && now - echo->rx.last_rx_time > 6) {
        echo->rx.state = SEARCHING;
        echo->rx.bits_received = 0;
        echo->stats.rx_timeouts++;
        log_info("RX timeout | state=reset");
        return;
    }

    if (echo->rx.state == SEARCHING) {
        if (check_sync_word(&echo->rx.sync_accumulator, &bit) != 0) return;

        echo->rx.state = DATA;
        echo->rx.bits_received = 0;
        echo->rx.packet_len = 0;
        echo->rx.header_accumulator = 0;
        echo->rx.last_rx_time = now;
        scrambler_reset(&echo->rx_scrambler);  // New frame = resync scrambler
        echo->stats.rx_sync_found++;
        log_info("RX sync=ok");
        return;
    }

    echo->rx.last_rx_time = now;
    handle_data_state(echo, bit);
}

void audio_to_rb(EchoProtocol *echo, float *sample) {
    float mag[4];
    for (int i = 0; i < 4; i++) {
        mag[i] = process_goertzel(&echo->freq_states[i], sample);
    }

    if (++echo->rx.rx_sample_count < SAMPLES_PER_SYMBOL) {
        return;
    }

    int max_idx = 0;
    for (int i = 1; i < 4; i++) {
        if (mag[i] > mag[max_idx]) max_idx = i;
    }

    uint8_t bits[2];
    bits[0] = (max_idx >> 1) & 1;
    bits[1] = max_idx & 1;

    for (int i = 0; i < 4; i++) {
        reset_state(&echo->freq_states[i]);
    }
    echo->rx.rx_sample_count = 0;

    process_rx_bit(echo, bits[0]);
    process_rx_bit(echo, bits[1]);
}

static void rx_reset(EchoProtocol *echo) {
    echo->rx.state = SEARCHING;
    echo->rx.bits_received = 0;
    echo->rx.packet_len = 0;
    echo->rx.header_accumulator = 0;
    echo->rx.sync_accumulator = 0;
    echo->rx.is_compressed = 0;
    echo->rx.is_rohc = 0;
    rb_reset(echo->rx_rb);
    scrambler_reset(&echo->rx_scrambler);
}

void rb_to_tun(EchoProtocol *echo, int *packet_len) {
    uint8_t audio_payload[SIZE_BUF];
    uint8_t final_ip_packet[SIZE_BUF * 2];
    memset(audio_payload, 0, sizeof(audio_payload));

    uint8_t bit;
    for(int i=0; i<16; i++) {
        if (get_bits(echo->rx_rb, &bit) == 0) {
            rx_reset(echo);
            return;
        }
    }

    if (*packet_len <= 0 || *packet_len > SIZE_BUF) {
        echo->stats.rx_corrupted++;
        log_warn("RX corrupt | reason=bad_len | air=%d | total=%llu", *packet_len,
                 (unsigned long long)echo->stats.rx_corrupted);
        rx_reset(echo);
        return;
    }

    int total_bits = (*packet_len) * SIZE_BYTE;
    for (int i = 0; i < total_bits; i++) {
        if (get_bits(echo->rx_rb, &bit) == 0) {
            rx_reset(echo);
            return;
        }
        audio_payload[i >> 3] = (audio_payload[i >> 3] << 1) | bit;
    }

    int out_size = 0;

    if (echo->rx.is_rohc) {
        out_size = rohc_decompress(&echo->rohc_rx, audio_payload, *packet_len, final_ip_packet, sizeof(final_ip_packet));
        if (out_size <= 0) {
            rohc_sync_context(&echo->rohc_rx, audio_payload, *packet_len);
            out_size = rohc_decompress(&echo->rohc_rx, audio_payload, *packet_len, final_ip_packet, sizeof(final_ip_packet));
            if (out_size <= 0) {
                echo->stats.rx_corrupted++;
                log_warn("RX corrupt | reason=rohc_fail | air=%d | total=%llu", *packet_len,
                         (unsigned long long)echo->stats.rx_corrupted);
                rx_reset(echo);
                return;
            }
        }
    }

    if (!echo->rx.is_rohc && echo->rx.is_compressed) {
        out_size = LZ4_decompress_safe((const char*)audio_payload, (char*)final_ip_packet, *packet_len, sizeof(final_ip_packet));
        if (out_size < 0) {
            echo->stats.rx_corrupted++;
            log_warn("RX corrupt | reason=lz4_fail | air=%d | total=%llu", *packet_len,
                     (unsigned long long)echo->stats.rx_corrupted);
            rx_reset(echo);
            return;
        }
    }

    if (!echo->rx.is_rohc && !echo->rx.is_compressed) {
        memcpy(final_ip_packet, audio_payload, *packet_len);
        out_size = *packet_len;
        rohc_sync_context(&echo->rohc_rx, audio_payload, *packet_len);
    }

    if (out_size >= 20 && (final_ip_packet[0] & 0xF0) == 0x40) {
        int ihl = (final_ip_packet[0] & 0x0F) * 4;
        uint16_t cksum = ip_checksum(final_ip_packet, ihl);
        if (cksum != 0) {
            echo->stats.rx_corrupted++;
            log_warn("RX corrupt | reason=ip_cksum | air=%d | total=%llu", *packet_len,
                     (unsigned long long)echo->stats.rx_corrupted);
            rx_reset(echo);
            return;
        }
    }

    if (out_size > 0) {
        tun_write(echo->tun_fd, final_ip_packet, out_size);
        echo->stats.rx_packets++;
        echo->stats.rx_bytes += out_size;
        log_debug("RX OK | tun=%d | air=%d | rohc=%d | lz4=%d", out_size, *packet_len,
                  echo->rx.is_rohc, echo->rx.is_compressed);
    }
    rx_reset(echo);
}

void echo_close(EchoProtocol *echo) {
    close(echo->tun_fd);
    free(echo->tx_rb);
    free(echo->rx_rb);
}