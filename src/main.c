#include "../include/echo_protocol.h"
#include "../include/audio_io.h"
#include "../include/log.h"
#include "../include/agc.h"
#include "../include/hw_calibrate.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <poll.h>
#include <unistd.h>
#include <stdatomic.h>
#include <time.h>
#include <math.h>

volatile atomic_int keep_running = 1;

static void signal_handler(int sig) {
    (void)sig;
    atomic_store(&keep_running, 0);
}

static int parse_device_id(const char *arg) {
    if (arg == NULL) return -1;
    char *end;
    long val = strtol(arg, &end, 10);
    if (end == arg || *end != '\0') {
        log_warn("argumento '%s' invalido, usando dispositivo padrao", arg);
        return -1;
    }
    return (int)val;
}

int main(int argc, char *argv[]) {
    if (argc < 2) {
        printf("Uso: %s <interface> <ip_local> [input_id] [output_id]\n", argv[0]);
        printf("Ou: %s -l (para listar dispositivos de audio)\n", argv[0]);
        return 1;
    }

    if (strcmp(argv[1], "-l") == 0) {
        audio_list_devices();
        return 0;
    }

    if (argc < 3) {
        log_error("faltam argumentos de interface e IP");
        return 1;
    }

    char *dev_name = argv[1];
    char *ip_addr  = argv[2];

    int input_id  = parse_device_id(argc > 3 ? argv[3] : NULL);
    int output_id = parse_device_id(argc > 4 ? argv[4] : NULL);

    log_init();
    log_set_console_level(LOG_INFO);

    struct sigaction sa = {0};
    sa.sa_handler = signal_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    EchoProtocol echo;
    AudioState    audio;

    log_info("inicializando Echo Protocol em %s", dev_name);
    if (echo_init(&echo, dev_name) < 0) {
        log_error("echo_init falhou");
        return 1;
    }

    log_info("configurando interface %s (IP: %s, MTU: 512)", dev_name, ip_addr);
    tun_set_ip(dev_name, ip_addr);
    tun_set_mtu(dev_name, 512);
    tun_set_up(dev_name);

    log_info("inicializando PortAudio");
    if (audio_init(&audio, &echo, input_id, output_id) < 0) {
        log_error("audio_init falhou");
        echo_close(&echo);
        return 1;
    }

    if (audio_start(&audio) < 0) {
        log_error("audio_start falhou");
        audio_close(&audio);
        echo_close(&echo);
        return 1;
    }

    /* Auto-calibração: mede noise floor, testa frequências, ajusta AGC */
    HWCalibration hw;
    if (hw_calibrate(&audio, &echo, echo.agc, &hw) < 0) {
        log_warn("hw_calibrate falhou, usando parâmetros padrão");
    } else {
        log_info("hardware calibrado com sucesso");
    }

    /* Ganho TX ALTO para IR packets (warm-up) — garante que cheguem no peer.
     * Depois o AGC assume com ganho BAIXO (conservador) e sobe por medição. */
    extern void agc_set_initial_gains(AudioState *audio, float tx_gain, float rx_gain);
    agc_set_initial_gains(&audio, 2.5f, 1.0f);

    log_info("warm-up de audio (5s) + estabelecendo contexto ROHC...");
    sleep(5);

    // Enviar 8 pacotes raw (IR) com ganho alto
    for (int i = 0; i < 8; i++) {
        push_preamble(echo.tx_rb);
        push_sync_word(echo.tx_rb);
        
        uint8_t dummy_ip[28] = {
            0x45, 0x00, 0x00, 0x1C,
            0x00, 0x01, 0x40, 0x00,
            0x40, 0x01, 0x00, 0x00,
            0x0A, 0x00, 0x00, 0x01,
            0x0A, 0x00, 0x00, 0x02
        };
        dummy_ip[10] = 0; dummy_ip[11] = 0;
        uint16_t ck = ip_checksum(dummy_ip, 20);
        dummy_ip[10] = (ck >> 8) & 0xFF;
        dummy_ip[11] = ck & 0xFF;
        
        // Header: bit15=comp(0), bit14=rohc(0), bits13-0=len
        uint16_t header = (28 & 0x3FFF);
        scrambler_reset(&echo.tx_scrambler);
        for (int b = 15; b >= 0; b--) {
            uint8_t bit = (header >> b) & 1;
            bit = scrambler_process(&echo.tx_scrambler, bit);
            put_bits(echo.tx_rb, &bit);
        }
        for (int b = 0; b < 28 * 8; b++) {
            uint8_t bit = (dummy_ip[b >> 3] >> (7 - (b & 7))) & 1;
            bit = scrambler_process(&echo.tx_scrambler, bit);
            put_bits(echo.tx_rb, &bit);
        }
        usleep(50000);  // 50ms entre pacotes IR
    }

    /* IR packets enviados — agora BAIXA ganho para AGC conservador começar
     * O calibrador vai subir tx_gain/rx_gain baseado no RMS REAL medido */
    agc_set_initial_gains(&audio, 0.3f, 1.0f);
    log_info("ganho reduzido para calibracao conservadora (tx=0.3, rx=1.0)");

    log_info("sistema pronto - SSH via audio ativo");
    log_info("pressione Ctrl+C para encerrar");

    struct pollfd fds[1];
    fds[0].fd     = echo.tun_fd;
    fds[0].events = POLLIN;

    time_t last_stats = time(NULL);

    while (atomic_load(&keep_running)) {
        int ret = poll(fds, 1, 5);

        agc_tune(echo.agc, &echo, &audio);

        if (ret > 0 && (fds[0].revents & POLLIN)) {
            uint16_t used = (echo.tx_rb->head - echo.tx_rb->tail) & BUFFER_MASK;
            uint16_t free_space = BUFFER_SIZE - used;
            
            if (free_space < MAX_TX_FRAME_BYTES) {
                static time_t last_warn = 0;
                time_t now = time(NULL);
                if (now != last_warn) {
                    last_warn = now;
                    log_warn("TX backlog: tx_rb %d/%d free=%d (segurando TUN)", used, BUFFER_SIZE, free_space);
                }
            } else {
                push_preamble(echo.tx_rb);
                push_sync_word(echo.tx_rb);
                tun_to_rb(&echo);
            }
        }

        if (atomic_load(&echo.rx.packet_ready)) {
            int packet_len = echo.rx.packet_len;
            if (packet_len > 0) {
                rb_to_tun(&echo, &packet_len);
            }
            atomic_store(&echo.rx.packet_ready, 0);
        }

        time_t now = time(NULL);
        if (now - last_stats >= 10) {
            last_stats = now;
            uint16_t tx_used = (echo.tx_rb->head - echo.tx_rb->tail) & BUFFER_MASK;
            uint16_t rx_used = (echo.rx_rb->head - echo.rx_rb->tail) & BUFFER_MASK;
            float in_rms = atomic_load(&audio.in_rms);
            log_info("status: TX buf=%d/%d RX buf=%d/%d pkts TX=%llu RX=%llu corrupt=%llu in_rms=%.4f (%.1f dB)",
                     tx_used, BUFFER_SIZE, rx_used, BUFFER_SIZE,
                     (unsigned long long)echo.stats.tx_packets,
                     (unsigned long long)echo.stats.rx_packets,
                     (unsigned long long)echo.stats.rx_corrupted,
                     in_rms, 20.0f * log10f(in_rms + 1e-9f));
        }
    }

    log_info("encerrando...");
    audio_close(&audio);
    echo_close(&echo);
    log_close();
    return 0;
}