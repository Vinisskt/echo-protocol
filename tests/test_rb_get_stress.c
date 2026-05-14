#include <stdio.h>
#include <assert.h>
#include <stdlib.h>
#include <time.h>
#include "../include/rb_bits.h"

// Teste de simetria: escreve e lé milhares de bits aleatórios
void test_get_bits_stress_random() {
    printf("--- Teste: Stress de Simetria (Bits Aleatórios) ---\n");
    Buffer *rb = rb_init();
    assert(rb != NULL);
    srand(time(NULL));
    const int NUM_BITS = 5000;
    uint8_t *stream = malloc(NUM_BITS);
    
    // Gera stream aleatório
    for(int i = 0; i < NUM_BITS; i++) {
        stream[i] = rand() % 2;
    }
    int bits_written = 0;
    int bits_read = 0;
    uint8_t bit_out;
    
    // Simula tráfego: escreve alguns, lê alguns
    while(bits_read < NUM_BITS) {
        // Tenta escrever até 16 bits
        for(int i = 0; i < 16 && bits_written < NUM_BITS; i++) {
            if(put_bits(rb, &stream[bits_written])) {
                bits_written++;
            } else {
                break; // Buffer cheio
            }
        }
        // Tenta ler até 8 bits
        for(int i = 0; i < 8 && bits_read < bits_written; i++) {
            if(get_bits(rb, &bit_out)) {
                if(bit_out != stream[bits_read]) {
                    printf("\nERRO de simetria no bit %d! Esperado %d, recebido %d\n", 
                           bits_read, stream[bits_read], bit_out);
                    assert(bit_out == stream[bits_read]);
                }
                bits_read++;
            } else {
                break; // Buffer vazio
            }
        }
    }
    printf("Sucesso: %d bits transferidos sem erros.\n", bits_read);
    free(stream);
    free(rb);
}

// Teste de Wrap-around: força o head e tail a darem a volta no buffer várias vezes
// CORRIGIDO: Respeita a granularidade de bytes do buffer
void test_get_bits_wrap_around() {
    printf("--- Teste: Stress de Wrap-around (Circulando o Buffer) ---\n");
    Buffer *rb = rb_init();
    
    uint8_t bit_in;
    uint8_t bit_out;
    
    // Vamos transferir bits o suficiente para dar 5 voltas no buffer
    const int TOTAL_BYTES = BUFFER_SIZE * 5;
    const int TOTAL_BITS = TOTAL_BYTES * 8;
    
    for(int byte_idx = 0; byte_idx < TOTAL_BYTES; byte_idx++) {
        // Escreve 8 bits (um byte completo)
        for(int bit_idx = 0; bit_idx < 8; bit_idx++) {
            bit_in = (byte_idx * 8 + bit_idx) % 2;
            
            // Garante que consegue escrever
            while(!put_bits(rb, &bit_in)) {
                // Buffer cheio, precisa ler um pouco antes
                if(!get_bits(rb, &bit_out)) {
                    printf("Erro: Buffer vazio quando não deveria estar\n");
                    assert(0);
                }
            }
        }
        
        // Lê 8 bits (um byte completo)
        for(int bit_idx = 0; bit_idx < 8; bit_idx++) {
            bit_in = (byte_idx * 8 + bit_idx) % 2;
            
            if(!get_bits(rb, &bit_out)) {
                printf("Erro: Não conseguiu ler bit %d\n", byte_idx * 8 + bit_idx);
                assert(0);
            }
            
            if(bit_out != bit_in) {
                printf("Erro no bit %d (byte %d, bit %d): esperado %d, recebido %d\n",
                       byte_idx * 8 + bit_idx, byte_idx, bit_idx, bit_in, bit_out);
                assert(bit_out == bit_in);
            }
        }
    }
    
    printf("Sucesso: %d bits processados em wrap-around.\n", TOTAL_BITS);
    free(rb);
}

int main() {
    test_get_bits_stress_random();
    test_get_bits_wrap_around();
    printf("\n✓ Todos os testes passaram!\n");
    return 0;
}
