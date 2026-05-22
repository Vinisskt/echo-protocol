#include <stdio.h>
#include <stdint.h>
#include <math.h>
#include "../include/echo_protocol.h"

/**
 * Teste de Unidade: rb_to_audio (Assinatura: void rb_to_audio(EchoProtocol *echo, uint8_t *bit))
 * 
 * Valida se a função atualiza os campos current_sin e current_cos 
 * dentro do estado mod_state do protocolo.
 */

int main() {
    printf("--- Teste de Unidade: rb_to_audio (Atualização de Estado) ---\n");

    EchoProtocol echo;
    pre_calc_afsk(&echo.mod_state);

    uint8_t bit = 1;

    printf("Chamando rb_to_audio 5 vezes para ver a evolução da onda (Bit MARK)...\n");
    for (int i = 0; i < 5; i++) {
        rb_to_audio(&echo, &bit);
        printf("Amostra %d: Sin = %f, Cos = %f\n", i+1, echo.mod_state.current_sin, echo.mod_state.current_cos);
        
        float magnitude = echo.mod_state.current_sin * echo.mod_state.current_sin + 
                         echo.mod_state.current_cos * echo.mod_state.current_cos;
        
        if (magnitude < 0.99f || magnitude > 1.01f) {
            printf("[FALHA] Integridade da fase perdida na amostra %d!\n", i+1);
            return 1;
        }
    }
    printf("✓ SUCESSO: Onda evoluindo com integridade de fase.\n");

    return 0;
}
