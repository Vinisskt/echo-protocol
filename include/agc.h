#ifndef AGC_H
#define AGC_H

#include "echo_protocol.h"
#include "audio_io.h"
#include <time.h>

typedef enum {
    AGC_CALIBRATING,   /* fase rapida: sonda enlace, ajusta ganho a cada 1-2s */
    AGC_STEADY         /* estado normal: ajusta so se necessario, a cada 5s */
} AGCPhase;

typedef struct {
    uint64_t last_tx_packets;
    uint64_t last_rx_sync;
    uint64_t last_rx_packets;
    uint64_t last_rx_corrupted;
    float tx_gain_min;
    float tx_gain_max;
    float rx_gain_min;
    float rx_gain_max;
    float rms_min;
    float rms_max;
    float tx_step;
    float rx_step;
    int echo_sync_thresh;
    int settle_secs;
    time_t last_adjust;
    int echo_streak;
    float rms_avg;
    int enabled;

    /* Calibracao rapida */
    AGCPhase phase;
    int calib_secs_elapsed;
    int calib_tx_gain_step;     /* passos de TX gain ja tentados */
    int calib_rx_gain_step;     /* passos de RX gain ja tentados */
    float best_rms;             /* melhor RMS observado */
    float best_tx_gain;         /* TX gain que deu melhor RMS no peer */
    float best_rx_gain;         /* RX gain que deu melhor SNR local */
    int calib_done;             /* flag: calibracao convergiu */
    time_t calib_start_time;
} AGCState;

void agc_init(AGCState *agc);
void agc_tune(AGCState *agc, EchoProtocol *echo, AudioState *audio);
void agc_set_initial_gains(AudioState *audio, float tx_gain, float rx_gain);

#endif