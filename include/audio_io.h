#ifndef AUDIO_IO_H
#define AUDIO_IO_H

#include <portaudio.h>

/* Forward declaration to avoid circular dependency */
struct EchoProtocol_s;
typedef struct EchoProtocol_s EchoProtocol;

typedef struct {
    PaStream *stream;
    EchoProtocol *echo;
    float tx_gain;
    float rx_gain;
    float in_rms;
} AudioState;

int audio_init(AudioState *audio, EchoProtocol *echo, int input_id, int output_id);
int audio_start(AudioState *audio);
void audio_close(AudioState *audio);
void audio_list_devices();

#endif
