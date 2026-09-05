#ifndef LEVEL_CONTROL_H
#define LEVEL_CONTROL_H

/* Escala de amplitude aplicada no callback de áudio (thread realtime):
 *   - TX: ganho do AGC multiplica a forma de onda 4-FSK antes do DAC.
 *   - RX: ganho do AGC scala a amostra antes da demodulação.
 * Funções puras (sem estado) -> testáveis isoladamente.
 *
 * TX_LIMIT protege o DAC de clipping duro: generate_fsk tem pico 0.5, então
 * qualquer tx_gain >= 2.0 satura em 0.99 (0.5*2.0 = 1.0 já cortaria).
 * RX_LIMIT é guarda de sanidade contra ganhos absurdos (máx. da AGC: +24 dB = 15.8x). */
#define TX_GAIN_LIMIT 0.99f
#define RX_GAIN_LIMIT 8.0f

float level_scale_tx(float sample, float tx_gain);
float level_scale_rx(float sample, float rx_gain);

#endif