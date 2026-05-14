#include <stdio.h>
#include <assert.h>
#include <stdlib.h>
#include "../include/rb_bits.h"

// Protótipo da função que o usuário está escrevendo
// Assumindo a assinatura baseada no pedido: uint8_t get_bits(Buffer *buf, uint8_t *bit);
uint8_t get_bits(Buffer *buf, uint8_t *bit);

void test_get_bits_basic() {
    printf("Iniciando teste básico de get_bits...\n");
    
    Buffer *rb = rb_init();
    assert(rb != NULL);

    // 1. Inserir um byte conhecido: 0xA5 (10100101)
    uint8_t bits_to_write[] = {1, 0, 1, 0, 0, 1, 0, 1};
    for (int i = 0; i < 8; i++) {
        put_bits(rb, &bits_to_write[i]);
    }

    // 2. Ler de volta bit a bit
    uint8_t bit_read;
    uint8_t res;
    
    printf("Lendo bits: ");
    for (int i = 0; i < 8; i++) {
        res = get_bits(rb, &bit_read);
        assert(res == 1); // Sucesso na leitura
        printf("%d", bit_read);
        assert(bit_read == bits_to_write[i]);
    }
    printf("\n");

    // 3. Verificar se o buffer está vazio agora
    // (Dependendo de como você implementou o retorno de buffer vazio)
    res = get_bits(rb, &bit_read);
    // Assumindo 0 para vazio, seguindo a lógica do put_bits (0 para cheio)
    assert(res == 0); 

    printf("Teste básico de get_bits passou!\n");
    free(rb);
}

void test_get_bits_sequence() {
    printf("Iniciando teste de sequência (múltiplos bytes)...\n");
    
    Buffer *rb = rb_init();
    
    // Escrever dois bytes: 0xFF e 0x00
    uint8_t b1 = 1;
    uint8_t b0 = 0;
    for(int i=0; i<8; i++) put_bits(rb, &b1);
    for(int i=0; i<8; i++) put_bits(rb, &b0);

    uint8_t bit_read;
    // Ler os primeiros 8 (devem ser 1)
    for(int i=0; i<8; i++) {
        get_bits(rb, &bit_read);
        assert(bit_read == 1);
    }
    // Ler os próximos 8 (devem ser 0)
    for(int i=0; i<8; i++) {
        get_bits(rb, &bit_read);
        assert(bit_read == 0);
    }

    printf("Teste de sequência de get_bits passou!\n");
    free(rb);
}

int main() {
    test_get_bits_basic();
    test_get_bits_sequence();
    return 0;
}
