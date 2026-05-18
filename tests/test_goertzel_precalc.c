#include <stdio.h>
#include <math.h>
#include "../include/demod_afsk.h"
#include "../include/mod_afsk.h"

int main() {
    printf("--- Teste de Pré-Cálculo Goertzel ---\n");

    StateGoertzel space;
    uint16_t val_freq = 1200; 
    
    pre_calc_goertzel(&space, &val_freq);

    printf("N: %d\n", space.n);
    printf("K: %f\n", space.k);
    printf("Omega: %f\n", space.omega);
    printf("Coeff: %f\n", space.coeff);

    if (space.n == 40 && space.q1 == 0 && space.q2 == 0) {
        printf("✓ Inicialização de variáveis de estado OK!\n");
    } else {
        printf("[FALHA] Variáveis de estado não zeradas ou N incorreto!\n");
        return 1;
    }

    return 0;
}
