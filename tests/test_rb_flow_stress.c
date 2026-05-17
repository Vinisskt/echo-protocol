#include <stdio.h>
#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include "../include/rb_bits.h"

#define TOTAL_BITS 100000
#define CHUNK_SIZE 64 // Quantos bits processar por "ciclo"

void test_rb_flow_stress() {
    printf("Iniciando Teste de Fluxo Contínuo (Stress): %d bits...\n", TOTAL_BITS);
    
    Buffer *rb = rb_init();
    uint8_t *stream = malloc(TOTAL_BITS);
    
    // Gera stream de dados aleatórios
    for (int i = 0; i < TOTAL_BITS; i++) {
        stream[i] = rand() % 2;
    }

    int put_idx = 0;
    int get_idx = 0;
    int iterations = 0;

    // Simula um processo real: enche um pouco, esvazia um pouco
    while (get_idx < TOTAL_BITS) {
        // Produtor: Tenta colocar CHUNK_SIZE bits
        for (int i = 0; i < CHUNK_SIZE && put_idx < TOTAL_BITS; i++) {
            if (put_bits(rb, &stream[put_idx])) {
                put_idx++;
            } else {
                // Buffer cheio, para de produzir
                break;
            }
        }

        // Consumidor: Tenta ler CHUNK_SIZE bits
        for (int i = 0; i < CHUNK_SIZE && get_idx < put_idx; i++) {
            uint8_t bit;
            if (get_bits(rb, &bit)) {
                if (bit != stream[get_idx]) {
                    printf("[ERRO] Corrupção de dados na iteração %d! Bit %d: esperado %d, obtido %d\n", 
                           iterations, get_idx, stream[get_idx], bit);
                    exit(1);
                }
                get_idx++;
            } else {
                // Buffer vazio, para de consumir
                break;
            }
        }
        iterations++;
    }

    printf("Sucesso! %d bits processados sem erros em %d ciclos de I/O.\n", get_idx, iterations);
    
    free(stream);
    free(rb);
}

int main() {
    printf("--- Teste de Stress de Fluxo: Ring Buffer ---\n");
    // Seed fixa para reprodutibilidade
    srand(42); 
    test_rb_flow_stress();
    return 0;
}
