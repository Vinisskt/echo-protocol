#include "../include/agc.h"
#include "../include/echo_protocol.h"
#include "../include/log.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>

static void agc_calibrate(AGCState *agc, EchoProtocol *echo, AudioState *audio, time_t now);
static void agc_steady(AGCState *agc, EchoProtocol *echo, AudioState *audio, time_t now);

static void agc_binary_search_iter(AGCState *agc, EchoProtocol *echo, AudioState *audio, time_t now);
static void agc_multiplicative_loop(AGCState *agc, AudioState *audio, float power);

static int agc_check_auto_echo(AGCState *agc, AudioState *audio, uint64_t d_sync, uint64_t d_pkts,
                               uint64_t d_corrupt, float rms, time_t now);
static int agc_check_clip(AGCState *agc, AudioState *audio, float rms, time_t now);
static int agc_check_silence(AGCState *agc, AudioState *audio, uint64_t d_sync, uint64_t d_pkts,
                             float rms, time_t now);

static void agc_apply_gains(AudioState *audio, float tx_gain, float rx_gain);
static float agc_clamp(float v, float lo, float hi);
static float db_to_linear(float db);
static float linear_to_db(float linear);

void agc_init(AGCState *agc) {
    memset(agc, 0, sizeof(*agc));

    /* Limites de ganho */
    agc->tx_gain_db_min = -20.0f;
    agc->tx_gain_db_max =  10.0f;
    agc->rx_gain_db_min =   0.0f;
    agc->rx_gain_db_max =  24.0f;

    /* Faixa aceitável de RMS */
    agc->rms_min = 0.05f;
    agc->rms_max = 0.7f;

    /* Referência do laço */
    agc->target_db    = -20.0f;
    agc->target_power = powf(10.0f, agc->target_db / 10.0f);   /* 0.01 */
    agc->alpha        =  0.1f;
    agc->loop_bw      =  0.015f;

    /* Hysteresis steady-state */
    agc->hyst_margin  = 0.20f;   /* ±20 % ao redor do target */
    agc->beta_attack  = 0.15f;   /* resposta rápida a sinal alto */
    agc->beta_release = 0.03f;   /* resposta lenta a sinal baixo */

    /* Calibração */
    agc->echo_sync_thresh = 3;
    agc->settle_secs      = 1;
    agc->enabled          = 1;
    agc->phase            = AGC_CALIBRATING;

    /* Busca binária: limites iniciais = range completo (linear) */
    agc->calib_tx_low  = db_to_linear(agc->tx_gain_db_min);
    agc->calib_tx_high = db_to_linear(agc->tx_gain_db_max);
    agc->calib_tx_iter = 0;
    agc->calib_tx_good = 0;

    agc->calib_rx_low  = db_to_linear(agc->rx_gain_db_min);
    agc->calib_rx_high = db_to_linear(agc->rx_gain_db_max);
    agc->calib_rx_iter = 0;
    agc->calib_rx_good = 0;

    agc->best_rms     = 0.0f;
    agc->best_tx_gain = 1.0f;
    agc->best_rx_gain = 1.0f;
    agc->calib_done   = 0;

    agc->power_avg   = 0.0f;
    agc->gain_smooth = 1.0f;

    const char *e = getenv("ECHO_AGC");
    if (e && strcmp(e, "0") == 0) agc->enabled = 0;

    if (agc->enabled) {
        log_info("agc | init | tx %.1f..%.1f dB | rx %.1f..%.1f dB | "
                 "target %.1f dB | busca binaria + laço multiplicativo",
                 agc->tx_gain_db_min, agc->tx_gain_db_max,
                 agc->rx_gain_db_min, agc->rx_gain_db_max,
                 agc->target_db);
    } else {
        log_warn("agc | desabilitado (ECHO_AGC=0)");
    }
}

/* ------------------------------------------------------------------ */
/*  Conversores dB ↔ linear                                           */
/* ------------------------------------------------------------------ */

static float db_to_linear(float db) { return powf(10.0f, db / 20.0f); }

static float linear_to_db(float linear) {
    if (linear <= 0.0f) return -120.0f;
    return 20.0f * log10f(linear);
}

/* ------------------------------------------------------------------ */
/*  API pública                                                        */
/* ------------------------------------------------------------------ */

void agc_set_initial_gains(AudioState *audio, float tx_gain, float rx_gain) {
    audio->tx_gain = tx_gain;
    audio->rx_gain = rx_gain;
}

void agc_tune(AGCState *agc, EchoProtocol *echo, AudioState *audio) {
    if (!agc->enabled) return;
    if (audio->agc_freeze) return;

    time_t now = time(NULL);

    if (agc->calib_start_time == 0)
        agc->calib_start_time = now;
    agc->calib_secs_elapsed = (int)(now - agc->calib_start_time);

    /* Transição calibração → steady: converge ou timeout */
    if (agc->phase == AGC_CALIBRATING &&
        (agc->calib_secs_elapsed >= 20 || agc->calib_done)) {
        agc->phase = AGC_STEADY;
        agc->settle_secs = 5;
        /* Inicializa suavizador com o ganho convergido */
        agc->gain_smooth = audio->rx_gain;
        log_info("agc | calibração concluída em %ds | best rms=%.3f "
                 "tx=%.2f rx=%.2f",
                 agc->calib_secs_elapsed, agc->best_rms,
                 agc->best_tx_gain, agc->best_rx_gain);
    }

    if (agc->phase == AGC_CALIBRATING)
        agc_calibrate(agc, echo, audio, now);
    else
        agc_steady(agc, echo, audio, now);
}

/* ------------------------------------------------------------------ */
/*  CALIBRAÇÃO — busca binária adaptativa                              */
/* ------------------------------------------------------------------ */

/*
 * A cada iteração testa o ponto médio do intervalo atual.
 *   • Se收到了 pacotes válidos e RMS OK → ganho pode subir  → move low pra cima
 *   • Senão                            → ganho alto demais → move high pra baixo
 * Converge em ~log2(range/precisão) passos por eixo.
 */
static void agc_calibrate(AGCState *agc, EchoProtocol *echo, AudioState *audio, time_t now) {
    int interval = (agc->calib_secs_elapsed < 5) ? 1 : 2;
    if (now - agc->last_adjust < interval) return;

    agc_binary_search_iter(agc, echo, audio, now);
    agc->last_adjust = now;
}

static void agc_binary_search_iter(AGCState *agc, EchoProtocol *echo,
                                   AudioState *audio, time_t now) {
    float rms = atomic_load(&audio->in_rms);

    /* EMA da potência (detector) */
    float power = rms * rms;
    if (agc->power_avg == 0.0f) agc->power_avg = power;
    else agc->power_avg = (1.0f - agc->alpha) * agc->power_avg + agc->alpha * power;

    uint64_t d_sync    = echo->stats.rx_sync_found  - agc->last_rx_sync;
    uint64_t d_pkts    = echo->stats.rx_packets      - agc->last_rx_packets;
    uint64_t d_corrupt = echo->stats.rx_corrupted    - agc->last_rx_corrupted;

    agc->last_rx_sync     = echo->stats.rx_sync_found;
    agc->last_rx_packets  = echo->stats.rx_packets;
    agc->last_rx_corrupted = echo->stats.rx_corrupted;

    log_info("agc calib | t=%ds | rms=%.3f | sync=%llu pkts=%llu "
             "corrupt=%llu | tx=%.2f rx=%.2f | iter tx=%d rx=%d",
             agc->calib_secs_elapsed, rms,
             (unsigned long long)d_sync, (unsigned long long)d_pkts,
             (unsigned long long)d_corrupt,
             audio->tx_gain, audio->rx_gain,
             agc->calib_tx_iter, agc->calib_rx_iter);

    /* ── Guardas imediatos — prioridade sobre busca ── */
    if (agc_check_clip(agc, audio, rms, now))      return;
    if (agc_check_auto_echo(agc, audio, d_sync, d_pkts, d_corrupt, rms, now)) return;

    /* ── Convergiu: pacotes bons + RMS na faixa ── */
    if (d_pkts > 0 && rms >= agc->rms_min && rms <= agc->rms_max) {
        agc->best_rms     = rms;
        agc->best_tx_gain = audio->tx_gain;
        agc->best_rx_gain = audio->rx_gain;
        agc->calib_done   = 1;
        return;
    }

    /* ── Decisão binária: TX primeiro, depois RX ── */

    /* Eixo TX */
    if (agc->calib_tx_iter < 20) {
        float mid = (agc->calib_tx_low + agc->calib_tx_high) / 2.0f;
        mid = agc_clamp(mid, db_to_linear(agc->tx_gain_db_min),
                        db_to_linear(agc->tx_gain_db_max));

        int good = (d_pkts > 0 && rms >= agc->rms_min && rms <= agc->rms_max);

        if (good) {
            agc->calib_tx_low = mid;    /* pode subir */
        } else {
            agc->calib_tx_high = mid;   /* desce */
        }

        agc->calib_tx_iter++;
        agc_apply_gains(audio, mid, audio->rx_gain);

        log_info("agc calib | TX binaria | mid=%.2f | %s | range [%.2f, %.2f]",
                 mid, good ? "LOW↑" : "HIGH↓",
                 agc->calib_tx_low, agc->calib_tx_high);
        return;
    }

    /* Eixo RX */
    if (agc->calib_rx_iter < 20) {
        float mid = (agc->calib_rx_low + agc->calib_rx_high) / 2.0f;
        mid = agc_clamp(mid, db_to_linear(agc->rx_gain_db_min),
                        db_to_linear(agc->rx_gain_db_max));

        int good = (d_pkts > 0 && rms >= agc->rms_min && rms <= agc->rms_max);

        if (good) {
            agc->calib_rx_low = mid;
        } else {
            agc->calib_rx_high = mid;
        }

        agc->calib_rx_iter++;
        agc_apply_gains(audio, audio->tx_gain, mid);

        log_info("agc calib | RX binaria | mid=%.2f | %s | range [%.2f, %.2f]",
                 mid, good ? "LOW↑" : "HIGH↓",
                 agc->calib_rx_low, agc->calib_rx_high);
        return;
    }

    /* Ambos os eixos esgotaram → usa o melhor encontrado */
    if (agc->best_tx_gain > 0.0f || agc->best_rx_gain > 0.0f) {
        agc_apply_gains(audio, agc->best_tx_gain, agc->best_rx_gain);
        agc->calib_done = 1;
        log_info("agc calib | busca binaria concluida | tx=%.2f rx=%.2f",
                 agc->best_tx_gain, agc->best_rx_gain);
        return;
    }

    /* Fallback: sem resultado, aplica ganhos conservadores */
    agc_apply_gains(audio,
                    db_to_linear((agc->tx_gain_db_min + agc->tx_gain_db_max) / 2.0f),
                    db_to_linear((agc->rx_gain_db_min + agc->rx_gain_db_max) / 2.0f));
    agc->calib_done = 1;
}

/* ------------------------------------------------------------------ */
/*  STEADY — laço multiplicativo com hysteresis                       */
/* ------------------------------------------------------------------ */

/*
 * Detector:  power_avg  (EMA de RMS²)
 * Ganho ideal:  target_power / power_avg
 * Suavização:   gain *= (1 - beta) + ideal * beta
 * Hysteresis:   só ajusta se power_avg fora de [target/(1+m), target*(1+m)]
 *
 * Attack (sinal alto)  → beta_attack  (resposta rápida)
 * Release (sinal baixo)→ beta_release (resposta lenta)
 */
static void agc_steady(AGCState *agc, EchoProtocol *echo, AudioState *audio, time_t now) {
    if (now - agc->last_adjust < agc->settle_secs) return;

    uint64_t d_sync, d_pkts, d_corrupt;
    float rms;

    /* Leitura de deltas e potência */
    d_sync    = echo->stats.rx_sync_found  - agc->last_rx_sync;
    d_pkts    = echo->stats.rx_packets      - agc->last_rx_packets;
    d_corrupt = echo->stats.rx_corrupted    - agc->last_rx_corrupted;

    agc->last_rx_sync     = echo->stats.rx_sync_found;
    agc->last_rx_packets  = echo->stats.rx_packets;
    agc->last_rx_corrupted = echo->stats.rx_corrupted;

    rms = atomic_load(&audio->in_rms);
    float power = rms * rms;
    if (agc->power_avg == 0.0f) agc->power_avg = power;
    else agc->power_avg = (1.0f - agc->alpha) * agc->power_avg + agc->alpha * power;

    /* Guardas prioritários */
    if (agc_check_auto_echo(agc, audio, d_sync, d_pkts, d_corrupt, rms, now))
        goto done;
    if (agc_check_clip(agc, audio, rms, now))
        goto done;

    /* Laço multiplicativo com hysteresis */
    agc_multiplicative_loop(agc, audio, agc->power_avg);

    /* Silêncio: nenhum sinal do peer */
    if (d_sync == 0 && d_pkts == 0 && rms < agc->rms_min)
        agc_check_silence(agc, audio, d_sync, d_pkts, rms, now);

done:
    agc->last_adjust = now;
}

/*
 * Laço multiplicativo: ajusta RX gain multiplicativamente.
 * A correção é uma RAZÃO (target/power), não uma diferença em dB,
 * o que torna o settling time mais uniforme entre sinais fortes e fracos.
 * A hysteresis (faixa morta) evita oscilações perto do alvo.
 */
static void agc_multiplicative_loop(AGCState *agc, AudioState *audio,
                                    float power) {
    if (power <= 1e-12f) return;

    float ratio = agc->target_power / power;

    /* Faixa morta: só ajusta se fora da hysteresis */
    float lo = agc->target_power / (1.0f + agc->hyst_margin);
    float hi = agc->target_power * (1.0f + agc->hyst_margin);

    if (power >= lo && power <= hi)
        return;   /* dentro da faixa morta — nada a fazer */

    /* Beta diferenciado: attack (sinal alto) mais rápido que release */
    float beta = (power > hi) ? agc->beta_attack : agc->beta_release;

    /* Ganho suavizado: interpolação multiplicativa */
    agc->gain_smooth = agc->gain_smooth * (1.0f - beta) + ratio * agc->gain_smooth * beta;

    /* Aplica com clamp */
    agc->gain_smooth = agc_clamp(agc->gain_smooth,
                                 db_to_linear(agc->rx_gain_db_min),
                                 db_to_linear(agc->rx_gain_db_max));

    float power_db = 10.0f * log10f(power + 1e-12f);

    if (fabsf(agc->gain_smooth - audio->rx_gain) > 0.01f) {
        log_info("agc steady | mult loop | rms²=%.6f (%.1f dB) | target=%.1f dB "
                 "ratio=%.3f | rx %.2f→%.2f (%.1f→%.1f dB)",
                 power, power_db, agc->target_db, ratio,
                 audio->rx_gain, agc->gain_smooth,
                 linear_to_db(audio->rx_gain), linear_to_db(agc->gain_smooth));
        agc_apply_gains(audio, audio->tx_gain, agc->gain_smooth);
    }
}

/* ------------------------------------------------------------------ */
/*  Guardas compartilhados entre calibração e steady                   */
/* ------------------------------------------------------------------ */

static int agc_check_auto_echo(AGCState *agc, AudioState *audio,
                               uint64_t d_sync, uint64_t d_pkts,
                               uint64_t d_corrupt, float rms, time_t now) {
    if (d_pkts > 0) {
        agc->echo_streak = 0;
    } else if (d_sync > 0 && d_corrupt >= d_sync) {
        agc->echo_streak++;
    }

    if (agc->echo_streak >= agc->echo_sync_thresh) {
        agc->echo_streak = 0;
        if (audio->tx_gain > db_to_linear(agc->tx_gain_db_min)) {
            float tx_db     = linear_to_db(audio->tx_gain);
            float new_tx_db = tx_db - 3.0f;
            if (new_tx_db < agc->tx_gain_db_min) new_tx_db = agc->tx_gain_db_min;
            float new_tx = db_to_linear(new_tx_db);
            agc_apply_gains(audio, new_tx, audio->rx_gain);
            log_warn("agc | auto-eco | rms=%.3f | tx_gain %.2f→%.2f (%.1f→%.1f dB)",
                     rms, audio->tx_gain, new_tx, tx_db, new_tx_db);
            agc->last_adjust = now;
            return 1;
        }
    }
    return 0;
}

static int agc_check_clip(AGCState *agc, AudioState *audio, float rms, time_t now) {
    if (rms > agc->rms_max && audio->rx_gain > db_to_linear(agc->rx_gain_db_min)) {
        float rx_db     = linear_to_db(audio->rx_gain);
        float new_rx_db = rx_db - 3.0f;
        if (new_rx_db < agc->rx_gain_db_min) new_rx_db = agc->rx_gain_db_min;
        float new_rx = db_to_linear(new_rx_db);
        agc_apply_gains(audio, audio->tx_gain, new_rx);
        log_warn("agc | clip | rms=%.3f | rx_gain %.2f→%.2f (%.1f→%.1f dB)",
                 rms, audio->rx_gain, new_rx, rx_db, new_rx_db);
        agc->last_adjust = now;
        return 1;
    }
    return 0;
}

static int agc_check_silence(AGCState *agc, AudioState *audio,
                             uint64_t d_sync, uint64_t d_pkts, float rms, time_t now) {
    if (d_sync == 0 && d_pkts == 0 && rms < agc->rms_min) {
        if (audio->rx_gain < db_to_linear(agc->rx_gain_db_max)) {
            float rx_db     = linear_to_db(audio->rx_gain);
            float new_rx_db = rx_db + 3.0f;
            if (new_rx_db > agc->rx_gain_db_max) new_rx_db = agc->rx_gain_db_max;
            float new_rx = db_to_linear(new_rx_db);
            agc_apply_gains(audio, audio->tx_gain, new_rx);
            log_info("agc | silencio | rms=%.3f | rx_gain %.2f→%.2f (%.1f→%.1f dB)",
                     rms, audio->rx_gain, new_rx, rx_db, new_rx_db);
            agc->last_adjust = now;
            return 1;
        }
        if (audio->tx_gain < db_to_linear(agc->tx_gain_db_max)) {
            float tx_db     = linear_to_db(audio->tx_gain);
            float new_tx_db = tx_db + 1.5f;
            if (new_tx_db > agc->tx_gain_db_max) new_tx_db = agc->tx_gain_db_max;
            float new_tx = db_to_linear(new_tx_db);
            agc_apply_gains(audio, new_tx, audio->rx_gain);
            log_info("agc | silencio (rx teto) | tx_gain %.2f→%.2f (%.1f→%.1f dB)",
                     audio->tx_gain, new_tx, tx_db, new_tx_db);
            agc->last_adjust = now;
            return 1;
        }
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/*  Utilitários                                                        */
/* ------------------------------------------------------------------ */

static void agc_apply_gains(AudioState *audio, float tx_gain, float rx_gain) {
    audio->tx_gain = tx_gain;
    audio->rx_gain = rx_gain;
}

static float agc_clamp(float v, float lo, float hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}
