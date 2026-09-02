#ifndef FEC_H
#define FEC_H

#include <stdint.h>
#include <stddef.h>

/* FEC do modem acustico, seguindo a nota codigos_de_correcao_de_erros.md:
   - Reed-Solomon sobre GF(2^8) com polinomio primitivo 0x11D (o do ggwave).
   - Paridade adaptativa (estilo ggwave): ecc = len < 4 ? 2 : max(4, 2*(len/5)).
   - Bloco unico RS(n,k) quando cabe em 255; senao blocos RS(255,223).
   - Interleaver de matriz linhas x colunas (linhas = blocos RS): rajada no
     canal vira erros espalhados, cada linha recebe <= 1 erro por rajada curta.
   - CRC-32 de deteccao sobre o que o FEC nao conseguiu corrigir. */

#define FEC_GF_POLY         0x11D
#define FEC_RS_MAX_N        255
#define FEC_RS_MSGBLK       50
#define FEC_RS_PARITY_BLK   70
#define FEC_CRC_BYTES       4
#define FEC_LEN_BYTES       2
#define FEC_OVERHEAD        (FEC_LEN_BYTES + FEC_CRC_BYTES)

int  fec_ecc_bytes(int data_len);
int  fec_encoded_len(int data_len);
int  fec_encode(const uint8_t *data, int data_len, uint8_t *out, int out_cap);
int  fec_decode(const uint8_t *in, int fec_len, uint8_t *out, int out_cap);

#endif