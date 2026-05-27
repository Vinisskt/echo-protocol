#include <stdio.h>
#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include "../include/rb_bits.h"

int main() {
    printf("--- Teste: rb_init Zero State ---\n");
    
    Buffer *rb = rb_init();
    assert(rb != NULL);
    assert(rb->head == 0);
    assert(rb->tail == 0);
    assert(rb->count_put == 0);
    assert(rb->count_get == 0);
    
    // Verificar que o primeiro byte do buffer está zerado (importante para put_bits inicial)
    assert(rb->buf[0] == 0);
    
    printf("✓ Todos os campos inicializados corretamente.\n");
    
    free(rb);
    printf("✓ SUCESSO: Teste de inicialização concluído.\n");
    return 0;
}
