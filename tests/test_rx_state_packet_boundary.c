#include <stdio.h>
#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "../include/echo_protocol.h"

void feed_bits(EchoProtocol *echo, uint8_t *data, int len) {
    for (int i = 0; i < len; i++) {
        for (int b = 7; b >= 0; b--) {
            uint8_t bit = (data[i] >> b) & 1;
            uint16_t freq = bit ? 2400 : 1200;
            // Gerar samples para esse bit
            float step = (2.0f * 3.14159f * freq) / SAMPLE_RATE;
            for (int s = 0; s < SAMPLES_PER_BIT; s++) {
                float sample = sinf(s * step);
                audio_to_rb(echo, &sample);
            }
        }
    }
}

void feed_sync(EchoProtocol *echo) {
    for (int i = 31; i >= 0; i--) {
        uint16_t freq = (SYNC_WORD >> i) & 1 ? 2400 : 1200;
        float step = (2.0f * 3.14159f * freq) / SAMPLE_RATE;
        for (int s = 0; s < SAMPLES_PER_BIT; s++) {
            float sample = sinf(s * step);
            audio_to_rb(echo, &sample);
        }
    }
}

int main() {
    printf("--- Teste: rx_state Packet Boundary (DATA -> SEARCHING) ---\n");
    
    EchoProtocol echo;
    echo.tun_fd = -1;
    echo.tx_rb = rb_init();
    echo.rx_rb = rb_init();
    
    uint16_t fs = 1200;
    uint16_t fm = 2400;
    pre_calc_goertzel(&echo.space_state, &fs);
    pre_calc_goertzel(&echo.mark_state, &fm);
    
    echo.rx.state = SEARCHING;
    echo.rx.sync_accumulator = 0;
    echo.rx.rx_sample_count = 0;
    echo.rx.bits_received = 0;
    echo.rx.packet_len = 0;
    echo.rx.header_accumulator = 0;

    // Pacote 1: IPv4 de 28 bytes
    uint8_t pkt1[28] = {0};
    pkt1[0] = 0x45; // Version 4, IHL 5
    pkt1[2] = 0x00; pkt1[3] = 0x1C; // Total Length: 28
    for(int i=4; i<28; i++) pkt1[i] = i;

    // Pacote 2: IPv4 de 20 bytes
    uint8_t pkt2[20] = {0};
    pkt2[0] = 0x45;
    pkt2[3] = 0x14; // Total Length: 20
    for(int i=4; i<20; i++) pkt2[i] = i + 100;

    printf("Enviando Pacote 1...\n");
    feed_sync(&echo);
    feed_bits(&echo, pkt1, 28);
    
    assert(echo.rx.state == SEARCHING);
    assert(echo.rx.bits_received == 28 * 8); // No DATA state it was 224, then moved to SEARCHING
    // Wait, in handle_data_state: if (bits_received == total_bits) state = SEARCHING.
    // So when it hits 224, it transitions.
    printf("✓ Pacote 1 processado. Estado voltou para SEARCHING.\n");

    printf("Enviando Pacote 2...\n");
    feed_sync(&echo);
    feed_bits(&echo, pkt2, 20);
    
    assert(echo.rx.state == SEARCHING);
    printf("✓ Pacote 2 processado. Estado voltou para SEARCHING.\n");

    // Verificar se ambos estão no buffer
    uint8_t res1[28], res2[20];
    uint8_t bit;
    for(int i=0; i<28*8; i++) {
        get_bits(echo.rx_rb, &bit);
        res1[i>>3] = (res1[i>>3] << 1) | bit;
    }
    assert(memcmp(res1, pkt1, 28) == 0);
    printf("✓ Conteúdo do Pacote 1 validado no buffer.\n");

    for(int i=0; i<20*8; i++) {
        get_bits(echo.rx_rb, &bit);
        res2[i>>3] = (res2[i>>3] << 1) | bit;
    }
    assert(memcmp(res2, pkt2, 20) == 0);
    printf("✓ Conteúdo do Pacote 2 validado no buffer.\n");

    free(echo.tx_rb);
    free(echo.rx_rb);
    printf("✓ SUCESSO: Teste de fronteira de pacotes concluído.\n");
    return 0;
}
