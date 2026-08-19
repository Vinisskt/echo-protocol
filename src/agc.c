#include "../include/agc.h"
#include "../include/log.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>

static void agc_calibrate(AGCState *agc, EchoProtocol *echo, AudioState *audio, time_t now);
static void agc_steady(AGCState *agc, EchoProtocol *echo, AudioState *audio, time_t now);
static void agc_apply_gains(AudioState *audio, float tx_gain, float rx_gain);
static float agc_clamp(float v, float min, float max);
static float db_to_linear(float db);
static float linear_to_db(float linear);

void agc_init(AGCState *agc) {
    memset(agc, 0, sizeof(*agc));
    agc->tx_gain_db_min = -20.0f;   /* 0.1 linear */
    agc->tx_gain_db_max = 10.0f;    /* 3.16 linear */
    agc->rx_gain_db_min = 0.0f;     /* 1.0 linear */
    agc->rx_gain_db_max = 24.0f;    /* 16.0 linear */
    agc->rms_min = 0.05f;
    agc->rms_max = 0.7f;
    agc->target_db = -20.0f;        /* alvo: -20 dB RMS */
    agc->loop_bw = 0.001f;          /* largura de banda do laço */
    agc->alpha = 0.1f;              /* EMA alpha para potência */
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
    agc->power_avg = 0.0f;
    agc->lock_good_streak = 0;
    agc->lock_threshold = 20;        /* 20 pacotes bons = trava */
    agc->locked = 0;
    agc->unlock_corrupt_thresh = 5;  /* 5 corrupções = destrava */
    agc->unlock_rssi_drop_db = 6;    /* 6 dB queda = destrava */
    agc->last_unlock_time = 0;
    agc->unlock_cooldown_secs = 30;  /* 30s cooldown antes de re-lock */
    const char *e = getenv("ECHO_AGC");
    if (e && strcmp(e, "0") == 0) agc->enabled = 0;
    if (agc->enabled) {
        log_info("agc | habilitado | tx %.1f..%.1f dB | rx %.1f..%.1f dB | target %.1f dB | loop_bw %.4f | calibracao rapida ativa | lock=%d pkts",
                 agc->tx_gain_db_min, agc->tx_gain_db_max,
                 agc->rx_gain_db_min, agc->rx_gain_db_max,
                 agc->target_db, agc->loop_bw, agc->lock_threshold);
    } else {
        log_warn("agc | desabilitado (ECHO_AGC=0)");
    }
}

static float db_to_linear(float db) {
    return powf(10.0f, db / 20.0f);
}

static float linear_to_db(float linear) {
    if (linear <= 0.0f) return -120.0f;
    return 20.0f * log10f(linear);
}

void agc_set_initial_gains(AudioState *audio, float tx_gain, float rx_gain) {
    audio->tx_gain = tx_gain;
    audio->rx_gain = rx_gain;
}

void agc_tune(AGCState *agc, EchoProtocol *echo, AudioState *audio) {
    if (!agc->enabled) return;
    if (audio->agc_freeze) return;  // SIC básico: congela ganho durante TX local

    time_t now = time(NULL);

    if (agc->calib_start_time == 0) {
        agc->calib_start_time = now;
    }
    agc->calib_secs_elapsed = (int)(now - agc->calib_start_time);

    /* Transição: calibração -> steady após 20s OU convergência */
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
    int calib_interval = (agc->calib_secs_elapsed < 5) ? 1 : 2;
    if (now - agc->last_adjust < calib_interval) return;

    float rms = atomic_load(&audio->in_rms);

    /* EMA da potência para detector */
    float power = rms * rms;
    if (agc->power_avg == 0.0f) agc->power_avg = power;
    else agc->power_avg = (1.0f - agc->alpha) * agc->power_avg + agc->alpha * power;

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

    /* Se já tem pacotes bons -> convergiu */
    if (d_pkts > 0 && rms >= agc->rms_min && rms <= agc->rms_max) {
        agc->best_rms = rms;
        agc->best_tx_gain = audio->tx_gain;
        agc->best_rx_gain = audio->rx_gain;
        agc->calib_done = 1;
        agc->last_adjust = now;
        return;
    }

    /* 1) CLIP no ADC: rms alto demais -> baixa RX gain imediatamente */
    if (rms > agc->rms_max && audio->rx_gain > db_to_linear(agc->rx_gain_db_min)) {
        float rx_db = linear_to_db(audio->rx_gain);
        float new_rx_db = rx_db - 3.0f;  /* -3 dB step */
        if (new_rx_db < agc->rx_gain_db_min) new_rx_db = agc->rx_gain_db_min;
        float new_rx = db_to_linear(new_rx_db);
        agc_apply_gains(audio, audio->tx_gain, new_rx);
        log_warn("agc calib | clip detectado | rms=%.3f > %.3f | rx_gain %.2f->%.2f (%.1f->%.1f dB)",
                 rms, agc->rms_max, audio->rx_gain, new_rx, rx_db, new_rx_db);
        agc->last_adjust = now;
        return;
    }

    /* 2) AUTO-ECHO durante calibração: syncs sem pacotes -> baixa TX gain */
    if (d_sync > 0 && d_pkts == 0 && d_corrupt >= d_sync) {
        agc->echo_streak++;
        if (agc->echo_streak >= agc->echo_sync_thresh && audio->tx_gain > db_to_linear(agc->tx_gain_db_min)) {
            float tx_db = linear_to_db(audio->tx_gain);
            float new_tx_db = tx_db - 3.0f;
            if (new_tx_db < agc->tx_gain_db_min) new_tx_db = agc->tx_gain_db_min;
            float new_tx = db_to_linear(new_tx_db);
            agc_apply_gains(audio, new_tx, audio->rx_gain);
            log_warn("agc calib | auto-echo | streak=%d | tx_gain %.2f->%.2f (%.1f->%.1f dB)",
                     agc->echo_streak, audio->tx_gain, new_tx, tx_db, new_tx_db);
            agc->echo_streak = 0;
            agc->last_adjust = now;
            return;
        }
    } else if (d_pkts > 0) {
        agc->echo_streak = 0;
    }

    /* 3) Ruido excessivo sem sync: sinal fraco ou ganho alto demais -> ajusta */
    if (d_sync == 0 && rms > agc->rms_min && rms < agc->rms_max && audio->rx_gain > db_to_linear(agc->rx_gain_db_min)) {
        float rx_db = linear_to_db(audio->rx_gain);
        float new_rx_db = rx_db - 1.5f;  /* -1.5 dB step */
        if (new_rx_db < agc->rx_gain_db_min) new_rx_db = agc->rx_gain_db_min;
        float new_rx = db_to_linear(new_rx_db);
        agc_apply_gains(audio, audio->tx_gain, new_rx);
        log_info("agc calib | ruído sem sync | rms=%.3f | rx_gain %.2f->%.2f (%.1f->%.1f dB)",
                 rms, audio->rx_gain, new_rx, rx_db, new_rx_db);
        agc->last_adjust = now;
        return;
    }

    /* Estratégia de busca em grade (multiplicativo linear, mais rápido para busca inicial) */
    if (agc->calib_tx_gain_step < 5) {
        float new_tx = agc_clamp(audio->tx_gain * 1.3f, db_to_linear(agc->tx_gain_db_min), db_to_linear(agc->tx_gain_db_max));
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
        float new_rx = agc_clamp(audio->rx_gain * 1.4f, db_to_linear(agc->rx_gain_db_min), db_to_linear(agc->rx_gain_db_max));
        if (new_rx != audio->rx_gain) {
            agc_apply_gains(audio, audio->tx_gain, new_rx);
            agc->calib_rx_gain_step++;
            agc->last_adjust = now;
            log_info("agc calib | RX gain up %.2f -> %.2f (step %d/5)",
                     audio->rx_gain, new_rx, agc->calib_rx_gain_step);
            return;
        }
    }

    /* Se chegou no teto de ambos e ainda não tem pacotes -> reset e tenta combo diferente */
    if (audio->tx_gain >= db_to_linear(agc->tx_gain_db_max) * 0.95f && 
        audio->rx_gain >= db_to_linear(agc->rx_gain_db_max) * 0.95f) {
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
    float power = rms * rms;
    if (agc->power_avg == 0.0f) agc->power_avg = power;
    else agc->power_avg = (1.0f - agc->alpha) * agc->power_avg + agc->alpha * power;

    float power_db = 10.0f * log10f(agc->power_avg + 1e-12f);
    float error_db = agc->target_db - power_db;
    float gain_db_step = 4.0f * agc->loop_bw * error_db;

    /* --- LOCK/UNLOCK LOGIC --- */

    if (agc->locked) {
        /* CHECK UNLOCK CONDITIONS */
        int unlock = 0;
        if (d_corrupt >= (uint64_t)agc->unlock_corrupt_thresh) {
            log_warn("agc unlock | corrupção spike (%llu >= %d) | destravando ganhos",
                     (unsigned long long)d_corrupt, agc->unlock_corrupt_thresh);
            unlock = 1;
        }
        float current_rssi_db = 20.0f * log10f(rms + 1e-12f);
        float locked_rssi_db = 20.0f * log10f(agc->best_rms + 1e-12f);
        if ((locked_rssi_db - current_rssi_db) >= agc->unlock_rssi_drop_db) {
            log_warn("agc unlock | RSSI drop %.1f dB (%.1f->%.1f) | destravando",
                     locked_rssi_db - current_rssi_db, locked_rssi_db, current_rssi_db);
            unlock = 1;
        }
        if (d_sync == 0 && d_pkts == 0) {
            log_warn("agc unlock | link silencioso | destravando");
            unlock = 1;
        }
        /* NOVO: unlock por taxa de corrupção (>20% nos pacotes da janela) */
        if (d_pkts > 10) {  /* só avalia se tem amostras suficientes */
            float corrupt_rate = (float)d_corrupt / (float)d_pkts;
            if (corrupt_rate > 0.20f) {  /* >20% corrupção */
                log_warn("agc unlock | taxa corrupção %.1f%% (%llu/%llu) > 20%% | destravando",
                         corrupt_rate * 100.0f, (unsigned long long)d_corrupt, (unsigned long long)d_pkts);
                unlock = 1;
            }
        }

        if (unlock) {
            agc->locked = 0;
            agc->lock_good_streak = 0;
            agc->phase = AGC_STEADY;
            agc->settle_secs = 5;
            agc->last_unlock_time = now;  /* registra tempo do unlock para cooldown */
            log_info("agc | DESTRAVADO | voltando para STEADY");
        } else {
            /* LOCKED: mantém ganhos travados, só atualiza timestamp */
            agc->last_adjust = now;
            agc->lock_good_streak = 0;
            return;
        }
    }

    /* NOT LOCKED: track good packets for lock */
    agc->last_rx_sync = echo->stats.rx_sync_found;
    agc->last_rx_packets = echo->stats.rx_packets;
    agc->last_rx_corrupted = echo->stats.rx_corrupted;

    /* TRACK GOOD PACKETS FOR LOCK */
    int good_this_window = (d_pkts > d_corrupt) ? (d_pkts - d_corrupt) : 0;
    if (good_this_window > 0 && d_corrupt == 0) {
        agc->lock_good_streak += good_this_window;
        if (agc->lock_good_streak >= agc->lock_threshold && !agc->locked) {
            /* COOLDOWN: evita re-lock imediato após unlock */
            if (agc->last_unlock_time > 0 && (now - agc->last_unlock_time) < agc->unlock_cooldown_secs) {
                log_info("agc | cooldown ativo (%ds/%ds) | aguardando para re-lock",
                         (int)(now - agc->last_unlock_time), agc->unlock_cooldown_secs);
            } else {
                /* LOCK GAINS */
                agc->locked = 1;
                agc->locked_tx_gain_db = linear_to_db(audio->tx_gain);
                agc->locked_rx_gain_db = linear_to_db(audio->rx_gain);
                agc->best_rms = rms;
                agc->phase = AGC_LOCKED;
                agc->lock_good_streak = 0;
                log_info("agc | TRAVADO (LOCKED) | tx=%.1f dB rx=%.1f dB rms=%.3f | streak=%d/%d",
                         agc->locked_tx_gain_db, agc->locked_rx_gain_db, rms,
                         agc->lock_good_streak, agc->lock_threshold);
                agc->last_adjust = now;
                return;
            }
        }
    } else if (d_corrupt > 0) {
        /* corruption resets streak */
        agc->lock_good_streak = 0;
    }

    /* 1) Auto-echo: syncs corrompidas sem pacotes bons -> baixa TX gain */
    if (d_pkts > 0) {
        agc->echo_streak = 0;
    } else if (d_sync > 0 && d_corrupt >= d_sync) {
        agc->echo_streak++;
    }

    if (agc->echo_streak >= agc->echo_sync_thresh) {
        agc->echo_streak = 0;
        if (audio->tx_gain > db_to_linear(agc->tx_gain_db_min)) {
            float tx_db = linear_to_db(audio->tx_gain);
            float new_tx_db = tx_db - 3.0f;
            if (new_tx_db < agc->tx_gain_db_min) new_tx_db = agc->tx_gain_db_min;
            float new_tx = db_to_linear(new_tx_db);
            agc_apply_gains(audio, new_tx, audio->rx_gain);
            log_warn("agc steady | auto-eco | rms=%.3f | tx_gain %.2f->%.2f (%.1f->%.1f dB)",
                     rms, audio->tx_gain, new_tx, tx_db, new_tx_db);
            agc->last_adjust = now;
            return;
        }
        if (rms < agc->rms_min && audio->rx_gain < db_to_linear(agc->rx_gain_db_max)) {
            float rx_db = linear_to_db(audio->rx_gain);
            float new_rx_db = rx_db + 3.0f;
            if (new_rx_db > agc->rx_gain_db_max) new_rx_db = agc->rx_gain_db_max;
            float new_rx = db_to_linear(new_rx_db);
            agc_apply_gains(audio, audio->tx_gain, new_rx);
            log_info("agc steady | sinal fraco (pos eco) | rms=%.3f | rx_gain %.2f->%.2f (%.1f->%.1f dB)",
                     rms, audio->rx_gain, new_rx, rx_db, new_rx_db);
            agc->last_adjust = now;
            return;
        }
    }

    /* 2) Clip no ADC -> baixa RX gain */
    if (rms > agc->rms_max && audio->rx_gain > db_to_linear(agc->rx_gain_db_min)) {
        float rx_db = linear_to_db(audio->rx_gain);
        float new_rx_db = rx_db - 3.0f;
        if (new_rx_db < agc->rx_gain_db_min) new_rx_db = agc->rx_gain_db_min;
        float new_rx = db_to_linear(new_rx_db);
        agc_apply_gains(audio, audio->tx_gain, new_rx);
        log_warn("agc steady | clip | rms=%.3f | rx_gain %.2f->%.2f (%.1f->%.1f dB)",
                 rms, audio->rx_gain, new_rx, rx_db, new_rx_db);
        agc->last_adjust = now;
        return;
    }

    /* 3) Laço proporcional em dB para manter RMS no target */
    if (fabsf(error_db) > 1.0f) {  /* só ajusta se erro > 1 dB */
        float rx_db = linear_to_db(audio->rx_gain);
        float new_rx_db = rx_db + gain_db_step;
        new_rx_db = agc_clamp(new_rx_db, agc->rx_gain_db_min, agc->rx_gain_db_max);
        float new_rx = db_to_linear(new_rx_db);
        if (new_rx != audio->rx_gain) {
            agc_apply_gains(audio, audio->tx_gain, new_rx);
            log_info("agc steady | loop dB | rms=%.3f (%.1f dB) | target=%.1f dB | err=%.1f dB | rx_gain %.2f->%.2f (%.1f->%.1f dB)",
                     rms, power_db, agc->target_db, error_db, audio->rx_gain, new_rx, rx_db, new_rx_db);
            agc->last_adjust = now;
            return;
        }
    }

    /* 4) Silêncio: nenhum sinal do peer -> sobe RX gain */
    if (d_sync == 0 && d_pkts == 0 && rms < agc->rms_min) {
        if (audio->rx_gain < db_to_linear(agc->rx_gain_db_max)) {
            float rx_db = linear_to_db(audio->rx_gain);
            float new_rx_db = rx_db + 3.0f;
            if (new_rx_db > agc->rx_gain_db_max) new_rx_db = agc->rx_gain_db_max;
            float new_rx = db_to_linear(new_rx_db);
            agc_apply_gains(audio, audio->tx_gain, new_rx);
            log_info("agc steady | silêncio | rms=%.3f | rx_gain %.2f->%.2f (%.1f->%.1f dB)",
                     rms, audio->rx_gain, new_rx, rx_db, new_rx_db);
            agc->last_adjust = now;
            return;
        }
        if (audio->tx_gain < db_to_linear(agc->tx_gain_db_max)) {
            float tx_db = linear_to_db(audio->tx_gain);
            float new_tx_db = tx_db + 1.5f;
            if (new_tx_db > agc->tx_gain_db_max) new_tx_db = agc->tx_gain_db_max;
            float new_tx = db_to_linear(new_tx_db);
            agc_apply_gains(audio, new_tx, audio->rx_gain);
            log_info("agc steady | silêncio (rx teto) | tx_gain %.2f->%.2f (%.1f->%.1f dB)",
                     audio->tx_gain, new_tx, tx_db, new_tx_db);
            agc->last_adjust = now;
            return;
        }
    }

    /* 5) Link OK: mantém ganhos, só atualiza timestamp */
    agc->last_adjust = now;
}

static void agc_apply_gains(AudioState *audio, float tx_gain, float rx_gain) {
    audio->tx_gain = tx_gain;
    audio->rx_gain = rx_gain;
}

static float agc_clamp(float v, float min, float max) {
    if (v < min) return min;
    if (v > max) return max;
    return v;
}