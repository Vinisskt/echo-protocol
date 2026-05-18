#include <stdio.h>
#include <math.h>
#include <stdint.h>
#include "../include/demod_afsk.h"
#include "../include/mod_afsk.h"

int main() {
    printf("--- Teste de Detecção Goertzel: 1200Hz vs 2400Hz ---\n");

    StateGoertzel space, mark;
    uint16_t f1200 = 1200;
    uint16_t f2400 = 2400;
    
    pre_calc_goertzel(&space, &f1200); 
    pre_calc_goertzel(&mark, &f2400);

    float sample;
    float phase = 0.0f;
    float phase_step = (2.0f * M_PI * 1200.0f) / 48000.0f; // Onda de 1200Hz
    float mag_space = 0, mag_mark = 0;

    printf("Processando 40 amostras de 1200Hz...\n");
    for (int i = 0; i < 40; i++) {
        sample = sinf(phase);
        mag_space = process_goertzel(&space, &sample);
        mag_mark = process_goertzel(&mark, &sample);
        phase += phase_step;
    }

    printf("Magnitude final 1200Hz (Space): %f\n", mag_space);
    printf("Magnitude final 2400Hz (Mark): %f\n", mag_mark);

    if (mag_space > mag_mark) {
        printf("✓ Sucesso: Frequência de 1200Hz detectada corretamente!\n");
    } else {
        printf("[FALHA] Detecção incorreta ou empate.\n");
        return 1;
    }

    return 0;
}
