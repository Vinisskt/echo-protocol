#include <stdio.h>
#include <assert.h>
#include <stdlib.h>
#include "../include/rb_bits.h"

void test_put_bits_individual_bits() {
    printf("Iniciando teste de inserção bit-a-bit (Sucesso = 1)...\n");
    
    Buffer *rb = rb_init();
    assert(rb != NULL);

    uint8_t bits[] = {1, 0, 1, 0, 1, 0, 1, 0};
    
    for (int i = 0; i < 8; i++) {
        uint8_t res = put_bits(rb, &bits[i]);
        assert(res == 1); 
    }

    assert(rb->head == 0);
    assert(rb->count == 8);
    assert(rb->buf[rb->tail] == 0xAA);

    uint8_t extra_bit = 0;
    uint8_t res_extra = put_bits(rb, &extra_bit);
    assert(res_extra == 1);
    assert(rb->head == 1);
    assert(rb->count == 0);

    printf("Teste de inserção bit-a-bit passou!\n");
    free(rb);
}

void test_rb_full() {
    printf("Iniciando teste de buffer cheio...\n");
    
    Buffer *rb = rb_init();
    assert(rb != NULL);

    // O buffer tem BUFFER_SIZE (1024)
    // Na sua implementação, a cada 8 bits inseridos, o head aumenta em 1 (na chamada seguinte).
    // O check de full é: ((buf->buf[buf->head] + 1) & BUFFER_MASK) == buf->tail
    
    // ATENÇÃO: Notei uma possível inconsistência na sua lógica de 'full':
    // Você está usando `buf->buf[buf->head]` (o conteúdo do buffer) no cálculo do índice.
    // Provavelmente você queria usar `buf->head` diretamente?
    // ((buf->head + 1) & BUFFER_MASK) == buf->tail
    
    // Vou simular o preenchimento de muitos bits para ver o comportamento do retorno 0.
    uint8_t bit = 1;
    int success_count = 0;
    
    // Inserindo bits até que a função retorne 0 (cheio)
    // Limitamos a um número razoável para evitar loop infinito se houver bug
    for (int i = 0; i < BUFFER_SIZE * 9; i++) {
        uint8_t res = put_bits(rb, &bit);
        if (res == 0) {
            printf("Buffer detectado como cheio após %d bits.\n", i);
            success_count = i;
            break;
        }
    }

    assert(success_count > 0);
    printf("Teste de buffer cheio concluído.\n");
    free(rb);
}

int main() {
    test_put_bits_individual_bits();
    test_rb_full();
    return 0;
}
