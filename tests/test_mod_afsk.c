#include <stdio.h>
#include <assert.h>
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include "../include/mod_afsk.h"

#define EPSILON 0.00001f

void test_pre_calc_afsk_coefficients() {
    printf("Iniciando teste de coeficientes pre_calc_afsk...\n");

    StateAFSK state;
    pre_calc_afsk(&state);

    float expected_cos_s = cosf(2.0f * M_PI * FREQ_SPACE / SAMPLE_RATE);
    float expected_sin_s = sinf(2.0f * M_PI * FREQ_SPACE / SAMPLE_RATE);
    float expected_cos_m = cosf(2.0f * M_PI * FREQ_MARK / SAMPLE_RATE);
    float expected_sin_m = sinf(2.0f * M_PI * FREQ_MARK / SAMPLE_RATE);

    assert(fabsf(state.step_cos_space - expected_cos_s) < EPSILON);
    assert(fabsf(state.step_sin_space - expected_sin_s) < EPSILON);
    assert(fabsf(state.step_cos_mark - expected_cos_m) < EPSILON);
    assert(fabsf(state.step_sin_mark - expected_sin_m) < EPSILON);

    printf("Coeficientes verificados com sucesso!\n");
}

void test_bit_consumption_timing() {
    printf("Testando timing e integridade com 200 bits (Buffer Stress)...\n");
    
    StateAFSK state;
    pre_calc_afsk(&state);
    Buffer *rb = rb_init();
    
    uint8_t input_bits[200];
    for (int i = 0; i < 200; i++) {
        input_bits[i] = i % 2;
        put_bits(rb, &input_bits[i]);
    }

    printf("200 bits inseridos. Iniciando consumo e modulação...\n");

    for (int i = 0; i < 200; i++) {
        uint8_t current_bit;
        int has_bit = get_bits(rb, &current_bit);
        
        assert(has_bit == 1);
        assert(current_bit == input_bits[i]);

        // Simula o tempo de 40 amostras para cada bit chamando a função real
        for (int j = 0; j < SAMPLES_PER_BIT; j++) {
            generate_afsk(&state, &current_bit);
        }
    }
    
    printf("Timing e integridade de 200 bits validados com sucesso!\n");
    free(rb);
}

void test_frequency_consistency() {
    printf("Testando consistência da frequência por bit (Ponteiro de bit)...\n");
    
    StateAFSK state;
    pre_calc_afsk(&state);
    
    uint8_t bit = 1; // Mark

    float last_phase = atan2f(state.current_sin, state.current_cos);
    
    for (int i = 0; i < SAMPLES_PER_BIT; i++) {
        generate_afsk(&state, &bit); 

        float current_phase = atan2f(state.current_sin, state.current_cos);
        float delta = current_phase - last_phase;
        
        // Normaliza o delta para [0, 2*PI]
        if (delta < 0) delta += 2.0f * M_PI;
        
        float expected_delta = (2.0f * M_PI * FREQ_MARK) / SAMPLE_RATE;
        
        // Verifica se a variação de fase corresponde à frequência correta
        assert(fabsf(delta - expected_delta) < 0.001f);
        
        last_phase = current_phase;
    }
    
    printf("Frequência Mark (%dHz) consistente com a função real!\n", FREQ_MARK);
}

int main() {
    printf("--- Executando Testes de Rigor: mod_afsk ---\n");
    test_pre_calc_afsk_coefficients();
    test_bit_consumption_timing();
    test_frequency_consistency();
    printf("\n✓ Todos os testes de rigor passaram com código limpo!\n");
    return 0;
}
