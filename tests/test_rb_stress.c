#include <stdio.h>
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include "../include/rb_bits.h"

// Teste 1: Verifica perda de bits na fronteira de bytes (o 9º bit)
void test_bit_loss_at_boundary() {
    printf("--- Teste: Perda de bits na fronteira ---\n");
    Buffer *rb = rb_init();
    
    // Inserir 8 bits (Byte 1)
    uint8_t bit1 = 1;
    for(int i=0; i<8; i++) put_bits(rb, &bit1);
    
    // O 9º bit deveria ser o primeiro bit do SEGUNDO byte
    uint8_t bit_zero = 0;
    put_bits(rb, &bit_zero); 
    
    // Se o código atual retornar 1 mas não salvar o bit_zero, 
    // quando inserirmos o 10º bit, o bit_zero terá sumido.
    uint8_t bit_one = 1;
    put_bits(rb, &bit_one);

    // No seu código atual, após o 9º bit (que só incrementa head), 
    // o 10º bit será o primeiro a ser realmente escrito no buf[tail].
    // Vamos ver o que tem no segundo byte (que deveria estar começando a ser formado)
    printf("Estado após 10 bits: buf[0]=0x%02X, count_put=%d, head=%d\n", rb->buf[0], rb->count_put, rb->head);
    
    // Se não houver perda, o bit_zero e bit_one deveriam estar lá.
    // Como a função usa tail fixo em 0 e head avança, isso vai gerar confusão no buffer.
    free(rb);
}

// Teste 2: Verifica se lixo na memória do malloc corrompe os dados
void test_memory_garbage_pollution() {
    printf("--- Teste: Poluição por lixo de memória ---\n");
    Buffer *rb = rb_init();
    
    // Simula lixo na memória escrevendo algo no buffer manualmente
    memset(rb->buf, 0xFF, BUFFER_SIZE);
    rb->head = 0;
    rb->tail = 0;
    rb->count_put = 0;
    rb->count_get = 0;

    uint8_t bit = 0; // Queremos formar um byte 0x00
    for(int i=0; i<8; i++) put_bits(rb, &bit);

    printf("Byte formado com lixo prévio: 0x%02X (esperado 0x00)\n", rb->buf[rb->tail]);
    // Se o código não limpa o byte antes de começar o shift, 0xFF vira 0x00 após 8 shifts? 
    // Na verdade, 0xFF << 1 | 0 vira 0xFE, depois 0xFC... o lixo só sai após 8 bits.
    // Mas se você ler o byte antes dele completar 8 bits, ele estará sujo.
    
    free(rb);
}

// Teste 3: Stress de preenchimento total e verificação de integridade
void test_integrity_stress() {
    printf("--- Teste: Stress de Integridade ---\n");
    Buffer *rb = rb_init();
    uint8_t bit_set = 1;
    uint8_t bit_clr = 0;

    // Escreve 0xA (1010) e depois 0x5 (0101)
    put_bits(rb, &bit_set); // 1
    put_bits(rb, &bit_clr); // 0
    put_bits(rb, &bit_set); // 1
    put_bits(rb, &bit_clr); // 0
    
    put_bits(rb, &bit_clr); // 0
    put_bits(rb, &bit_set); // 1
    put_bits(rb, &bit_clr); // 0
    put_bits(rb, &bit_set); // 1

    printf("Byte final: 0x%02X (esperado 0xA5)\n", rb->buf[rb->tail]);
    assert(rb->buf[rb->tail] == 0xA5);

    free(rb);
}

int main() {
    test_bit_loss_at_boundary();
    test_memory_garbage_pollution();
    test_integrity_stress();
    return 0;
}
