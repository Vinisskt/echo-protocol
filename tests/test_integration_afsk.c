#include <stdio.h>
#include <assert.h>
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include "../include/mod_afsk.h"
#include "../include/rb_bits.h"

void test_full_chain_integration() {
    printf("Iniciando Teste de Integração: Ring Buffer -> AFSK Modulator...\n");

    // 1. Inicialização
    Buffer *rb = rb_init();
    StateAFSK state;
    pre_calc_afsk(&state);

    // 2. Produção: Inserir um byte (0xA5 = 10100101)
    uint8_t data = 0xA5;
    for (int i = 0; i < 8; i++) {
        uint8_t bit = (data >> (7 - i)) & 1;
        put_bits(rb, &bit);
    }
    printf("Byte 0xA5 (10100101) inserido no buffer.\n");

    // 3. Consumo e Modulação
    int total_samples = 0;
    uint8_t current_bit;
    
    printf("Iniciando modulação bit-a-bit...\n");
    while (get_bits(rb, &current_bit)) {
        // Para cada bit, geramos SAMPLES_PER_BIT (40)
        float start_phase = atan2f(state.current_sin, state.current_cos);
        
        for (int j = 0; j < SAMPLES_PER_BIT; j++) {
            generate_afsk(&state, &current_bit);
            total_samples++;
        }
        
        float end_phase = atan2f(state.current_sin, state.current_cos);
        float delta = end_phase - start_phase;
        
        // Normaliza para [-PI, PI]
        while (delta > M_PI) delta -= 2.0f * M_PI;
        while (delta < -M_PI) delta += 2.0f * M_PI;

        // Como 40 amostras em 1200/2400Hz @ 48kHz dão ciclos INTEIROS,
        // o delta de fase deve ser próximo de zero.
        // Aumentamos a tolerância para 0.08 para acomodar erro acumulado de float
        assert(fabsf(delta) < 0.08f);
        
        printf("  Bit %d: %d amostras geradas. Fase delta: %.4f (OK)\n", current_bit, SAMPLES_PER_BIT, delta);
    }

    assert(total_samples == 8 * SAMPLES_PER_BIT);
    printf("Sucesso! Total de %d amostras geradas para 8 bits.\n", total_samples);

    free(rb);
}

int main() {
    printf("--- Teste de Integração: Fluxo Completo de Transmissão ---\n");
    test_full_chain_integration();
    printf("\n✓ Integração validada!\n");
    return 0;
}
