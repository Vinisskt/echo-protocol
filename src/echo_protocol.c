#include "../include/echo_protocol.h"
#include "../include/log.h"
#include "../include/fec.h"
#include "../include/arq.h"
#include "../include/delta.h"
#include "../include/mac.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <linux/if_tun.h>
#include <time.h>
#include <lz4.h>

#define HEADER_BITS 16
#define ROHC_RESYNC_INTERVAL 100

/* ── Forward declarations (internal) ────────────────────────────────── */
static void     rx_reset(EchoProtocol *echo);
static uint8_t  is_valid_header(uint16_t header);
static uint8_t  is_valid_fec_len(uint16_t len);
static void     rx_decode_header(uint16_t header, uint8_t *comp, uint8_t *rohc,
                                 uint8_t *fec, uint16_t *len);
static int      tx_push_bit(EchoProtocol *echo, uint8_t bit, const char *where);

/* ── TX helpers ─────────────────────────────────────────────────────── */
static int      tx_compress(const uint8_t *raw, int raw_len,
                            EchoProtocol *echo,
                            uint8_t *out, uint16_t *out_len,
                            uint8_t *rohc_flag, uint8_t *comp_flag);
static int      tx_encode_fec(const uint8_t *data, uint16_t data_len,
                              uint8_t *out, uint16_t *out_len,
                              uint8_t *fec_flag);
static int      tx_push_header(EchoProtocol *echo, uint16_t header);
static int      tx_push_payload(EchoProtocol *echo, const uint8_t *data, uint16_t len);

/* ── RX helpers ─────────────────────────────────────────────────────── */
static int      rx_read_header(EchoProtocol *echo, uint16_t *header);
static int      rx_decode_fec(EchoProtocol *echo,
                              uint8_t *data, int *data_len);
static int      rx_decompress(EchoProtocol *echo,
                              const uint8_t *in, int in_len,
                              uint8_t *out, int *out_len);
static int      rx_validate_ip(const uint8_t *pkt, int len);
static int      rx_deliver_tun(EchoProtocol *echo, const uint8_t *pkt, int len);

/* ══════════════════════════════════════════════════════════════════════
   echo_init / echo_close
   ══════════════════════════════════════════════════════════════════════ */

int echo_init(EchoProtocol *echo, char *dev_name) {
    echo->tun_fd = tun_alloc(dev_name, IFF_TUN | IFF_NO_PI);
    if (echo->tun_fd < 0) return -1;

    echo->tx_rb = rb_init();
    echo->rx_rb = rb_init();

    pre_calc_fsk(&echo->mod_state);

    uint16_t freqs[4] = {FREQ_00, FREQ_01, FREQ_10, FREQ_11};
    for (int i = 0; i < 4; i++)
        pre_calc_goertzel(&echo->freq_states[i], &freqs[i]);

    rx_reset(echo);

    rohc_init(&echo->rohc_tx);
    rohc_init(&echo->rohc_rx);
    scrambler_init(&echo->tx_scrambler);
    scrambler_init(&echo->rx_scrambler);

    /* Initialize new modules */
    arq_init(&echo->arq, 16, 500, NULL);  /* window=16, timeout=500ms */
    delta_init(&echo->delta, NULL);
    mac_init(&echo->mac, 0, 2, 4, 10000, 3000, NULL);  /* node_id=0, 2 nodes, 4 slots, 10ms slots, 3ms guard */
    
    echo->use_delta_compression = true;
    echo->use_arq = true;
    echo->use_adaptive_mac = true;
    echo->screen_state_valid = false;

    echo->tx.tx_sample_count = SAMPLES_PER_SYMBOL;
    memset(&echo->stats, 0, sizeof(echo->stats));
    return 0;
}

void echo_close(EchoProtocol *echo) {
    close(echo->tun_fd);
    free(echo->tx_rb);
    free(echo->rx_rb);
}

/* ══════════════════════════════════════════════════════════════════════
   TX PATH:  tun_to_rb
   ══════════════════════════════════════════════════════════════════════ */

static int tx_compress(const uint8_t *raw, int raw_len,
                       EchoProtocol *echo,
                       uint8_t *out, uint16_t *out_len,
                       uint8_t *rohc_flag, uint8_t *comp_flag) {
    uint8_t rohc_buf[ROHC_MAX_COMPRESSED];
    uint8_t lz4_buf[SIZE_BUF + 64];

    int rohc_result = rohc_compress(&echo->rohc_tx, raw, raw_len,
                                     rohc_buf, sizeof(rohc_buf));
    if (rohc_result > 0 && rohc_result < raw_len) {
        *out_len = (uint16_t)rohc_result;
        *rohc_flag = 1;
        memcpy(out, rohc_buf, *out_len);
        return 0;
    }

    if (rohc_result < 0) {
        int lz4_size = LZ4_compress_default((const char *)raw, (char *)lz4_buf,
                                            raw_len, sizeof(lz4_buf));
        if (lz4_size > 0 && lz4_size < raw_len) {
            *out_len = (uint16_t)lz4_size;
            *comp_flag = 1;
            memcpy(out, lz4_buf, *out_len);
            return 0;
        }
    }

    *out_len = (uint16_t)raw_len;
    memcpy(out, raw, *out_len);
    return 0;
}

static int tx_encode_fec(const uint8_t *data, uint16_t data_len,
                         uint8_t *out, uint16_t *out_len,
                         uint8_t *fec_flag) {
    int fec_len = fec_encoded_len(data_len);
    if (fec_len <= 0 || fec_len > SIZE_BUF * 2)
        return 0;

    int result = fec_encode(data, data_len, out, SIZE_BUF * 2);
    if (result <= 0)
        return 0;

    *out_len = (uint16_t)result;
    *fec_flag = 1;
    return 0;
}

static int tx_push_bit(EchoProtocol *echo, uint8_t bit, const char *where) {
    bit = scrambler_process(&echo->tx_scrambler, bit);
    if (!put_bits(echo->tx_rb, &bit)) {
        log_warn("TX buffer full during %s", where);
        return -1;
    }
    return 0;
}

static int tx_push_header(EchoProtocol *echo, uint16_t header) {
    scrambler_reset(&echo->tx_scrambler);

    for (int i = HEADER_BITS - 1; i >= 0; i--) {
        if (tx_push_bit(echo, (header >> i) & 1, "header") < 0)
            return -1;
    }
    return 0;
}

static int tx_push_payload(EchoProtocol *echo, const uint8_t *data, uint16_t len) {
    int total_bits = len * SIZE_BYTE;
    for (int i = 0; i < total_bits; i++) {
        uint8_t bit = (data[i >> 3] >> (7 - (i & 7))) & 1;
        if (tx_push_bit(echo, bit, "payload") < 0)
            return -1;
    }
    return 0;
}

void tun_to_rb(EchoProtocol *echo) {
    uint8_t raw_buf[SIZE_BUF];
    uint8_t payload_buf[SIZE_BUF * 2];
    uint8_t fec_buf[SIZE_BUF * 2];
    uint16_t payload_len = 0;
    uint8_t rohc_flag = 0, comp_flag = 0, fec_flag = 0;

    int nread = tun_read(echo->tun_fd, raw_buf, sizeof(raw_buf));
    if (nread <= 0) return;

    tx_compress(raw_buf, nread, echo, payload_buf, &payload_len,
                &rohc_flag, &comp_flag);

    if (tx_encode_fec(payload_buf, payload_len, fec_buf, &payload_len, &fec_flag) == 0) {
        memcpy(payload_buf, fec_buf, payload_len);
    }

    static int tx_raw_counter = 0;
    if (++tx_raw_counter >= ROHC_RESYNC_INTERVAL) {
        tx_raw_counter = 0;
        payload_len = (uint16_t)nread;
        memcpy(payload_buf, raw_buf, nread);
        rohc_flag = 0;
        comp_flag = 0;
        fec_flag = 0;
        log_debug("TX force raw for ROHC resync");
    }

    uint16_t header = (comp_flag << 15) | (rohc_flag << 14) |
                      (fec_flag << 13) | (payload_len & 0x1FFF);

    if (tx_push_header(echo, header) < 0) return;
    if (tx_push_payload(echo, payload_buf, payload_len) < 0) return;

    echo->stats.tx_packets++;
    echo->stats.tx_bytes += payload_len;
    log_debug("TX | tun=%d | air=%d | rohc=%d | lz4=%d | fec=%d",
              nread, payload_len, rohc_flag, comp_flag, fec_flag);
}

/* ══════════════════════════════════════════════════════════════════════
   TX helper: rb_to_audio (just pass-through)
   ══════════════════════════════════════════════════════════════════════ */

void rb_to_audio(EchoProtocol *echo, uint8_t *symbol) {
    generate_fsk(&echo->mod_state, symbol);
}

/* ══════════════════════════════════════════════════════════════════════
   RX PATH:  rb_to_tun
   ══════════════════════════════════════════════════════════════════════ */

static int rx_read_header(EchoProtocol *echo, uint16_t *header) {
    uint8_t bit;
    uint32_t acc = 0;

    for (int i = 0; i < HEADER_BITS; i++) {
        if (!get_bits(echo->rx_rb, &bit)) return -1;
        acc = (acc << 1) | bit;
    }

    *header = (uint16_t)(acc & 0xFFFF);
    rx_decode_header(*header, &echo->rx.is_compressed, &echo->rx.is_rohc,
                     &echo->rx.is_fec, &echo->rx.packet_len);

    if (!is_valid_header(*header)) {
        log_warn("RX invalid header=0x%04X (comp=%u rohc=%u fec=%u len=%u)",
                 *header, echo->rx.is_compressed, echo->rx.is_rohc,
                 echo->rx.is_fec, echo->rx.packet_len);
        return -1;
    }
    return 0;
}

static int rx_decode_fec(EchoProtocol *echo, uint8_t *data, int *data_len) {
    if (!echo->rx.is_fec) return 0;

    uint8_t decoded[SIZE_BUF * 2];
    int result = fec_decode(data, *data_len, decoded, sizeof(decoded));
    if (result <= 0) {
        echo->stats.rx_corrupted++;
        log_warn("RX corrupt | reason=fec_fail | air=%d", *data_len);
        return -1;
    }
    memcpy(data, decoded, result);
    *data_len = result;
    return 0;
}

static int rx_decompress(EchoProtocol *echo,
                         const uint8_t *in, int in_len,
                         uint8_t *out, int *out_len) {
    if (echo->rx.is_rohc) {
        *out_len = rohc_decompress(&echo->rohc_rx, in, in_len,
                                   out, SIZE_BUF * 2);
        if (*out_len <= 0) {
            rohc_sync_context(&echo->rohc_rx, in, in_len);
            *out_len = rohc_decompress(&echo->rohc_rx, in, in_len,
                                       out, SIZE_BUF * 2);
        }
        if (*out_len <= 0) {
            echo->stats.rx_corrupted++;
            log_warn("RX corrupt | reason=rohc_fail | air=%d", in_len);
            return -1;
        }
        return 0;
    }

    if (echo->rx.is_compressed) {
        *out_len = LZ4_decompress_safe((const char *)in, (char *)out,
                                       in_len, SIZE_BUF * 2);
        if (*out_len < 0) {
            echo->stats.rx_corrupted++;
            log_warn("RX corrupt | reason=lz4_fail | air=%d", in_len);
            return -1;
        }
        return 0;
    }

    memcpy(out, in, in_len);
    *out_len = in_len;
    rohc_sync_context(&echo->rohc_rx, in, in_len);
    return 0;
}

static int rx_validate_ip(const uint8_t *pkt, int len) {
    if (len < 20) return 0;
    if ((pkt[0] & 0xF0) != 0x40) return 0;

    int ihl = (pkt[0] & 0x0F) * 4;
    if (ip_checksum(pkt, ihl) != 0) return -1;
    return 0;
}

static int rx_deliver_tun(EchoProtocol *echo, const uint8_t *pkt, int len) {
    if (len <= 0) return -1;
    tun_write(echo->tun_fd, (uint8_t *)pkt, len);
    echo->stats.rx_packets++;
    echo->stats.rx_bytes += len;
    log_info("RX OK | tun=%d | air=%d | rohc=%d | lz4=%d",
             len, echo->rx.packet_len, echo->rx.is_rohc, echo->rx.is_compressed);
    return 0;
}

static int rx_read_payload(EchoProtocol *echo, uint8_t *out, int len) {
    int total_bits = len * SIZE_BYTE;
    for (int i = 0; i < total_bits; i++) {
        uint8_t bit;
        if (!get_bits(echo->rx_rb, &bit)) return -1;
        out[i >> 3] = (out[i >> 3] << 1) | bit;
    }
    return 0;
}

void rb_to_tun(EchoProtocol *echo, int *packet_len) {
    uint16_t header;
    uint8_t payload[SIZE_BUF * 2];
    uint8_t final_ip[SIZE_BUF * 2];
    int payload_len = 0;
    int out_len = 0;

    if (rx_read_header(echo, &header) < 0) {
        rx_reset(echo);
        return;
    }

    if (*packet_len <= 0 || *packet_len > (int)sizeof(payload)) {
        echo->stats.rx_corrupted++;
        log_warn("RX corrupt | reason=bad_len | air=%d", *packet_len);
        rx_reset(echo);
        return;
    }

    if (rx_read_payload(echo, payload, *packet_len) < 0) {
        rx_reset(echo);
        return;
    }
    payload_len = *packet_len;

    if (rx_decode_fec(echo, payload, &payload_len) < 0) {
        rx_reset(echo);
        return;
    }

    if (rx_decompress(echo, payload, payload_len, final_ip, &out_len) < 0) {
        rx_reset(echo);
        return;
    }

    if (rx_validate_ip(final_ip, out_len) < 0) {
        echo->stats.rx_corrupted++;
        log_warn("RX corrupt | reason=ip_cksum | air=%d", *packet_len);
        rx_reset(echo);
        return;
    }

    rx_deliver_tun(echo, final_ip, out_len);
    rx_reset(echo);
}

/* ══════════════════════════════════════════════════════════════════════
   RX BIT PROCESSING:  audio_to_rb → process_rx_bit
   ══════════════════════════════════════════════════════════════════════ */

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
}

static uint8_t is_valid_fec_len(uint16_t len) {
    if (len < 3) return 0;
    if (len <= FEC_RS_MAX_N + 2) {
        for (int data_len = 1; data_len <= FEC_RS_MAX_N; data_len++) {
            if (fec_encoded_len(data_len) == len) return 1;
        }
        return 0;
    }
    if ((len - 2) % FEC_RS_MAX_N == 0) {
        int nblocks = (len - 2) / FEC_RS_MAX_N;
        int data_len = nblocks * FEC_RS_MSGBLK;
        if (fec_encoded_len(data_len) == len) return 1;
    }
    return 0;
}

static void rx_decode_header(uint16_t header, uint8_t *comp, uint8_t *rohc,
                             uint8_t *fec, uint16_t *len) {
    *comp = (header >> 15) & 1;
    *rohc = (header >> 14) & 1;
    *fec  = (header >> 13) & 1;
    *len  = header & 0x1FFF;
}

static uint8_t is_valid_header(uint16_t header) {
    uint8_t comp, rohc, fec;
    uint16_t len;
    rx_decode_header(header, &comp, &rohc, &fec, &len);

    if (comp && rohc) return 0;
    if (len == 0 || len > SIZE_BUF) return 0;
    if (fec && !is_valid_fec_len(len)) return 0;
    return 1;
}

static void handle_searching(EchoProtocol *echo, uint8_t bit, time_t now) {
    if (check_sync_word(&echo->rx.sync_accumulator, &bit) != 0) return;

    echo->rx.state = DATA;
    echo->rx.bits_received = 0;
    echo->rx.packet_len = 0;
    echo->rx.header_accumulator = 0;
    echo->rx.last_rx_time = now;
    scrambler_reset(&echo->rx_scrambler);
    echo->stats.rx_sync_found++;
    log_info("RX sync=ok");
}

static void handle_data(EchoProtocol *echo, uint8_t bit, time_t now) {
    if (echo->rx.bits_received == 0) {
        echo->rx.last_rx_time = now;
    }

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
    rx_decode_header(header, &echo->rx.is_compressed, &echo->rx.is_rohc,
                     &echo->rx.is_fec, &echo->rx.packet_len);

    if (!is_valid_header(header)) {
        log_warn("RX invalid header=0x%04X (comp=%u rohc=%u fec=%u len=%u) -> back to SEARCHING",
                 header, echo->rx.is_compressed, echo->rx.is_rohc, echo->rx.is_fec, echo->rx.packet_len);
        rx_reset(echo);
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
        handle_searching(echo, bit, now);
        return;
    }

    handle_data(echo, bit, now);
}

void audio_to_rb(EchoProtocol *echo, float *sample) {
    float mag[4];
    for (int i = 0; i < 4; i++)
        mag[i] = process_goertzel(&echo->freq_states[i], sample);

    if (++echo->rx.rx_sample_count < SAMPLES_PER_SYMBOL)
        return;

    int max_idx = 0;
    for (int i = 1; i < 4; i++)
        if (mag[i] > mag[max_idx]) max_idx = i;

    uint8_t bits[2];
    bits[0] = (max_idx >> 1) & 1;
    bits[1] = max_idx & 1;

    for (int i = 0; i < 4; i++)
        reset_state(&echo->freq_states[i]);
    echo->rx.rx_sample_count = 0;

    process_rx_bit(echo, bits[0]);
    process_rx_bit(echo, bits[1]);
}

/* ══════════════════════════════════════════════════════════════════════
   NEW LAYER INTEGRATION: ARQ + Delta Compression + MAC Adaptive
   ══════════════════════════════════════════════════════════════════════ */

/* Send packet through ARQ -> Delta -> MAC layers */
int echo_send_packet(EchoProtocol *echo, const uint8_t *data, uint16_t len) {
    if (!echo->use_arq) {
        /* Direct to TX ring buffer (legacy path) */
        return 0;
    }
    
    DeltaFrame delta_frame;
    
    if (echo->use_delta_compression) {
        /* Delta compress the screen/terminal state */
        delta_compress(&echo->delta, data, len, &delta_frame);
        
        /* Send through ARQ */
        if (delta_frame.is_intra) {
            /* Intra-frame: send full state */
            return arq_send(&echo->arq, delta_frame.state, delta_frame.state_len);
        } else {
            /* Delta frame: send delta payload */
            return arq_send(&echo->arq, delta_frame.delta, delta_frame.delta_len);
        }
    } else {
        /* No delta compression, send raw through ARQ */
        return arq_send(&echo->arq, data, len);
    }
}

/* Receive packet through MAC -> ARQ -> Delta layers */
int echo_recv_packet(EchoProtocol *echo, uint8_t *out, uint16_t max_len) {
    if (!echo->use_arq) {
        return -1;
    }
    
    /* Check ARQ for delivered packets */
    if (!arq_has_delivered(&echo->arq)) {
        return -1;
    }
    
    uint8_t arq_payload[256];
    int arq_len = arq_get_delivered(&echo->arq, arq_payload, sizeof(arq_payload));
    if (arq_len <= 0) return -1;
    
    if (echo->use_delta_compression) {
        /* Decompress delta */
        DeltaFrame frame;
        frame.seq_num = 0;  /* Will be filled by ARQ layer in real impl */
        frame.delta_len = arq_len;
        memcpy(frame.delta, arq_payload, arq_len);
        frame.is_intra = false;  /* Would be determined by frame header */
        
        uint8_t decompressed[DELTA_MAX_STATE_SIZE];
        uint16_t dec_len = 0;
        int res = delta_decompress(&echo->delta, &frame, decompressed, &dec_len);
        if (res < 0) {
            if (delta_needs_intra(&echo->delta)) {
                /* Request intra-frame */
                uint16_t base = delta_get_intra_base(&echo->delta);
                ArqFrame intra_req;
                arq_gen_intra_req(&echo->arq, base, &intra_req);
                /* Would send intra_req through MAC */
            }
            return -1;
        }
        
        if (dec_len > max_len) dec_len = max_len;
        memcpy(out, decompressed, dec_len);
        return dec_len;
    } else {
        /* No delta, return raw */
        if (arq_len > max_len) arq_len = max_len;
        memcpy(out, arq_payload, arq_len);
        return arq_len;
    }
}

/* Update MAC with fixed timestep (game loop style) */
void echo_update_mac(EchoProtocol *echo, uint32_t dt_us) {
    if (!echo->use_adaptive_mac) return;
    
    mac_update(&echo->mac, dt_us);
    
    /* Check for slot boundary and get action */
    if (mac_is_slot_boundary(&echo->mac)) {
        MacAction action = mac_get_action(&echo->mac);
        
        /* Handle MAC action */
        switch (action) {
            case MAC_ACTION_TX_NOW:
                /* Trigger transmission */
                mac_on_tx_result(&echo->mac, true, false, false);
                break;
            case MAC_ACTION_WAIT:
                /* Do nothing this slot */
                break;
            case MAC_ACTION_POWER_LOW:
            case MAC_ACTION_POWER_HIGH:
                /* Adjust TX power (would integrate with audio_io) */
                break;
            default:
                break;
        }
    }
}

/* Process ARQ timeouts and retransmissions */
void echo_process_arq_timeouts(EchoProtocol *echo) {
    if (!echo->use_arq) return;
    
    arq_timeout_check(&echo->arq);
    
    /* Check for frames to retransmit */
    ArqFrame frame;
    if (arq_get_next_frame(&echo->arq, &frame)) {
        /* Frame would be sent through MAC/PHY layer */
        /* For now, just log */
        log_debug("ARQ: frame ready for TX seq=%u type=%d len=%u",
                  frame.seq_num, frame.type, frame.payload_len);
    }
}
