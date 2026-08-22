#ifndef ROHC_H
#define ROHC_H

#include <stdint.h>

#define ROHC_CTX_SIZE          40
#define ROHC_FLAG_IP_ID        0x01
#define ROHC_FLAG_TTL          0x02
#define ROHC_FLAG_TOS          0x04
#define ROHC_FLAG_FLAGS        0x08
#define ROHC_V6_FLAG_NEXT_HDR  0x01
#define ROHC_V6_FLAG_HOP_LIMIT 0x02
#define ROHC_V6_FLAG_TC        0x04
#define ROHC_VERSION_V6        0x80
#define ROHC_MAX_COMPRESSED    (1 + 1 + 2 + 1 + 2 + 2 + 2048)

typedef struct {
    uint8_t  context[ROHC_CTX_SIZE];
    uint8_t  context_valid;
} ROHCState;

void     rohc_init(ROHCState *state);
int      rohc_compress(ROHCState *state, const uint8_t *packet, int packet_len, uint8_t *out, int out_len);
int      rohc_decompress(ROHCState *state, const uint8_t *compressed, int comp_len, uint8_t *out, int out_len);
void     rohc_reset(ROHCState *state);
void     rohc_sync_context(ROHCState *state, const uint8_t *packet, int packet_len);
uint16_t ip_checksum(const uint8_t *header, int len);

#endif
