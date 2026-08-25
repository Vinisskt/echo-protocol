#ifndef AUDIO_IO_H
#define AUDIO_IO_H

#include <portaudio.h>
#include <stdatomic.h>

/* Forward declaration to avoid circular dependency */
struct EchoProtocol_s;
typedef struct EchoProtocol_s EchoProtocol;

/* Janela de medição de RMS de entrada (pré-AGC). 256 amostras @ 48 kHz ≈ 5,3 ms,
   cabe folgado no preâmbulo (~16 ms) conforme DEM4-agc.md. */
#define AUDIO_RMS_WIN 256
#define AUDIO_DC_ALPHA 0.001f   /* EMA lenta para estimar e remover DC offset */

typedef struct {
    PaStream *stream;
    EchoProtocol *echo;
    float tx_gain;
    float rx_gain;
    _Atomic float in_rms;       /* nível RMS real de entrada (escrito no callback) */
    int agc_freeze;
    /* estado de medição de RMS (pré-AGC) */
    float in_rms_win[AUDIO_RMS_WIN];
    int   in_rms_idx;
    float in_rms_sum;
    float in_dc_ema;
} AudioState;

int audio_init(AudioState *audio, EchoProtocol *echo, int input_id, int output_id);
int audio_start(AudioState *audio);
void audio_close(AudioState *audio);
void audio_list_devices();

#endif
