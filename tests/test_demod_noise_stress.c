#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <math.h>
#include <time.h>
#include "../include/demod_afsk.h"
#include "../include/mod_afsk.h"
#include "../include/rb_bits.h"

#define NOISE_AMPLITUDE 1.25f // 125% de ruído em relação ao sinal original

int main() {
    printf("--- Teste de Stress: Buffer Cheio + Ruído (%.0f%%) ---\n", NOISE_AMPLITUDE * 100);
    srand(time(NULL));

    Buffer *rb = rb_init();
    StateAFSK mod_state;
    pre_calc_afsk(&mod_state);

    StateGoertzel space, mark;
    uint16_t f1200 = 1200;
    uint16_t f2400 = 2400;
    pre_calc_goertzel(&space, &f1200);
    pre_calc_goertzel(&mark, &f2400);

    // 1. Preencher o buffer até o limite (1024 bytes ~ 8192 bits)
    printf("Preenchendo buffer com dados aleatórios...\n");
    uint8_t input_bits[BUFFER_SIZE * 8];
    uint32_t total_bits = 0;
    for (uint32_t i = 0; i < BUFFER_SIZE * 8; i++) {
        uint8_t bit = rand() % 2;
        if (put_bits(rb, &bit)) {
            input_bits[total_bits++] = bit;
        } else {
            break; // Buffer cheio
        }
    }
    printf("Buffer cheio com %u bits.\n", total_bits);

    // 2. Processar bit a bit com ruído
    uint32_t error_count = 0;
    printf("Iniciando transmissão e demodulação com ruído...\n");

    for (uint32_t b = 0; b < total_bits; b++) {
        uint8_t bit_to_mod;
        get_bits(rb, &bit_to_mod);

        // Reset dos filtros Goertzel para o novo bit
        space.q1 = 0; space.q2 = 0;
        mark.q1 = 0; mark.q2 = 0;

        float mag_s = 0, mag_m = 0;

        for (int i = 0; i < SAMPLES_PER_BIT; i++) {
            generate_afsk(&mod_state, &bit_to_mod);
            
            // Adicionando ruído branco à amostra
            float noise = ((float)rand() / (float)RAND_MAX * 2.0f - 1.0f) * NOISE_AMPLITUDE;
            float sample = mod_state.current_sin + noise;
            
            mag_s = process_goertzel(&space, &sample);
            mag_m = process_goertzel(&mark, &sample);
        }

        uint8_t detected_bit = (mag_m > mag_s) ? 1 : 0;
        if (detected_bit != input_bits[b]) {
            error_count++;
        }
    }

    printf("\n--- Resultados ---\n");
    printf("Total de bits processados: %u\n", total_bits);
    printf("Erros detectados: %u\n", error_count);
    float ber = (float)error_count / total_bits;
    printf("Taxa de Erro de Bit (BER): %.4f%%\n", ber * 100);

    if (error_count == 0) {
        printf("✓ SUCESSO: Zero erros mesmo com ruído e carga máxima!\n");
    } else if (ber < 0.01f) {
        printf("⚠ ALERTA: Alguns erros detectados (dentro do limite tolerável de 1%%).\n");
    } else {
        printf("[FALHA] Taxa de erro muito alta (%.2f%%).\n", ber * 100);
        return 1;
    }

    return 0;
}
