#include <stdio.h>
#include <stdint.h>
#include <math.h>
#include "../include/echo_protocol.h"

int main() {
    printf("--- Teste de Unidade: audio_to_rb (Validação de Inserção) ---\n");

    EchoProtocol echo;
    echo.rx_rb = rb_init();
    pre_calc_goertzel(&echo.space_state, (uint16_t[]){FREQ_SPACE});
    pre_calc_goertzel(&echo.mark_state, (uint16_t[]){FREQ_MARK});
    echo.rx.state = DATA; // Adicionado: Permitir inserção direta de bits para teste de unidade
    echo.rx.rx_sample_count = 0; 
    echo.rx.packet_len = 100; // Garantir que não volte para SEARCHING prematuramente
    float phase = 0;
    float step = (2.0f * M_PI * FREQ_MARK) / SAMPLE_RATE;

    printf("Enviando 40 amostras de 2400Hz (MARK)...\n");
    for (int i = 0; i < 40; i++) {
        float sample = sinf(phase);
        audio_to_rb(&echo, &sample);
        phase += step;
    }

    uint8_t bit = 99; // Valor inicial impossível
    uint8_t success = get_bits(echo.rx_rb, &bit);

    if (success) {
        printf("✓ Sucesso: O buffer contém um bit: %d\n", bit);
        if (bit == 1) {
            printf("✓ SUCESSO FINAL: Bit 1 detectado corretamente.\n");
        } else {
            printf("[FALHA] Bit incorreto detectado: %d\n", bit);
        }
    } else {
        printf("[FALHA CRÍTICA] O buffer está VAZIO. A função audio_to_rb não inseriu o bit.\n");
        printf("Dica: Verifique se a checagem 'if (echo->rx_sample_count == ...)' está batendo com o valor %d na 40ª amostra.\n", echo.rx.rx_sample_count);
    }

    return 0;
}
