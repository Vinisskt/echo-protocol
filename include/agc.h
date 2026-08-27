#ifndef AGC_H
#define AGC_H

#include <time.h>
#include <stdint.h>
#include "audio_io.h"

/* Forward declare para EchoProtocol (evita include circular com echo_protocol.h) */
typedef struct EchoProtocol_s EchoProtocol;

typedef enum {
    AGC_CALIBRATING,   /* fase rapida: sonda enlace, ajusta ganho a cada 1-2s */
    AGC_STEADY,        /* estado normal: ajusta so se necessario, a cada 5s */
    AGC_LOCKED         /* ganho travado: link bom, nao mexe mais */
} AGCPhase;

struct AGCState {
    uint64_t last_tx_packets;
    uint64_t last_rx_sync;
    uint64_t last_rx_packets;
    uint64_t last_rx_corrupted;

    /* Ganhos em dB (linear em dB = laço linear, settling time independente de nível) */
    float tx_gain_db_min;    /* ex: -20 dB = 0.1 linear */
    float tx_gain_db_max;    /* ex: +10 dB = 3.16 linear */
    float rx_gain_db_min;    /* ex: 0 dB = 1.0 linear */
    float rx_gain_db_max;    /* ex: +24 dB = 16 linear */

    float rms_min;           /* abaixo disso: sinal fraco */
    float rms_max;           /* acima disso: clip/saturação */
    float target_db;         /* referência RMS alvo em dB (ex: -20 dB) */
    float loop_bw;           /* largura de banda do laço (ex: 0.001) */
    float alpha;             /* EMA alpha para detector potência */

    int echo_sync_thresh;
    int settle_secs;
    time_t last_adjust;
    int echo_streak;
    float power_avg;         /* EMA da potência (RMS²) */
    int enabled;

    /* Calibração rápida (multiplicativo linear, mais rápido para busca inicial) */
    AGCPhase phase;
    int calib_secs_elapsed;
    int calib_tx_gain_step;
    int calib_rx_gain_step;
    float best_rms;
    float best_tx_gain;
    float best_rx_gain;
    int calib_done;
    time_t calib_start_time;

    /* Lock de ganho (link estavel) */
    int lock_good_streak;       /* pacotes bons consecutivos */
    int lock_threshold;         /* quantos pacotes bons para travar (ex: 20) */
    int locked;                 /* flag: ganho travado */
    float locked_tx_gain_db;    /* ganho TX travado */
    float locked_rx_gain_db;    /* ganho RX travado */
    int unlock_corrupt_thresh;  /* corrupções absolutas para destravar (ex: 5) */
    int unlock_rssi_drop_db;    /* queda RSSI para destravar (ex: 6 dB) */
    time_t last_unlock_time;    /* timestamp do último unlock (cooldown) */
    int unlock_cooldown_secs;   /* segundos de cooldown antes de re-lock (ex: 30) */
};

typedef struct AGCState AGCState;

void agc_init(AGCState *agc);
void agc_tune(AGCState *agc, EchoProtocol *echo, AudioState *audio);
void agc_set_initial_gains(AudioState *audio, float tx_gain, float rx_gain);

#endif