#include "../include/agc.h"
#include "../include/log.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>

void agc_init(AGCState *agc) {
    memset(agc, 0, sizeof(*agc));
    agc->tx_gain_min = 0.1f;
    agc->tx_gain_max = 3.0f;
    agc->rx_gain_min = 1.0f;
    agc->rx_gain_max = 16.0f;
    agc->rms_min = 0.05f;
    agc->rms_max = 0.7f;
    agc->tx_step = 0.7f;   /* baixa TX em passos de 30% quando ha eco */
    agc->rx_step = 1.5f;   /* sobe RX em passos de 50% quando o peer some */
    agc->echo_sync_thresh = 3;
    agc->settle_secs = 5;  /* alinhado com o heartbeat das stats */
    agc->enabled = 1;
    const char *e = getenv("ECHO_AGC");
    if (e && strcmp(e, "0") == 0) agc->enabled = 0;
    if (agc->enabled) {
        LOG_INFO("agc | habilitado | tx %.1f..%.1f | rx %.1f..%.1f",
                 agc->tx_gain_min, agc->tx_gain_max,
                 agc->rx_gain_min, agc->rx_gain_max);
    } else {
        LOG_WARN("agc | desabilitado (ECHO_AGC=0)");
    }
}

/* Laço de ajuste dinamico. Regra (caderno codificacao-e-scrambling.md: AGC):
   a decisao por dominancia relativa ja tolera volume, entao o RX nao precisa
   de ganho para amplitude — precisa de protecao contra o AUTO-ECO. */
void agc_tune(AGCState *agc, EchoProtocol *echo, AudioState *audio) {
    if (!agc->enabled) return;

    time_t now = time(NULL);
    if (now - agc->last_adjust < agc->settle_secs) return;

    uint64_t d_sync = echo->stats.rx_sync_found - agc->last_rx_sync;
    uint64_t d_pkts = echo->stats.rx_packets - agc->last_rx_packets;
    uint64_t d_corrupt = echo->stats.rx_corrupted - agc->last_rx_corrupted;
    agc->last_rx_sync = echo->stats.rx_sync_found;
    agc->last_rx_packets = echo->stats.rx_packets;
    agc->last_rx_corrupted = echo->stats.rx_corrupted;
    agc->last_tx_packets = echo->stats.tx_packets;

    float rms = atomic_load(&audio->in_rms);

    /* Filtro de laço do AGC (caderno codificacao-e-scrambling.md): suaviza o
       erro antes do comparador. Leitura instantanea a cada 5s seria ruidosa
       (depende de o peer estar transmitindo naquele instante). */
    if (agc->rms_avg == 0.0f) {
        agc->rms_avg = rms;
    } else {
        agc->rms_avg = 0.7f * agc->rms_avg + 0.3f * rms;
    }
    rms = agc->rms_avg;

    /* Streak de falso lock: syncs que corrompem sem NENHUM pacote bom. O eco
       do proprio TX produz exatamente isso (sync no proprio sinal + bad_len
       absurdo); um link legitimo em calibracao tambem corrompe o 1o frame,
       por isso so age apos varias consecutivas. Persiste entre janelas sem
       sync (o eco nao dispara sync toda janela) e so zera com pacote bom. */
    if (d_pkts > 0) {
        agc->echo_streak = 0;
    } else if (d_sync > 0 && d_corrupt >= d_sync) {
        agc->echo_streak++;
    }

    if (agc->echo_streak >= agc->echo_sync_thresh) {
        agc->echo_streak = 0;
        if (rms >= agc->rms_min && audio->tx_gain > agc->tx_gain_min) {
            audio->tx_gain *= agc->tx_step;
            if (audio->tx_gain < agc->tx_gain_min) audio->tx_gain = agc->tx_gain_min;
            agc->last_adjust = now;
            LOG_WARN("agc | auto-eco | rms=%.3f | sync=%llu | corrupt=%llu | tx_gain=%.2f",
                     rms, (unsigned long long)d_sync,
                     (unsigned long long)d_corrupt, audio->tx_gain);
            return;
        }
        if (rms < agc->rms_min && audio->rx_gain < agc->rx_gain_max) {
            audio->rx_gain *= agc->rx_step;
            if (audio->rx_gain > agc->rx_gain_max) audio->rx_gain = agc->rx_gain_max;
            agc->last_adjust = now;
            LOG_INFO("agc | sinal fraco | rms=%.3f | sync=%llu | rx_gain=%.2f",
                     rms, (unsigned long long)d_sync, audio->rx_gain);
            return;
        }
    }

    /* 2) Saturação no ADC: entrada perto do clip distorce os tons (harmônicos
       confundem o Goertzel). Reduz RX gain. */
    if (rms > agc->rms_max && audio->rx_gain > agc->rx_gain_min) {
        audio->rx_gain /= agc->rx_step;
        if (audio->rx_gain < agc->rx_gain_min) audio->rx_gain = agc->rx_gain_min;
        agc->last_adjust = now;
        LOG_WARN("agc | clip | rms=%.3f | rx_gain=%.2f", rms, audio->rx_gain);
        return;
    }

    /* 3) Sem sinal do peer: nenhuma sync e RMS no chao. Sobe RX gain para
       alcancar o peer; se RX ja no teto, o problema pode ser o nosso TX
       fraco demais para o peer nos ouvir -> sobe devagar. */
    if (d_sync == 0 && d_pkts == 0 && rms < agc->rms_min) {
        if (audio->rx_gain < agc->rx_gain_max) {
            audio->rx_gain *= agc->rx_step;
            if (audio->rx_gain > agc->rx_gain_max) audio->rx_gain = agc->rx_gain_max;
            agc->last_adjust = now;
            LOG_INFO("agc | silencio | rms=%.3f | rx_gain=%.2f", rms, audio->rx_gain);
            return;
        }
        if (audio->tx_gain < agc->tx_gain_max) {
            audio->tx_gain *= 1.2f;
            if (audio->tx_gain > agc->tx_gain_max) audio->tx_gain = agc->tx_gain_max;
            agc->last_adjust = now;
            LOG_INFO("agc | silencio(rx teto) | tx_gain=%.2f", audio->tx_gain);
            return;
        }
    }

    /* 4) Link OK (pacotes bons chegando): nao mexe em nada — deixar estavel. */
    agc->last_adjust = now;
}