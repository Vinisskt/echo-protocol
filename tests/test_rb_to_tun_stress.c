#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>
#include "../include/echo_protocol.h"

/**
 * Teste de Estresse: rb_to_tun
 * 
 * Valida a integridade de dados enviando 1000 pacotes de tamanhos variados
 * com dados aleatórios, forçando múltiplas rotações no buffer circular.
 */

int main() {
    printf("--- Teste de Estresse: rb_to_tun (1000 pacotes) ---\n");
    srand(time(NULL));

    EchoProtocol echo;
    int pipefd[2];

    if (pipe(pipefd) == -1) {
        perror("Erro ao criar pipe");
        return 1;
    }

    // Inicialização manual para o teste
    memset(&echo, 0, sizeof(EchoProtocol));
    echo.tun_fd = pipefd[1];
    echo.rx_rb = rb_init();
    
    // Configurar leitura não bloqueante
    fcntl(pipefd[0], F_SETFL, O_NONBLOCK);

    int num_packets = 1000;
    uint8_t packet[SIZE_BUF];
    uint8_t result[SIZE_BUF];

    for (int p = 0; p < num_packets; p++) {
        // Tamanho aleatório entre 20 e 500 bytes
        int packet_len = 20 + (rand() % 480);
        
        // Gerar dados aleatórios
        for (int i = 0; i < packet_len; i++) {
            packet[i] = rand() & 0xFF;
        }

        // Inserir bits no buffer
        for (int i = 0; i < packet_len; i++) {
            for (int b = 7; b >= 0; b--) {
                uint8_t bit = (packet[i] >> b) & 1;
                if (!put_bits(echo.rx_rb, &bit)) {
                    printf("[ERRO] Buffer cheio no pacote %d\n", p);
                    return 1;
                }
            }
        }

        // Processar pacote
        rb_to_tun(&echo, &packet_len);

        // Validar leitura
        int n = read(pipefd[0], result, sizeof(result));
        if (n != packet_len) {
            printf("[FALHA] Tamanho incorreto no pacote %d. Esperado: %d, Recebido: %d\n", p, packet_len, n);
            return 1;
        }

        if (memcmp(result, packet, packet_len) != 0) {
            printf("[FALHA] Corrupção de dados no pacote %d!\n", p);
            return 1;
        }

        if ((p + 1) % 100 == 0) {
            printf("Processados %d pacotes...\n", p + 1);
        }
    }

    printf("\n✓ SUCESSO ABSOLUTO: 1000 pacotes processados sem corrupção ou falha de tamanho.\n");

    close(pipefd[0]);
    close(pipefd[1]);
    free(echo.rx_rb);
    return 0;
}
