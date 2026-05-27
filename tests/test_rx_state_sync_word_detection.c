#include <stdio.h>
#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "../include/echo_protocol.h"

// Função auxiliar para gerar samples de uma frequência por um bit
void feed_bit(EchoProtocol *echo, uint16_t freq) {
    float step = (2.0f * 3.14159f * freq) / SAMPLE_RATE;
    for (int i = 0; i < SAMPLES_PER_BIT; i++) {
        float sample = sinf(i * step);
        audio_to_rb(echo, &sample);
    }
}

int main() {
    printf("--- Teste: rx_state Sync Word Detection ---\n");
    
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

    printf("Enviando bits que NÃO são a sync word...\n");
    for (int i = 0; i < 32; i++) {
        feed_bit(&echo, 1200); // Space
    }
    assert(echo.rx.state == SEARCHING);
    assert(echo.rx.bits_received == 0);
    printf("✓ Ainda em SEARCHING após dados aleatórios.\n");

    printf("Enviando SYNC_WORD (0x%08X)...\n", SYNC_WORD);
    for (int i = 31; i >= 0; i--) {
        uint16_t freq = (SYNC_WORD >> i) & 1 ? 2400 : 1200;
        feed_bit(&echo, freq);
    }

    assert(echo.rx.state == DATA);
    assert(echo.rx.bits_received == 0); // Primeiro bit do payload ainda não chegou
    printf("✓ Transição para DATA detectada após SYNC_WORD!\n");

    printf("Enviando primeiro bit do payload (1)...\n");
    feed_bit(&echo, 2400);
    assert(echo.rx.bits_received == 1);
    
    uint8_t bit;
    get_bits(echo.rx_rb, &bit);
    assert(bit == 1);
    printf("✓ Primeiro bit do payload recebido corretamente.\n");

    free(echo.tx_rb);
    free(echo.rx_rb);
    printf("✓ SUCESSO: Teste de sincronismo concluído.\n");
    return 0;
}
