#include <stdio.h>
#include <stdint.h>
#include <math.h>
#include "../include/demod_afsk.h"
#include "../include/mod_afsk.h"

int main() {
    printf("--- Teste de Integração: Modulador -> Demodulador (8 bits) ---\n");

    StateAFSK mod_state;
    pre_calc_afsk(&mod_state);

    StateGoertzel space, mark;
    uint16_t f1200 = 1200;
    uint16_t f2400 = 2400;
    pre_calc_goertzel(&space, &f1200);
    pre_calc_goertzel(&mark, &f2400);

    uint8_t original_byte = 0xA5;
    uint8_t decoded_byte = 0;
    
    printf("Transmitindo byte: 0x%02X\n", original_byte);

    for (int b = 7; b >= 0; b--) {
        uint8_t bit = (original_byte >> b) & 1;
        float mag_s = 0, mag_m = 0;

        space.q1 = 0; space.q2 = 0;
        mark.q1 = 0; mark.q2 = 0;

        for (int i = 0; i < SAMPLES_PER_BIT; i++) {
            generate_afsk(&mod_state, &bit);
            float sample = mod_state.current_sin;
            mag_s = process_goertzel(&space, &sample);
            mag_m = process_goertzel(&mark, &sample);
        }

        uint8_t detected_bit = (mag_m > mag_s) ? 1 : 0;
        decoded_byte = (decoded_byte << 1) | detected_bit;
        
        printf("  Bit %d: Enviado=%d, Detectado=%d (MagS: %.2f, MagM: %.2f)\n", 
                7-b, bit, detected_bit, mag_s, mag_m);
    }

    printf("Byte decodificado: 0x%02X\n", decoded_byte);

    if (decoded_byte == original_byte) {
        printf("✓ SUCESSO: O byte foi transmitido e recuperado perfeitamente!\n");
    } else {
        printf("[FALHA] O byte decodificado não confere com o original.\n");
        return 1;
    }

    return 0;
}
