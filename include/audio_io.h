#ifndef AUDIO_IO_H
#define AUDIO_IO_H

#include "echo_protocol.h"
#include <portaudio.h>

typedef struct {
    PaStream *stream;
    EchoProtocol *echo;
} AudioState;

int audio_init(AudioState *audio, EchoProtocol *echo);
int audio_start(AudioState *audio);
void audio_close(AudioState *audio);

#endif
