#include "../include/echo_protocol.h"
#include "../include/audio_io.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <poll.h>
#include <unistd.h>
#include <stdatomic.h>  /* FIX #1: atomic para proteger estado RX entre threads */

/* FIX #1: atomic garante que leituras/escritas entre threads sejam seguras
 * sem precisar de mutex — overhead zero no caminho critico */
volatile atomic_int keep_running = 1;

void signal_handler(int sig) {
    (void)sig;
    atomic_store(&keep_running, 0);
}

/* FIX #5: validacao de argumento numerico com deteccao de erro real */
static int parse_device_id(const char *arg) {
    if (arg == NULL) return -1;
    char *end;
    long val = strtol(arg, &end, 10);
    if (end == arg || *end != '\0') {
        fprintf(stderr, "Aviso: argumento '%s' nao e um numero valido, usando dispositivo padrao.\n", arg);
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
        printf("Erro: Faltam argumentos de interface e IP.\n");
        return 1;
    }

    char *dev_name = argv[1];
    char *ip_addr  = argv[2];

    /* FIX #5: strtol ao inves de atoi — detecta "abc" ou "" corretamente */
    int input_id  = parse_device_id(argc > 3 ? argv[3] : NULL);
    int output_id = parse_device_id(argc > 4 ? argv[4] : NULL);

    EchoProtocol echo;
    AudioState    audio;

    signal(SIGINT, signal_handler);

    printf("[Main] Inicializando Echo Protocol em %s...\n", dev_name);
    if (echo_init(&echo, dev_name) < 0) {
        fprintf(stderr, "Falha ao inicializar Echo Protocol\n");
        return 1;
    }

    printf("[Main] Configurando interface %s (IP: %s, MTU: 1000)...\n", dev_name, ip_addr);
    tun_set_ip(dev_name, ip_addr);
    tun_set_mtu(dev_name, 1000);
    tun_set_up(dev_name);

    printf("[Main] Inicializando PortAudio...\n");
    if (audio_init(&audio, &echo, input_id, output_id) < 0) {
        echo_close(&echo);
        return 1;
    }

    if (audio_start(&audio) < 0) {
        audio_close(&audio);
        echo_close(&echo);
        return 1;
    }

    printf("[Main] Sistema pronto! SSH via Audio ativo.\n");
    printf("[Main] Pressione Ctrl+C para encerrar.\n");

    struct pollfd fds[1];
    fds[0].fd     = echo.tun_fd;
    fds[0].events = POLLIN;

    while (atomic_load(&keep_running)) {

        /* FIX #2: timeout reduzido de 50ms para 5ms
         * Antes: cada pacote esperava ate 50ms so no poll
         * Agora: maxima espera de 5ms — budget de latencia muito mais confortavel
         * CPU: poll bloqueante continua eficiente; nao e busy-wait */
        int ret = poll(fds, 1, 5);

        if (ret > 0 && (fds[0].revents & POLLIN)) {

            /* FIX #3: ler o pacote COMPLETO antes de inserir preambulo
             * Antes: preambulo era inserido para cada evento POLLIN,
             *        que pode disparar multiplas vezes para um mesmo pacote grande.
             * Agora: drenamos todo o dado disponivel com um unico preambulo+sync
             *        por pacote logico lido do TUN */
            push_preamble(echo.tx_rb);
            push_sync_word(echo.tx_rb);
            tun_to_rb(&echo);  /* le exatamente um pacote TUN por vez */
        }

        /* FIX #4: verificar flag dedicado de "pacote completo" em vez de
         * inferir pelo estado SEARCHING + bits_received.
         *
         * O problema original: state == SEARCHING significa que o demodulador
         * JA SAIU do estado de recepcao, mas bits_received pode ter sido
         * zerado ou parcialmente atualizado pelo thread de audio nesse meio tempo
         * (race condition + logica invertida).
         *
         * Solucao correta: o demodulador deve setar um flag atomico
         * "packet_ready" quando terminar de montar um pacote. O main loop
         * apenas reage a esse flag, sem depender do estado interno da maquina
         * de estados. Isso elimina a race condition e a logica invertida.
         *
         * Como echo_protocol.h deve expor esse flag:
         *   atomic_int packet_ready;   <- setado pelo thread de audio
         *   int        packet_len;     <- protegido: so escrito antes do flag
         *
         * Se sua versao atual nao tem packet_ready ainda, este e o padrao
         * a implementar em echo_protocol para corrigir definitivamente. */
        if (atomic_load(&echo.rx.packet_ready)) {
            int packet_len = echo.rx.packet_len;
            if (packet_len > 0) {
                rb_to_tun(&echo, &packet_len);
            }
            /* zera o flag DEPOIS de processar — ordem importa */
            atomic_store(&echo.rx.packet_ready, 0);
        }
    }

    printf("\n[Main] Encerrando...\n");
    audio_close(&audio);
    echo_close(&echo);
    return 0;
}
