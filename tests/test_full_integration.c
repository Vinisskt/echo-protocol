#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "../include/rb_bits.h"
#include "../include/mod_afsk.h"
#include "../include/demod_afsk.h"

int main() {
    printf("--- Teste de Integração Total: Pipeline Completo ---\n");

    // 1. Inicialização de todos os componentes
    Buffer *tx_rb = rb_init();
    Buffer *rx_rb = rb_init();
    
    StateAFSK mod_state;
    pre_calc_afsk(&mod_state);

    StateGoertzel space, mark;
    uint16_t f1200 = 1200;
    uint16_t f2400 = 2400;
    pre_calc_goertzel(&space, &f1200);
    pre_calc_goertzel(&mark, &f2400);

    // 2. Preparar dados de teste (Payload)
    char *payload = "EchoProtocol";
    int payload_len = strlen(payload);
    printf("Payload original: %s (%d bytes)\n", payload, payload_len);

    // Inserir payload no Ring Buffer de transmissão
    for (int i = 0; i < payload_len; i++) {
        uint8_t byte = (uint8_t)payload[i];
        for (int b = 7; b >= 0; b--) {
            uint8_t bit = (byte >> b) & 1;
            put_bits(tx_rb, &bit);
        }
    }

    // 3. Loop de Processamento (Pipeline)
    printf("Iniciando processamento do pipeline...\n");
    uint8_t tx_bit;
    uint32_t bits_processed = 0;

    while (get_bits(tx_rb, &tx_bit)) {
        // A cada bit, resetamos o Goertzel
        space.q1 = 0; space.q2 = 0;
        mark.q1 = 0; mark.q2 = 0;

        float last_mag_s = 0, last_mag_m = 0;

        // Modulação e Demodulação Simultânea
        for (int i = 0; i < SAMPLES_PER_BIT; i++) {
            generate_afsk(&mod_state, &tx_bit);
            float sample = mod_state.current_sin;

            // Alimenta o demodulador com a amostra gerada
            last_mag_s = process_goertzel(&space, &sample);
            last_mag_m = process_goertzel(&mark, &sample);
        }

        // Decisão do Bit
        uint8_t rx_bit = (last_mag_m > last_mag_s) ? 1 : 0;
        
        // Inserir bit recuperado no Ring Buffer de recepção
        put_bits(rx_rb, &rx_bit);
        bits_processed++;
    }

    printf("Processamento concluído: %u bits convertidos.\n", bits_processed);

    // 4. Verificação Final
    printf("Recuperando dados do buffer de recepção...\n");
    char recovered_payload[payload_len + 1];
    memset(recovered_payload, 0, sizeof(recovered_payload));

    for (int i = 0; i < payload_len; i++) {
        uint8_t byte = 0;
        uint8_t bit;
        for (int b = 0; b < 8; b++) {
            if (get_bits(rx_rb, &bit)) {
                byte = (byte << 1) | bit;
            }
        }
        recovered_payload[i] = (char)byte;
    }

    printf("Payload recuperado: %s\n", recovered_payload);

    if (strcmp(payload, recovered_payload) == 0) {
        printf("✓ SUCESSO: Integração total validada (TX -> Mod -> Demod -> RX)\n");
    } else {
        printf("[FALHA] Os dados recuperados não conferem com o original.\n");
        return 1;
    }

    free(tx_rb);
    free(rx_rb);
    return 0;
}
