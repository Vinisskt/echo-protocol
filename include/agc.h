#ifndef AGC_H
#define AGC_H

#include "echo_protocol.h"
#include "audio_io.h"

/* Controlador de ganho dinamico (ver caderno: sincronizacao/codificacao-e-scrambling.md
   "AGC"). Neste modem a decisao e por dominancia relativa dos 4 Goertzels, entao
   volume absoluto ja e tolerado; o que derruba o RX e o AUTO-ECO (full-duplex:
   o proprio TX satura o proprio mic). O laço entao:
   - detecta falso lock por eco: rx_sync subindo sem rx_packets + rx_corrupted
     (bad_len absurdo) -> baixa TX gain (fator que cega o RX);
   - sem sinal do peer (rx_sync parado + in_rms no chao) -> sobe RX gain;
   - nunca mexe em TX gain estavel que ja esta recebendo (link ok). */

typedef struct {
    uint64_t last_tx_packets;
    uint64_t last_rx_sync;
    uint64_t last_rx_packets;
    uint64_t last_rx_corrupted;
    float tx_gain_min;
    float tx_gain_max;
    float rx_gain_min;
    float rx_gain_max;
    float rms_min;        /* abaixo disso o sinal do peer esta no chao */
    float rms_max;        /* acima disso ha clipping/saturacao */
    float tx_step;        /* fator de ajuste do TX gain */
    float rx_step;        /* fator de ajuste do RX gain */
    int echo_sync_thresh; /* syncs corrompidas consecutivas sem pacote bom => falso lock */
    int settle_secs;      /* espera entre ajustes consecutivos */
    time_t last_adjust;
    int echo_streak;      /* syncs corrompidas consecutivas sem pacote bom */
    float rms_avg;        /* EMA do RMS (filtro de laço do AGC) */
    int enabled;
} AGCState;

void agc_init(AGCState *agc);
void agc_tune(AGCState *agc, EchoProtocol *echo, AudioState *audio);

#endif