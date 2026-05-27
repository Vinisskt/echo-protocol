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
    printf("--- Teste: IPv6 Header Extraction ---\n");
    
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

    // Cabeçalho IPv6: 40 bytes fixos
    // Payload length está nos bytes 4-5
    uint8_t pkt6[60] = {0};
    pkt6[0] = 0x60; // Version 6
    pkt6[4] = 0x00; pkt6[5] = 0x14; // Payload Length: 20 bytes (total 60)
    for(int i=40; i<60; i++) pkt6[i] = i;

    printf("Enviando Pacote IPv6 de 60 bytes (40 header + 20 payload)...\n");
    feed_sync(&echo);
    feed_bits(&echo, pkt6, 60);
    
    assert(echo.rx.state == SEARCHING);
    assert(echo.rx.packet_len == 60);
    printf("✓ Pacote IPv6 detectado com tamanho 60 (correto).\n");

    uint8_t res[60];
    uint8_t bit;
    for(int i=0; i<60*8; i++) {
        get_bits(echo.rx_rb, &bit);
        res[i>>3] = (res[i>>3] << 1) | bit;
    }
    assert(memcmp(res, pkt6, 60) == 0);
    printf("✓ Conteúdo do Pacote IPv6 validado.\n");

    echo_close(&echo);
    printf("✓ SUCESSO: Teste de extração de header IPv6 concluído.\n");
    return 0;
}
