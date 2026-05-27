#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <math.h>
#include <time.h>
#include "../include/echo_protocol.h"

void run_noise_test(float noise_amplitude) {
    printf("\n--- Testando com %.0f%% de ruído ---\n", noise_amplitude * 100);
    
    EchoProtocol echo;
    // Mock manual de inicialização para evitar dependência de TUN
    echo.tx_rb = rb_init();
    echo.rx_rb = rb_init();
    pre_calc_afsk(&echo.mod_state);
    uint16_t f1200 = 1200;
    uint16_t f2400 = 2400;
    pre_calc_goertzel(&echo.space_state, &f1200);
    pre_calc_goertzel(&echo.mark_state, &f2400);
    
    // Forçar estado DATA para teste de BER puro
    echo.rx.state = DATA;
    echo.rx.rx_sample_count = 0;
    echo.rx.bits_received = 0;
    echo.rx.packet_len = 2000; // Valor alto para não resetar

    uint32_t num_bits = 5000;
    uint8_t input_bits[5000];
    for (uint32_t i = 0; i < num_bits; i++) {
        input_bits[i] = rand() % 2;
    }

    uint32_t errors = 0;
    for (uint32_t b = 0; b < num_bits; b++) {
        uint8_t bit = input_bits[b];
        
        for (int i = 0; i < SAMPLES_PER_BIT; i++) {
            generate_afsk(&echo.mod_state, &bit);
            float noise = ((float)rand() / (float)RAND_MAX * 2.0f - 1.0f) * noise_amplitude;
            float sample = echo.mod_state.current_sin + noise;
            audio_to_rb(&echo, &sample);
        }

        uint8_t detected;
        if (get_bits(echo.rx_rb, &detected)) {
            if (detected != bit) {
                errors++;
            }
        } else {
            // Se não produziu bit, conta como erro de sincronismo/perda
            errors++;
        }
    }

    float ber = (float)errors / num_bits;
    printf("Resultados (%.0f%% ruído): Erros: %u | BER: %.4f%%\n", 
           noise_amplitude * 100, errors, ber * 100);

    free(echo.tx_rb);
    free(echo.rx_rb);
}

int main() {
    printf("--- Teste de Stress de Ruído Progressivo ---\n");
    srand(42); // Seed fixa para comparação justa

    run_noise_test(1.25f); // 125% (Base atual)
    run_noise_test(1.50f); // 150%
    run_noise_test(2.00f); // 200%
    run_noise_test(2.50f); // 250%

    return 0;
}
