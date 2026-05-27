#include <stdio.h>
#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "../include/echo_protocol.h"

int main() {
    printf("--- Teste: audio_to_rb Sample Count ---\n");
    
    EchoProtocol echo;
    // Mock tun_fd e buffers
    echo.tun_fd = -1;
    echo.tx_rb = rb_init();
    echo.rx_rb = rb_init();
    
    // Inicialização do DSP (frequências fixas para teste)
    uint16_t freq_space = 1200;
    uint16_t freq_mark = 2400;
    pre_calc_goertzel(&echo.space_state, &freq_space);
    pre_calc_goertzel(&echo.mark_state, &freq_mark);
    
    // Inicializar RxState
    echo.rx.state = DATA; // Pular detecção de sync para testar apenas contagem
    echo.rx.rx_sample_count = 0;
    echo.rx.bits_received = 0;
    echo.rx.packet_len = 100; // Valor alto para não resetar estado
    
    float sample = 0.5f; // Sample constante (não importa para contagem)

    printf("Simulando %d samples (deve produzir 1 bit)...\n", SAMPLES_PER_BIT);
    
    for (int i = 0; i < SAMPLES_PER_BIT - 1; i++) {
        audio_to_rb(&echo, &sample);
        assert(echo.rx.bits_received == 0);
    }
    printf("✓ %d samples processados: 0 bits produzidos (correto).\n", SAMPLES_PER_BIT - 1);

    // O último sample deve disparar a produção de um bit
    audio_to_rb(&echo, &sample);
    assert(echo.rx.bits_received == 1);
    assert(echo.rx.rx_sample_count == 0); // Deve ter resetado
    printf("✓ %dº sample processado: 1 bit produzido e contador resetado (correto).\n", SAMPLES_PER_BIT);

    free(echo.tx_rb);
    free(echo.rx_rb);
    printf("✓ SUCESSO: Teste de contagem de samples concluído.\n");
    return 0;
}
