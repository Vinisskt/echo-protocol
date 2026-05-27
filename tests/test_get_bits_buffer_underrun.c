#include <stdio.h>
#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include "../include/rb_bits.h"

int main() {
    printf("--- Teste: get_bits Buffer Underrun ---\n");
    
    Buffer *rb = rb_init();
    uint8_t bit = 77; // Valor inicial arbitrário
    
    // 1. Buffer vazio deve retornar 0 e não mudar o valor do bit
    uint8_t res = get_bits(rb, &bit);
    assert(res == 0);
    assert(bit == 77);
    printf("✓ Caso 1: Buffer vazio retorna 0 e mantém valor do bit.\n");

    // 2. Colocar 1 bit e tentar ler 2
    uint8_t input_bit = 1;
    put_bits(rb, &input_bit);
    
    res = get_bits(rb, &bit);
    assert(res == 1);
    assert(bit == 1);
    
    bit = 77;
    res = get_bits(rb, &bit);
    assert(res == 0);
    assert(bit == 77);
    printf("✓ Caso 2: Underrun após esvaziar buffer detectado.\n");

    free(rb);
    printf("✓ SUCESSO: Teste de underrun concluído.\n");
    return 0;
}
