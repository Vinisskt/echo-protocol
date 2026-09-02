#include "../include/echo_protocol.h"
#include "../include/log.h"
#include "../include/fec.h"
#include "../include/agc.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <linux/if_tun.h>
#include <time.h>
#include <lz4.h>

static void rx_reset(EchoProtocol *echo);
static uint8_t is_valid_header(uint16_t header);
static uint8_t is_valid_fec_len(uint16_t len);

int echo_init(EchoProtocol *echo, char *dev_name) {
    
    echo->tun_fd = tun_alloc(dev_name, IFF_TUN | IFF_NO_PI);
    if (echo->tun_fd < 0) {
        return -1;
    }

    echo->tx_rb = rb_init();
    echo->rx_rb = rb_init();

    pre_calc_fsk(&echo->mod_state);

    /* Set A: filtros principais (32 amostras) */
    uint16_t freqs[4] = {FREQ_00, FREQ_01, FREQ_10, FREQ_11};
    for (int i = 0; i < 4; i++) {
        pre_calc_goertzel(&echo->freq_states[i], &freqs[i]);
    }

    /* Set B: filtros validadores (64 amostras, mesmas frequências) */
    for (int i = 0; i < 4; i++) {
        pre_calc_goertzel_long(&echo->freq_valid[i], &freqs[i]);
    }
    for (int i = 0; i < 4; i++) {
        echo->long_buf_idx[i] = 0;
        memset(echo->long_window_buf[i], 0, sizeof(echo->long_window_buf[i]));
    }

    /* Set C: monitores de banda (ruído broadband) */
    uint16_t mon_freqs[NUM_FREQ_MON] = {FREQ_MON_LOW, FREQ_MON_MID, FREQ_MON_HIGH};
    for (int i = 0; i < NUM_FREQ_MON; i++) {
        pre_calc_goertzel(&echo->freq_mon[i], &mon_freqs[i]);
    }

    /* Pipeline de decisão */
    echo->pending_candidate = -1;
    echo->block_counter = 0;
    echo->long_samples_count = 0;

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

    /* Banco de 4 filtros passa-banda (um por tom FSK) - largura ~500 Hz */
    bandpass_bank_init(&echo->bp_bank, SAMPLE_RATE, 2000.0f);

    echo->agc = calloc(1, sizeof(AGCState));
    if (!echo->agc) return -1;
    agc_init(echo->agc);

    echo->tx.tx_sample_count = SAMPLES_PER_SYMBOL; 

    memset(&echo->stats, 0, sizeof(echo->stats));

    return 0;
}

void tun_to_rb(EchoProtocol *echo) {
    uint8_t raw_buf[SIZE_BUF];
    uint8_t rohc_buf[ROHC_MAX_COMPRESSED];
    uint8_t lz4_buf[SIZE_BUF + 64];
    uint8_t fec_buf[SIZE_BUF * 2];  /* FEC adds overhead */

    int nread = tun_read(echo->tun_fd, raw_buf, sizeof(raw_buf));
    if (nread <= 0) return;

    uint8_t *final_ptr = raw_buf;
    uint16_t final_len = (uint16_t)nread;
    uint8_t comp_flag = 0;
    uint8_t rohc_flag = 0;
    uint8_t fec_flag = 0;

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

    /* FEC encoding: wrap compressed/raw payload with Reed-Solomon + interleaver */
    int fec_len = fec_encoded_len(final_len);
    if (fec_len > 0 && fec_len <= (int)sizeof(fec_buf)) {
        int fec_result = fec_encode(final_ptr, final_len, fec_buf, sizeof(fec_buf));
        if (fec_result > 0 && fec_result <= (int)sizeof(fec_buf)) {
            final_ptr = fec_buf;
            final_len = (uint16_t)fec_result;
            fec_flag = 1;
        }
    }

    log_debug("TX | tun=%d | air=%d | rohc=%d | lz4=%d | fec=%d", nread, final_len, rohc_flag, comp_flag, fec_flag);

    echo->stats.tx_packets++;
    echo->stats.tx_bytes += final_len;

    /* Force raw packet every N packets to resync ROHC context on RX (U-mode) */
    static int tx_raw_counter = 0;
    if (++tx_raw_counter >= 100) {
        tx_raw_counter = 0;
        comp_flag = 0;
        rohc_flag = 0;
        fec_flag = 0;
        final_ptr = raw_buf;
        final_len = (uint16_t)nread;
        log_debug("TX force raw for ROHC resync");
    }

    scrambler_reset(&echo->tx_scrambler);

    /* Header: bit 15=comp, 14=rohc, 13=fec, 12-0=len (max 8191) */
    uint16_t header = (comp_flag << 15) | (rohc_flag << 14) | (fec_flag << 13) | (final_len & 0x1FFF);
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
        } else if (echo->rx.bits_received > (payload_bits + 16)) {
            /* Comprimento declarado no header não bateu com o recebido
               (ex.: packet_len corrompido) -> descarta já, em vez de travar
               em DATA até o timeout de 6 s de process_rx_bit. */
            rx_reset(echo);
        }
        return;
    }

    echo->rx.header_accumulator = (echo->rx.header_accumulator << 1) | bit;
    if (echo->rx.bits_received != 16) return;

    uint16_t header = (uint16_t)(echo->rx.header_accumulator & 0xFFFF);
    echo->rx.is_compressed = (header >> 15) & 1;
    echo->rx.is_rohc = (header >> 14) & 1;
    echo->rx.is_fec = (header >> 13) & 1;
    echo->rx.packet_len = header & 0x1FFF;

    // Validate header before committing to DATA state
    if (!is_valid_header(header)) {
        log_warn("RX invalid header=0x%04X (comp=%u rohc=%u fec=%u len=%u) -> back to SEARCHING",
                 header, echo->rx.is_compressed, echo->rx.is_rohc, echo->rx.is_fec, echo->rx.packet_len);
        rx_reset(echo);
        return;
    }
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
    /* === FILTRAGEM: passa-banda por frequência FSK (remove ruído fora de banda) === */
    float filtered[4];
    bandpass_bank_process(&echo->bp_bank, *sample, filtered);

    /* === SET A: filtros principais (32 amostras, curta janela) === */
    float mag_a[4];
    for (int i = 0; i < 4; i++) {
        mag_a[i] = process_goertzel_windowed(&echo->freq_states[i], &filtered[i]);
    }

    /* === SET B: acumular em buffer de janela longa (64 amostras) === */
    for (int i = 0; i < 4; i++) {
        int idx = echo->long_buf_idx[i];
        echo->long_window_buf[i][idx] = filtered[i];
        echo->long_buf_idx[i] = idx + 1;
    }

    /* === DECISÃO: a cada 32 amostras (final de símbolo) === */
    echo->long_samples_count++;
    if (++echo->rx.rx_sample_count < SAMPLES_PER_SYMBOL) {
        return;
    }

    /* --- Set A: escolher candidato (max magnitude) --- */
    int max_idx = 0;
    for (int i = 1; i < 4; i++) {
        if (mag_a[i] > mag_a[max_idx]) max_idx = i;
    }

    uint8_t bits[2];
    bits[0] = (max_idx >> 1) & 1;
    bits[1] = max_idx & 1;

    /* --- Validar candidato ANTERIOR quando Set B tem 64 amostras --- */
    if (echo->long_samples_count >= SAMPLES_LONG_WINDOW) {
        /* Set B: processar buffer de 64 amostras com Goertzel longo (2x resolução) */
        float mag_b[4];
        for (int i = 0; i < 4; i++) {
            mag_b[i] = process_goertzel_buffer_long(&echo->freq_valid[i],
                       echo->long_window_buf[i], SAMPLES_LONG_WINDOW);
        }

        /* Set B decide independentemente (2x resolução de frequência) */
        int b_idx = 0;
        for (int i = 1; i < 4; i++) {
            if (mag_b[i] > mag_b[b_idx]) b_idx = i;
        }

        /* Comparar decisão do Set B com candidato pendente do Set A (símbolo anterior) */
        if (echo->pending_candidate >= 0 && b_idx != echo->pending_candidate) {
            /* Set B discorda: registrar falha de validação */
            echo->stats.val_failures++;
        }

        /* Limpar buffers de Set B e contador */
        for (int i = 0; i < 4; i++) {
            echo->long_buf_idx[i] = 0;
        }
        echo->long_samples_count = 0;
    }

    /* --- Guardar candidato para validação futura (depois da validação) --- */
    echo->pending_candidate = max_idx;
    echo->pending_bits[0] = bits[0];
    echo->pending_bits[1] = bits[1];

    /* --- Reset Set A para próximo símbolo --- */
    for (int i = 0; i < 4; i++) {
        reset_state(&echo->freq_states[i]);
    }
    echo->rx.rx_sample_count = 0;
    echo->block_counter++;

    /* --- Output: bits do Set A --- */
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
    echo->rx.is_fec = 0;
    rb_reset(echo->rx_rb);
    scrambler_reset(&echo->rx_scrambler);
    sync_correlator_reset();
    bandpass_bank_reset(&echo->bp_bank);
}

/* Check if a length is a valid FEC-encoded length */
static uint8_t is_valid_fec_len(uint16_t len) {
    if (len < 3) return 0;  /* Need at least 2 bytes len + 1 byte data */
    /* Single block: len = data_len + 2 + ecc, where data_len <= 253, ecc <= 32 */
    /* Max single block: 255 + 2 = 257, but with ecc: 253 + 2 + 32 = 287 */
    if (len <= FEC_RS_MAX_N + 2) {
        /* Could be single block - check if it matches any valid encoding */
        for (int data_len = 1; data_len <= FEC_RS_MAX_N; data_len++) {
            if (fec_encoded_len(data_len) == len) return 1;
        }
        return 0;
    }
    /* Multi-block: must be multiple of FEC_RS_MAX_N plus 2 */
    if ((len - 2) % FEC_RS_MAX_N == 0) {
        int nblocks = (len - 2) / FEC_RS_MAX_N;
        int data_len = nblocks * FEC_RS_MSGBLK;
        if (fec_encoded_len(data_len) == len) return 1;
    }
    return 0;
}

static uint8_t is_valid_header(uint16_t header) {
    uint8_t comp = (header >> 15) & 1;
    uint8_t rohc = (header >> 14) & 1;
    uint8_t fec = (header >> 13) & 1;
    uint16_t len = header & 0x1FFF;
    // comp and rohc are mutually exclusive (per TX logic)
    if (comp && rohc) return 0;
    // comp, rohc, fec cannot all be 1 simultaneously (reserved)
    if (comp && rohc && fec) return 0;
    // Length must be reasonable
    if (len == 0 || len > SIZE_BUF) return 0;
    // If FEC flag set, length must be valid FEC-encoded length
    if (fec && !is_valid_fec_len(len)) return 0;
    return 1;
}

void rb_to_tun(EchoProtocol *echo, int *packet_len) {
    uint8_t audio_payload[SIZE_BUF * 2];  /* FEC decoded can be larger */
    uint8_t final_ip_packet[SIZE_BUF * 2];
    memset(audio_payload, 0, sizeof(audio_payload));

    uint8_t bit;
    for(int i=0; i<16; i++) {
        if (get_bits(echo->rx_rb, &bit) == 0) {
            rx_reset(echo);
            return;
        }
    }

    if (*packet_len <= 0 || *packet_len > (int)sizeof(audio_payload)) {
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

    /* FEC decoding if flag set */
    uint8_t fec_decoded[SIZE_BUF * 2];
    if (echo->rx.is_fec) {
        int fec_result = fec_decode(audio_payload, *packet_len, fec_decoded, sizeof(fec_decoded));
        if (fec_result <= 0) {
            echo->stats.rx_corrupted++;
            log_warn("RX corrupt | reason=fec_fail | air=%d | total=%llu", *packet_len,
                     (unsigned long long)echo->stats.rx_corrupted);
            rx_reset(echo);
            return;
        }
        /* Copy decoded payload back to audio_payload for decompression */
        memcpy(audio_payload, fec_decoded, fec_result);
        *packet_len = fec_result;
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
        log_info("RX OK | tun=%d | air=%d | rohc=%d | lz4=%d", out_size, *packet_len,
                  echo->rx.is_rohc, echo->rx.is_compressed);
    }
    rx_reset(echo);
}

void echo_close(EchoProtocol *echo) {
    close(echo->tun_fd);
    free(echo->tx_rb);
    free(echo->rx_rb);
    free(echo->agc);
}

#ifdef ECHO_PROTOCOL_TEST
/* Expõe handle_data_state para testes unitários (não faz parte do binário normal). */
void echo_test_handle_data_state(EchoProtocol *echo, uint8_t bit) {
    handle_data_state(echo, bit);
}
#endif