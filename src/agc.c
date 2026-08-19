#include "../include/agc.h"
#include "../include/log.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>

static void agc_calibrate(AGCState *agc, EchoProtocol *echo, AudioState *audio, time_t now);
static void agc_steady(AGCState *agc, EchoProtocol *echo, AudioState *audio, time_t now);
static void agc_apply_gains(AudioState *audio, float tx_gain, float rx_gain);
static float agc_clamp(float v, float min, float max);

void agc_init(AGCState *agc) {
    memset(agc, 0, sizeof(*agc));
    agc->tx_gain_min = 0.1f;
    agc->tx_gain_max = 3.0f;
    agc->rx_gain_min = 1.0f;
    agc->rx_gain_max = 16.0f;
    agc->rms_min = 0.05f;
    agc->rms_max = 0.7f;
    agc->tx_step = 0.7f;
    agc->rx_step = 1.5f;
    agc->echo_sync_thresh = 3;
    agc->settle_secs = 5;
    agc->enabled = 1;
    agc->phase = AGC_CALIBRATING;
    agc->calib_tx_gain_step = 0;
    agc->calib_rx_gain_step = 0;
    agc->best_rms = 0.0f;
    agc->best_tx_gain = 1.0f;
    agc->best_rx_gain = 1.0f;
    agc->calib_done = 0;
    const char *e = getenv("ECHO_AGC");
    if (e && strcmp(e, "0") == 0) agc->enabled = 0;
    if (agc->enabled) {
        log_info("agc | habilitado | tx %.1f..%.1f | rx %.1f..%.1f | calibracao rapida ativa",
                 agc->tx_gain_min, agc->tx_gain_max,
                 agc->rx_gain_min, agc->rx_gain_max);
    } else {
        log_warn("agc | desabilitado (ECHO_AGC=0)");
    }
}

void agc_tune(AGCState *agc, EchoProtocol *echo, AudioState *audio) {
    if (!agc->enabled) return;

    time_t now = time(NULL);

    if (agc->calib_start_time == 0) {
        agc->calib_start_time = now;
    }
    agc->calib_secs_elapsed = (int)(now - agc->calib_start_time);

    /* Transicao: calibracao -> steady apos 20s OU convergencia */
    if (agc->phase == AGC_CALIBRATING && (agc->calib_secs_elapsed >= 20 || agc->calib_done)) {
        agc->phase = AGC_STEADY;
        agc->settle_secs = 5;
        log_info("agc | calibracao concluida em %ds | best_rms=%.3f | tx_gain=%.2f | rx_gain=%.2f",
                 agc->calib_secs_elapsed, agc->best_rms, agc->best_tx_gain, agc->best_rx_gain);
    }

    if (agc->phase == AGC_CALIBRATING) {
        agc_calibrate(agc, echo, audio, now);
    } else {
        agc_steady(agc, echo, audio, now);
    }
}

static void agc_calibrate(AGCState *agc, EchoProtocol *echo, AudioState *audio, time_t now) {
    /* Ajuste rapido a cada 1-2 segundos durante calibracao */
    int calib_interval = (agc->calib_secs_elapsed < 5) ? 1 : 2;
    if (now - agc->last_adjust < calib_interval) return;

    float rms = atomic_load(&audio->in_rms);

    /* EMA do RMS */
    if (agc->rms_avg == 0.0f) agc->rms_avg = rms;
    else agc->rms_avg = 0.8f * agc->rms_avg + 0.2f * rms;
    rms = agc->rms_avg;

    uint64_t d_sync = echo->stats.rx_sync_found - agc->last_rx_sync;
    uint64_t d_pkts = echo->stats.rx_packets - agc->last_rx_packets;
    uint64_t d_corrupt = echo->stats.rx_corrupted - agc->last_rx_corrupted;

    agc->last_rx_sync = echo->stats.rx_sync_found;
    agc->last_rx_packets = echo->stats.rx_packets;
    agc->last_rx_corrupted = echo->stats.rx_corrupted;

    log_info("agc calib | t=%ds | rms=%.3f | sync=%llu | pkts=%llu | corrupt=%llu | tx_gain=%.2f | rx_gain=%.2f",
             agc->calib_secs_elapsed, rms,
             (unsigned long long)d_sync, (unsigned long long)d_pkts, (unsigned long long)d_corrupt,
             audio->tx_gain, audio->rx_gain);

    /* Se ja tem pacotes bons -> convergiu */
    if (d_pkts > 0 && rms >= agc->rms_min && rms <= agc->rms_max) {
        agc->best_rms = rms;
        agc->best_tx_gain = audio->tx_gain;
        agc->best_rx_gain = audio->rx_gain;
        agc->calib_done = 1;
        agc->last_adjust = now;
        return;
    }

    /* 1) CLIP no ADC: rms alto demais -> baixa RX gain imediatamente */
    if (rms > agc->rms_max && audio->rx_gain > agc->rx_gain_min) {
        float new_rx = agc_clamp(audio->rx_gain / agc->rx_step, agc->rx_gain_min, agc->rx_gain_max);
        agc_apply_gains(audio, audio->tx_gain, new_rx);
        log_warn("agc calib | clip detectado | rms=%.3f > %.3f | rx_gain %.2f->%.2f",
                 rms, agc->rms_max, audio->rx_gain, new_rx);
        agc->last_adjust = now;
        return;
    }

    /* 2) AUTO-ECHO durante calibracao: syncs sem pacotes -> baixa TX gain */
    if (d_sync > 0 && d_pkts == 0 && d_corrupt >= d_sync) {
        agc->echo_streak++;
        if (agc->echo_streak >= agc->echo_sync_thresh && audio->tx_gain > agc->tx_gain_min) {
            float new_tx = agc_clamp(audio->tx_gain * agc->tx_step, agc->tx_gain_min, agc->tx_gain_max);
            agc_apply_gains(audio, new_tx, audio->rx_gain);
            log_warn("agc calib | auto-echo | streak=%d | tx_gain %.2f->%.2f",
                     agc->echo_streak, audio->tx_gain, new_tx);
            agc->echo_streak = 0;
            agc->last_adjust = now;
            return;
        }
    } else if (d_pkts > 0) {
        agc->echo_streak = 0;  /* reset com pacote bom */
    }

    /* 3) Ruido excessivo sem sync: sinal fraco ou ganho alto demais -> ajusta */
    if (d_sync == 0 && rms > agc->rms_min && rms < agc->rms_max && audio->rx_gain > agc->rx_gain_min) {
        /* RMS no meio mas sem sync = ruído -> tenta baixar RX gain */
        float new_rx = agc_clamp(audio->rx_gain * 0.8f, agc->rx_gain_min, agc->rx_gain_max);
        agc_apply_gains(audio, audio->tx_gain, new_rx);
        log_info("agc calib | ruído sem sync | rms=%.3f | rx_gain %.2f->%.2f",
                 rms, audio->rx_gain, new_rx);
        agc->last_adjust = now;
        return;
    }

    /* Estrategia de busca em grade: varia TX gain primeiro, depois RX gain */
    if (agc->calib_tx_gain_step < 5) {
        /* Sobe TX gain em passos para alcancar o peer */
        float new_tx = agc_clamp(audio->tx_gain * 1.3f, agc->tx_gain_min, agc->tx_gain_max);
        if (new_tx != audio->tx_gain) {
            agc_apply_gains(audio, new_tx, audio->rx_gain);
            agc->calib_tx_gain_step++;
            agc->last_adjust = now;
            log_info("agc calib | TX gain up %.2f -> %.2f (step %d/5)",
                     audio->tx_gain, new_tx, agc->calib_tx_gain_step);
            return;
        }
    }

    if (agc->calib_rx_gain_step < 5) {
        /* Sobe RX gain para captar melhor */
        float new_rx = agc_clamp(audio->rx_gain * 1.4f, agc->rx_gain_min, agc->rx_gain_max);
        if (new_rx != audio->rx_gain) {
            agc_apply_gains(audio, audio->tx_gain, new_rx);
            agc->calib_rx_gain_step++;
            agc->last_adjust = now;
            log_info("agc calib | RX gain up %.2f -> %.2f (step %d/5)",
                     audio->rx_gain, new_rx, agc->calib_rx_gain_step);
            return;
        }
    }

    /* Se chegou no teto de ambos e ainda nao tem pacotes -> reset e tenta combo diferente */
    if (audio->tx_gain >= agc->tx_gain_max * 0.95f && audio->rx_gain >= agc->rx_gain_max * 0.95f) {
        log_warn("agc calib | teto atingido sem link -> reset gains para base");
        agc_apply_gains(audio, 0.5f, 4.0f);
        agc->calib_tx_gain_step = 0;
        agc->calib_rx_gain_step = 0;
        agc->last_adjust = now;
        return;
    }

    agc->last_adjust = now;
}

static void agc_steady(AGCState *agc, EchoProtocol *echo, AudioState *audio, time_t now) {
    if (now - agc->last_adjust < agc->settle_secs) return;

    uint64_t d_sync = echo->stats.rx_sync_found - agc->last_rx_sync;
    uint64_t d_pkts = echo->stats.rx_packets - agc->last_rx_packets;
    uint64_t d_corrupt = echo->stats.rx_corrupted - agc->last_rx_corrupted;
    agc->last_rx_sync = echo->stats.rx_sync_found;
    agc->last_rx_packets = echo->stats.rx_packets;
    agc->last_rx_corrupted = echo->stats.rx_corrupted;

    float rms = atomic_load(&audio->in_rms);
    if (agc->rms_avg == 0.0f) agc->rms_avg = rms;
    else agc->rms_avg = 0.7f * agc->rms_avg + 0.3f * rms;
    rms = agc->rms_avg;

    /* 1) Auto-echo: syncs corrompidas sem pacotes bons */
    if (d_pkts > 0) {
        agc->echo_streak = 0;
    } else if (d_sync > 0 && d_corrupt >= d_sync) {
        agc->echo_streak++;
    }

    if (agc->echo_streak >= agc->echo_sync_thresh) {
        agc->echo_streak = 0;
        if (rms >= agc->rms_min && audio->tx_gain > agc->tx_gain_min) {
            float new_tx = agc_clamp(audio->tx_gain * agc->tx_step, agc->tx_gain_min, agc->tx_gain_max);
            agc_apply_gains(audio, new_tx, audio->rx_gain);
            log_warn("agc steady | auto-eco | rms=%.3f | tx_gain %.2f->%.2f", rms, audio->tx_gain, new_tx);
            agc->last_adjust = now;
            return;
        }
        if (rms < agc->rms_min && audio->rx_gain < agc->rx_gain_max) {
            float new_rx = agc_clamp(audio->rx_gain * agc->rx_step, agc->rx_gain_min, agc->rx_gain_max);
            agc_apply_gains(audio, audio->tx_gain, new_rx);
            log_info("agc steady | sinal fraco (pos eco) | rms=%.3f | rx_gain %.2f->%.2f", rms, audio->rx_gain, new_rx);
            agc->last_adjust = now;
            return;
        }
    }

    /* 2) Clip no ADC */
    if (rms > agc->rms_max && audio->rx_gain > agc->rx_gain_min) {
        float new_rx = agc_clamp(audio->rx_gain / agc->rx_step, agc->rx_gain_min, agc->rx_gain_max);
        agc_apply_gains(audio, audio->tx_gain, new_rx);
        log_warn("agc steady | clip | rms=%.3f | rx_gain %.2f->%.2f", rms, audio->rx_gain, new_rx);
        agc->last_adjust = now;
        return;
    }

    /* 3) Silencio: nenhum sinal do peer */
    if (d_sync == 0 && d_pkts == 0 && rms < agc->rms_min) {
        if (audio->rx_gain < agc->rx_gain_max) {
            float new_rx = agc_clamp(audio->rx_gain * agc->rx_step, agc->rx_gain_min, agc->rx_gain_max);
            agc_apply_gains(audio, audio->tx_gain, new_rx);
            log_info("agc steady | silencio | rms=%.3f | rx_gain %.2f->%.2f", rms, audio->rx_gain, new_rx);
            agc->last_adjust = now;
            return;
        }
        if (audio->tx_gain < agc->tx_gain_max) {
            float new_tx = agc_clamp(audio->tx_gain * 1.2f, agc->tx_gain_min, agc->tx_gain_max);
            agc_apply_gains(audio, new_tx, audio->rx_gain);
            log_info("agc steady | silencio (rx teto) | tx_gain %.2f->%.2f", audio->tx_gain, new_tx);
            agc->last_adjust = now;
            return;
        }
    }

    /* 4) Link OK: mantem ganhos, so atualiza timestamp */
    agc->last_adjust = now;
}

static void agc_apply_gains(AudioState *audio, float tx_gain, float rx_gain) {
    audio->tx_gain = tx_gain;
    audio->rx_gain = rx_gain;
}

void agc_set_initial_gains(AudioState *audio, float tx_gain, float rx_gain) {
    audio->tx_gain = tx_gain;
    audio->rx_gain = rx_gain;
}

static float agc_clamp(float v, float min, float max) {
    if (v < min) return min;
    if (v > max) return max;
    return v;
}