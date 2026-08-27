#include "../include/fec.h"
#include <string.h>
#include <stdlib.h>

/* Reed-Solomon sobre GF(2^8), polinomio primitivo 0x11D (o do ggwave).
   Codigo sistematico: k bytes de dados + ecc bytes de paridade; corrige ate
   t = ecc/2 erros de simbolo (byte). Decodificador classico: sindromes,
   Berlekamp-Massey (locator), Chien (posicoes) e Forney (magnitudes). */

static uint8_t gf_exp[512];
static uint8_t gf_log[256];
static int gf_ready = 0;

static void gf_init(void) {
    if (gf_ready) return;
    int x = 1;
    for (int i = 0; i < 255; i++) {
        gf_exp[i] = (uint8_t)x;
        gf_log[x] = (uint8_t)i;
        x <<= 1;
        if (x & 0x100) x ^= FEC_GF_POLY;
    }
    for (int i = 255; i < 512; i++) gf_exp[i] = gf_exp[i - 255];
    gf_ready = 1;
}

static uint8_t gf_mul(uint8_t a, uint8_t b) {
    if (a == 0 || b == 0) return 0;
    return gf_exp[gf_log[a] + gf_log[b]];
}

static uint8_t gf_inv(uint8_t a) {
    if (a == 0) return 0;
    return gf_exp[255 - gf_log[a]];
}

static uint8_t gf_pow(uint8_t a, int e) {
    if (a == 0) return e == 0 ? 1 : 0;
    return gf_exp[(gf_log[a] * e) % 255];
}

static uint8_t gf_poly_eval(const uint8_t *poly, int deg, uint8_t x) {
    uint8_t y = poly[deg];
    for (int i = deg - 1; i >= 0; i--) y = gf_mul(y, x) ^ poly[i];
    return y;
}

/* Polinomio gerador monico: gen[0]=1, gen[j] eh o coef de x^j.
   g(x) = produto (x - alpha^i), i = 0..ecc-1. */
static void rs_generator_poly(int ecc, uint8_t *gen) {
    memset(gen, 0, 256);
    gen[0] = 1;
    int deg = 0;
    for (int i = 0; i < ecc; i++) {
        /* gen *= (x + root): new_j = old_{j-1} + root*old_j, new_0 = root*old_0 */
        uint8_t root = gf_exp[i];
        for (int j = deg + 1; j >= 1; j--)
            gen[j] = gen[j - 1] ^ gf_mul(gen[j], root);
        gen[0] = gf_mul(gen[0], root);
        deg++;
    }
}

static int rs_encode_block(const uint8_t *data, int k, int ecc, uint8_t *out) {
    int n = k + ecc;
    uint8_t gen[256];
    uint8_t msg[FEC_RS_MSGBLK];
    /* Copia local: out pode ser o mesmo buffer que data (multi-bloco) e o
       memset abaixo zeraria a mensagem antes da restauracao. */
    memcpy(msg, data, (size_t)k);
    memset(out, 0, (size_t)n);
    memcpy(out, msg, (size_t)k);
    rs_generator_poly(ecc, gen);
    /* Divisao longa: cancela o termo lider de M(x)*x^ecc e deixa o resto na
       cauda. gen[j] eh o coef de x^j (ascendente); a posicao i+m do array
       (coef de x^(n-1-i-m)) recebe gen[ecc-m]. No final a mensagem volta ao
       inicio (codigo sistematico). */
    for (int i = 0; i < k; i++) {
        uint8_t coef = out[i];
        if (coef == 0) continue;
        for (int m = 0; m <= ecc; m++)
            out[i + m] ^= gf_mul(gen[ecc - m], coef);
    }
    memcpy(out, msg, (size_t)k);
    return n;
}

/* Sindromes S_i = R(alpha^i), i = 0..ecc-1. Retorna 0 se todas nulas. */
static int rs_syndromes(const uint8_t *r, int n, int ecc, uint8_t *synd) {
    int nonzero = 0;
    for (int i = 0; i < ecc; i++) {
        uint8_t x = gf_exp[i];
        uint8_t s = r[0];
        for (int j = 1; j < n; j++) s = gf_mul(s, x) ^ r[j];
        synd[i] = s;
        if (s) nonzero = 1;
    }
    return nonzero;
}

/* Berlekamp-Massey: locator Lambda(x) = 1 + C[1]x + ... + C[L]x^L, grau L. */
static void rs_find_error_locator(const uint8_t *synd, int ecc,
                                  uint8_t *loc, int *loc_deg) {
    uint8_t C[256], B[256], T[256];
    memset(C, 0, sizeof(C));
    memset(B, 0, sizeof(B));
    C[0] = 1;
    B[0] = 1;
    int L = 0, m = 1;
    uint8_t b = 1;
    for (int n = 0; n < ecc; n++) {
        uint8_t d = synd[n];
        for (int i = 1; i <= L; i++) d ^= gf_mul(C[i], synd[n - i]);
        if (d == 0) {
            m++;
            continue;
        }
        memcpy(T, C, sizeof(T));
        uint8_t coef = gf_mul(d, gf_inv(b));
        for (int i = 0; i + m < 256; i++) C[i + m] ^= gf_mul(coef, B[i]);
        if (2 * L <= n) {
            L = n + 1 - L;
            memcpy(B, T, sizeof(B));
            b = d;
            m = 1;
        } else {
            m++;
        }
    }
    *loc_deg = L;
    memcpy(loc, C, sizeof(C));
}

/* Omega(xinv) = (S(x)*Lambda(x) mod x^ecc) avaliado em y. */
static uint8_t rs_omega_eval(const uint8_t *synd, int ecc,
                             const uint8_t *loc, int loc_deg, uint8_t y) {
    uint8_t acc = 0;
    for (int t = 0; t < ecc; t++) {
        uint8_t omega_t = 0;
        for (int i = 0; i <= t; i++) {
            int j = t - i;
            if (j > loc_deg) continue;
            omega_t ^= gf_mul(synd[i], loc[j]);
        }
        acc ^= gf_mul(omega_t, gf_pow(y, t));
    }
    return acc;
}

/* Derivada formal de Lambda (em GF(2^8) so os graus impares sobrevivem). */
static uint8_t rs_derivative_eval(const uint8_t *loc, int loc_deg, uint8_t y) {
    uint8_t acc = 0;
    for (int d = 1; d <= loc_deg; d += 2)
        acc ^= gf_mul(loc[d], gf_pow(y, d - 1));
    return acc;
}

/* Decodifica um bloco RS(n, k). Retorna 0 em sucesso (out recebe k bytes),
   -1 se incorrigivel. */
static int rs_decode_block(const uint8_t *r, int n, int k, uint8_t *out) {
    int ecc = n - k;
    uint8_t synd[256];
    if (rs_syndromes(r, n, ecc, synd) == 0) {
        memcpy(out, r, (size_t)k);
        return 0;
    }

    uint8_t loc[256];
    int loc_deg;
    rs_find_error_locator(synd, ecc, loc, &loc_deg);
    if (loc_deg == 0 || loc_deg > ecc / 2) return -1;

    /* Chien: erro na posicao j (coef de x^(n-1-j)) se Lambda(alpha^(-e)) == 0,
       e = n-1-j. X = alpha^e eh o numero do locator. */
    int nerr = 0;
    int err_pos[256];
    uint8_t err_num[256];
    for (int j = 0; j < n && nerr < 256; j++) {
        int e = (n - 1 - j) % 255;
        uint8_t x = gf_exp[(255 - e) % 255];
        if (gf_poly_eval(loc, loc_deg, x) == 0) {
            err_pos[nerr] = j;
            err_num[nerr] = gf_exp[e];
            nerr++;
        }
    }
    if (nerr != loc_deg) return -1;

    uint8_t buf[256];
    memcpy(buf, r, (size_t)n);
    for (int i = 0; i < nerr; i++) {
        uint8_t xinv = gf_inv(err_num[i]);
        uint8_t omega = rs_omega_eval(synd, ecc, loc, loc_deg, xinv);
        uint8_t deriv = rs_derivative_eval(loc, loc_deg, xinv);
        if (deriv == 0) return -1;
        uint8_t mag = gf_mul(gf_mul(err_num[i], omega), gf_inv(deriv));
        buf[err_pos[i]] ^= mag;
    }
    memcpy(out, buf, (size_t)k);
    return 0;
}

/* Interleaver de matriz: escrita por linha (blocos), leitura por coluna.
   No ar, bytes contiguos vem de blocos diferentes -> uma rajada de ate
   nblocks bytes contiguos atinge no maximo 1 byte por bloco (linha). */
static void fec_interleave(const uint8_t *blocks, int nblocks, int block_size,
                           uint8_t *out) {
    for (int c = 0; c < block_size; c++)
        for (int r = 0; r < nblocks; r++)
            out[c * nblocks + r] = blocks[r * block_size + c];
}

static void fec_deinterleave(const uint8_t *in, int nblocks, int block_size,
                             uint8_t *blocks) {
    for (int c = 0; c < block_size; c++)
        for (int r = 0; r < nblocks; r++)
            blocks[r * block_size + c] = in[c * nblocks + r];
}

int fec_ecc_bytes(int data_len) {
    if (data_len < 4) return 2;
    int e = 3 * (data_len / 5);
    return e < 6 ? 6 : e;
}

int fec_encoded_len(int data_len) {
    if (data_len <= 0) return -1;
    int ecc = fec_ecc_bytes(data_len);
    /* +2 for the data_len prefix (2 bytes) */
    if (data_len + ecc <= FEC_RS_MAX_N) return data_len + 2 + ecc;
    int nblocks = (data_len + FEC_RS_MSGBLK - 1) / FEC_RS_MSGBLK;
    return nblocks * FEC_RS_MAX_N + 2;
}

int fec_encode(const uint8_t *data, int data_len, uint8_t *out, int out_cap) {
    gf_init();
    if (data_len <= 0) return -1;
    int ecc = fec_ecc_bytes(data_len);
    
    /* Prefix original data_len (2 bytes, big-endian) */
    if (data_len + 2 + ecc > out_cap && data_len + ecc <= FEC_RS_MAX_N) return -1;
    if (data_len + 2 + ecc > out_cap && data_len + ecc > FEC_RS_MAX_N) return -1;
    
    out[0] = (data_len >> 8) & 0xFF;
    out[1] = data_len & 0xFF;
    
    if (data_len + ecc <= FEC_RS_MAX_N) {
        if (data_len + 2 + ecc > out_cap) return -1;
        int ret = rs_encode_block(data, data_len, ecc, out + 2);
        if (ret != data_len + ecc) return -1;
        return data_len + 2 + ecc;
    }
    
    int nblocks = (data_len + FEC_RS_MSGBLK - 1) / FEC_RS_MSGBLK;
    int block_size = FEC_RS_MAX_N;
    if (nblocks * block_size + 2 > out_cap) return -1;
    
    out[0] = (data_len >> 8) & 0xFF;
    out[1] = data_len & 0xFF;
    
    uint8_t *blocks = malloc((size_t)nblocks * (size_t)block_size);
    if (blocks == NULL) return -1;
    
    for (int r = 0; r < nblocks; r++) {
        int off = r * FEC_RS_MSGBLK;
        int k = data_len - off;
        if (k > FEC_RS_MSGBLK) k = FEC_RS_MSGBLK;
        uint8_t blk[FEC_RS_MAX_N];
        memset(blk, 0, sizeof(blk));
        memcpy(blk, data + off, (size_t)k);
        rs_encode_block(blk, FEC_RS_MSGBLK, FEC_RS_PARITY_BLK, blk);
        memcpy(blocks + r * block_size, blk, (size_t)block_size);
    }
    
    /* Interleave starting from offset 2 (after length prefix) */
    fec_interleave(blocks, nblocks, block_size, out + 2);
    free(blocks);
    return nblocks * block_size + 2;
}



int fec_decode(const uint8_t *in, int fec_len, uint8_t *out, int out_cap) {
    gf_init();
    if (fec_len < 3) return -1;  /* Need at least 2 bytes length + 1 byte data */
    
    /* Read original data_len from prefix (2 bytes, big-endian) */
    int data_len = (in[0] << 8) | in[1];
    if (data_len <= 0 || data_len > out_cap) return -1;
    
    /* Skip the 2-byte length prefix */
    const uint8_t *fec_data = in + 2;
    int fec_data_len = fec_len - 2;
    
    int ecc = fec_ecc_bytes(data_len);
    if (data_len + ecc <= FEC_RS_MAX_N) {
        if (fec_data_len != data_len + ecc) return -1;
        if (rs_decode_block(fec_data, fec_data_len, data_len, out) != 0) return -1;
        return data_len;
    }
    
    int nblocks = (data_len + FEC_RS_MSGBLK - 1) / FEC_RS_MSGBLK;
    int block_size = FEC_RS_MAX_N;
    if (fec_data_len != nblocks * FEC_RS_MAX_N) return -1;
    
    uint8_t *blocks = malloc((size_t)nblocks * (size_t)block_size);
    if (blocks == NULL) return -1;
    fec_deinterleave(fec_data, nblocks, block_size, blocks);
    int out_off = 0;
    for (int r = 0; r < nblocks; r++) {
        uint8_t dec[FEC_RS_MAX_N];
        if (rs_decode_block(blocks + r * block_size, FEC_RS_MAX_N,
                            FEC_RS_MSGBLK, dec) != 0) {
            free(blocks);
            return -1;
        }
        int remain = data_len - out_off;
        int cpy = remain > FEC_RS_MSGBLK ? FEC_RS_MSGBLK : remain;
        memcpy(out + out_off, dec, (size_t)cpy);
        out_off += cpy;
    }
    free(blocks);
    return out_off;
}

uint32_t fec_crc32(const uint8_t *data, size_t len) {
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int b = 0; b < 8; b++)
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}