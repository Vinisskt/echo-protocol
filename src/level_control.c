#include "../include/level_control.h"
#include <math.h>

static float clamp_abs(float v, float limit) {
    if (isnan(v) || isinf(v)) return 0.0f;
    if (v > limit)  return limit;
    if (v < -limit) return -limit;
    return v;
}

float level_scale_tx(float sample, float tx_gain) {
    return clamp_abs(sample * tx_gain, TX_GAIN_LIMIT);
}

float level_scale_rx(float sample, float rx_gain) {
    return clamp_abs(sample * rx_gain, RX_GAIN_LIMIT);
}