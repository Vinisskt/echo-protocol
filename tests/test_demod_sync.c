#include <stdio.h>
#include <stdint.h>
#include <assert.h>
#include "../include/demod_afsk.h"
#include "../include/mod_afsk.h"

int main() {
    printf("--- Teste de Sincronismo: check_sync_word ---\n");

    uint32_t shift_reg = 0;
    uint8_t bit;
    uint8_t result;

    // 1. Simular bits de lixo
    printf("Inserindo bits de lixo...\n");
    for (int i = 0; i < 32; i++) {
        bit = i % 2;
        result = check_sync_word(&shift_reg, &bit);
        if (result == 0) {
            printf("[FALHA] Detectou Sync Word onde não devia!\n");
            return 1;
        }
    }

    // 2. Inserir a SYNC_WORD real (0x930B51DE)
    // No mod_afsk.c, a sync word é inserida do MSB para o LSB
    printf("Inserindo SYNC_WORD (0x%08X)...\n", SYNC_WORD);
    for (int i = 31; i >= 0; i--) {
        bit = (SYNC_WORD >> i) & 1;
        result = check_sync_word(&shift_reg, &bit);
        
        if (i > 0) {
            if (result == 0) {
                printf("[FALHA] Detectou Sync Word prematuramente no bit %d!\n", i);
                return 1;
            }
        } else {
            if (result != 0) {
                printf("[FALHA] Não detectou a Sync Word no último bit!\n");
                return 1;
            }
        }
    }

    printf("✓ SYNC_WORD detectada com sucesso no momento exato!\n");

    // 3. Verificar se continua funcionando após detecção (deslocando um bit extra)
    bit = 0;
    result = check_sync_word(&shift_reg, &bit);
    if (result == 0) {
        printf("[FALHA] Sync Word persistiu incorretamente após novo bit!\n");
        return 1;
    }

    printf("✓ Teste de persistência passou!\n");
    printf("\nTodos os testes de sincronismo passaram!\n");

    return 0;
}
