#ifndef CRC16_H
#define CRC16_H

#include <stdint.h>

/* CRC-16/CCITT-FALSE (poli 0x1021, init 0xFFFF, sem reflexo e sem XOR final).
 * Cobre header(2B) + payload de cada frame do link: todo erro de 1 bit, de 2
 * bits (frames < 8192 B) e burst de ate 16 bits e detectado. */
uint16_t crc16_ccitt(const uint8_t *data, int len);

/* Continua um CRC a partir de valor previo (segmentos nao contiguos). */
uint16_t crc16_ccitt_carry(uint16_t crc, const uint8_t *data, int len);

#endif