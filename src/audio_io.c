#include "../include/audio_io.h"
#include "../include/echo_protocol.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <dlfcn.h>

static uint8_t current_tx_symbol = 0;

static int paCallback(const void *inputBuffer, void *outputBuffer,
                      unsigned long framesPerBuffer,
                      const PaStreamCallbackTimeInfo* timeInfo,
                      PaStreamCallbackFlags statusFlags,
                      void *userData) {
    (void)timeInfo;
    (void)statusFlags;
    
    AudioState *audio = (AudioState *)userData;
    EchoProtocol *echo = audio->echo;
    float *out = (float*)outputBuffer;
    float *in = (float*)inputBuffer;

    static int tx_active_frames = 0;
    uint16_t tx_used = (echo->tx_rb->head - echo->tx_rb->tail) & BUFFER_MASK;
    int tx_active = (tx_used > 0);

    if (tx_active) {
        tx_active_frames = 3;
    } else if (tx_active_frames > 0) {
        tx_active_frames--;
    }
    audio->agc_freeze = (tx_active || tx_active_frames > 0);

    /* ALSA xrun/underrun (input overflow / output underflow) are tolerated:
       the callback keeps running and the link-layer FEC/ARQ recover the loss. */

    for (unsigned int i = 0; i < framesPerBuffer; i++) {
        if (in) {
            audio_to_rb(echo, &in[i]);
        }

        if (echo->tx.tx_sample_count >= SAMPLES_PER_SYMBOL) {
            uint8_t bits[2];
            if (get_bits(echo->tx_rb, &bits[0]) && get_bits(echo->tx_rb, &bits[1])) {
                current_tx_symbol = (bits[0] << 1) | bits[1];
            } else {
                current_tx_symbol = 3;
            }
            echo->tx.tx_sample_count = 0;
        }

        out[i] = generate_fsk(&echo->mod_state, &current_tx_symbol);
        echo->tx.tx_sample_count++;
    }

    return paContinue;
}

static void suppress_alsa_errors(void) {
    void *h = dlopen("libasound.so.2", RTLD_LAZY);
    if (!h) return;
    void (*set_handler)(void *) = dlsym(h, "snd_lib_error_set_handler");
    if (set_handler) set_handler(NULL);
}

int audio_init(AudioState *audio, EchoProtocol *echo, int input_id, int output_id) {
    PaError err;

    suppress_alsa_errors();
    err = Pa_Initialize();
    if (err != paNoError) {
        fprintf(stderr, "Erro ao inicializar PortAudio: %s\n", Pa_GetErrorText(err));
        return -1;
    }

    audio->echo = echo;
    audio->agc_freeze = 0;

    PaStreamParameters inputParams, outputParams;

    inputParams.device = (input_id >= 0) ? input_id : Pa_GetDefaultInputDevice();
    if (inputParams.device == paNoDevice) {
        fprintf(stderr, "Erro: Nenhum dispositivo de entrada encontrado.\n");
        return -1;
    }
    inputParams.channelCount = 1;
    inputParams.sampleFormat = paFloat32;
    inputParams.suggestedLatency = Pa_GetDeviceInfo(inputParams.device)->defaultLowInputLatency;
    inputParams.hostApiSpecificStreamInfo = NULL;

    outputParams.device = (output_id >= 0) ? output_id : Pa_GetDefaultOutputDevice();
    if (outputParams.device == paNoDevice) {
        fprintf(stderr, "Erro: Nenhum dispositivo de saída encontrado.\n");
        return -1;
    }
    outputParams.channelCount = 1;
    outputParams.sampleFormat = paFloat32;
    outputParams.suggestedLatency = Pa_GetDeviceInfo(outputParams.device)->defaultLowOutputLatency;
    outputParams.hostApiSpecificStreamInfo = NULL;

    printf("[Audio] Abrindo Entrada [%d] e Saída [%d]\n", inputParams.device, outputParams.device);

    err = Pa_OpenStream(&audio->stream,
                        &inputParams,
                        &outputParams,
                        SAMPLE_RATE,
                        256,
                        paNoFlag,
                        paCallback,
                        audio);

    if (err != paNoError) {
        fprintf(stderr, "Erro ao abrir stream: %s\n", Pa_GetErrorText(err));
        return -1;
    }

    return 0;
}

int audio_start(AudioState *audio) {
    PaError err = Pa_StartStream(audio->stream);
    if (err != paNoError) {
        fprintf(stderr, "Erro ao iniciar stream: %s\n", Pa_GetErrorText(err));
        return -1;
    }
    return 0;
}

void audio_close(AudioState *audio) {
    if (audio->stream) {
        Pa_StopStream(audio->stream);
        Pa_CloseStream(audio->stream);
    }
    Pa_Terminate();
}

void audio_list_devices() {
    PaError err = Pa_Initialize();
    if (err != paNoError) {
        printf("Erro ao inicializar PortAudio: %s\n", Pa_GetErrorText(err));
        return;
    }

    int numDevices = Pa_GetDeviceCount();
    if (numDevices < 0) {
        Pa_Terminate();
        printf("Erro ao obter dispositivos: %s\n", Pa_GetErrorText(numDevices));
        return;
    }

    printf("Dispositivos de Áudio Disponíveis:\n");
    for (int i = 0; i < numDevices; i++) {
        const PaDeviceInfo *deviceInfo = Pa_GetDeviceInfo(i);
        printf("[%d] %s (Entradas: %d, Saídas: %d)\n", i, deviceInfo->name,
               deviceInfo->maxInputChannels, deviceInfo->maxOutputChannels);
    }
    Pa_Terminate();
}
