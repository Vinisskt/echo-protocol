#include <stdio.h>
#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include "../include/mod_afsk.h"
#include "../include/rb_bits.h"

void test_push_preamble() {
    printf("Testando push_preamble: Verificando padrão de sincronismo...\n");

    Buffer *rb = rb_init();
    
    // Chama a função implementada pelo usuário
    push_preamble(rb);

    // O Preamble definido no header é 1010101010101010 (16 bits)
    // Vamos verificar se os bits saem do buffer no padrão correto.
    uint8_t bit;
    for (int i = 0; i < 16; i++) {
        int has_bit = get_bits(rb, &bit);
        assert(has_bit == 1);
        
        // O padrão esperado é alternado: 1, 0, 1, 0...
        uint8_t expected = (i % 2 == 0) ? 1 : 0;
        
        if (bit != expected) {
            printf("[FALHA] Preamble incorreto na posição %d: esperado %d, obtido %d\n", i, expected, bit);
            exit(1);
        }
    }

    printf("Preamble verificado com sucesso (16 bits alternados)!\n");
    free(rb);
}

void test_push_sync_word() {
    printf("Testando push_sync_word: Verificando marcador de início de frame (0x930B51DE)...\n");

    Buffer *rb = rb_init();
    
    // Chama a função que o usuário vai implementar
    push_sync_word(rb);

    uint32_t expected_sync = SYNC_WORD;
    uint8_t bit;

    for (int i = 31; i >= 0; i--) {
        int has_bit = get_bits(rb, &bit);
        assert(has_bit == 1);
        
        uint8_t expected_bit = (expected_sync >> i) & 1;
        
        if (bit != expected_bit) {
            printf("[FALHA] Sync Word incorreta na posição %d: esperado %d, obtido %d\n", 31-i, expected_bit, bit);
            exit(1);
        }
    }

    printf("Sync Word verificada com sucesso (32 bits corretos)!\n");
    free(rb);
}

int main() {
    printf("--- Testes de Cabeçalho AFSK: Preamble & Sync ---\n");
    test_push_preamble();
    test_push_sync_word();
    printf("\n✓ Todos os testes de cabeçalho passaram!\n");
    return 0;
}
