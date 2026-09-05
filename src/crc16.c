#include "../include/crc16.h"

/* CRC-16/CCITT-FALSE: poli 0x1021 (x^16+x^12+x^5+1, fator (x+1) incluso),
 * init 0xFFFF, sem reflexo e sem XOR final.
 * Garante: todo erro de 1 bit; todo erro de 2 bits (frame < 8192 bytes);
 * todo burst contiguo de ate 16 bits; todos os erros de paridade ímpar. */
uint16_t crc16_ccitt_carry(uint16_t crc, const uint8_t *data, int len) {
    for (int i = 0; i < len; i++) {
        crc ^= (uint16_t)data[i] << 8;
        for (int j = 0; j < 8; j++) {
            crc = (crc & 0x8000u)
                      ? (uint16_t)((crc << 1) ^ 0x1021u)
                      : (uint16_t)(crc << 1);
        }
    }
    return crc;
}

uint16_t crc16_ccitt(const uint8_t *data, int len) {
    return crc16_ccitt_carry(0xFFFFu, data, len);
}