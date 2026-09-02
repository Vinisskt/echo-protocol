#ifndef AGC_H
#define AGC_H

#include <time.h>
#include <stdint.h>
#include "audio_io.h"

/* Forward declare para EchoProtocol (evita include circular com echo_protocol.h) */
typedef struct EchoProtocol_s EchoProtocol;

typedef enum {
    AGC_CALIBRATING,   /* fase rapida: sonda enlace, ajusta ganho a cada 1-2s */
    AGC_STEADY         /* estado normal: ajusta continuamente */
} AGCPhase;

struct AGCState {
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

    /* Calibração: busca binária adaptativa */
    AGCPhase phase;
    int calib_secs_elapsed;
    int calib_done;
    time_t calib_start_time;

    /* Busca binária — eixo TX */
    float calib_tx_low;
    float calib_tx_high;
    int   calib_tx_iter;
    int   calib_tx_good;       /* 1 = teste anterior foi bom */

    /* Busca binária — eixo RX */
    float calib_rx_low;
    float calib_rx_high;
    int   calib_rx_iter;
    int   calib_rx_good;

    /* Melhor combo encontrado */
    float best_rms;
    float best_tx_gain;
    float best_rx_gain;

    /* Steady-state: laço multiplicativo com hysteresis */
    float gain_smooth;         /* ganho RX suavizado (linear) */
    float target_power;        /* target_db convertido para potência linear */
    float hyst_margin;         /* margem da faixa morta (fração, ex: 0.2 = 20%) */
    float beta_attack;         /* taxa de correção para sinal alto */
    float beta_release;        /* taxa de correção para sinal baixo */

    /* Contador de ramp silencioso (limita quantas vezes TX sobe em sequência) */
    int silence_ramp_count;

    /* Freeze durante silêncio: congela TX e RX gains */
    int frozen;
};

typedef struct AGCState AGCState;

void agc_init(AGCState *agc);
void agc_tune(AGCState *agc, EchoProtocol *echo, AudioState *audio);
void agc_set_initial_gains(AudioState *audio, float tx_gain, float rx_gain);

#endif