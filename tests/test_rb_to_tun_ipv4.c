#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <arpa/inet.h>
#include "../include/echo_protocol.h"

/**
 * Teste de Validação: rb_to_tun com Cabeçalho IPv4 Real
 * 
 * Este teste envia um fluxo de bits que representa um pacote IPv4 ICMP (Ping)
 * para a função rb_to_tun. Ele valida:
 * 1. Reconstrução de bits para bytes.
 * 2. Identificação do tamanho do pacote (via cabeçalho IP).
 * 3. Escrita correta no descritor do TUN.
 */

int main() {
    printf("--- Teste de Validação: rb_to_tun (IPv4) ---\n");

    EchoProtocol echo;
    int pipefd[2];

    if (pipe(pipefd) == -1) {
        perror("Erro ao criar pipe");
        return 1;
    }

    // Inicialização manual para o teste
    memset(&echo, 0, sizeof(EchoProtocol));
    echo.tun_fd = pipefd[1];
    echo.rx_rb = rb_init(); // Inicializar o buffer!
    
    // Configurar leitura não bloqueante
    fcntl(pipefd[0], F_SETFL, O_NONBLOCK);

    // 1. Criar e enviar múltiplos pacotes para testar rotação e fluxo
    uint8_t base_packet[] = {
        0x45, 0x00, 0x00, 0x1C, // Total Length (28 bytes)
        0x12, 0x34, 0x40, 0x00, 
        0x40, 0x01, 0xAF, 0xB7, 
        0x0A, 0x00, 0x00, 0x01, 
        0x0A, 0x00, 0x00, 0x02, 
        0x08, 0x00, 0xF7, 0xFF, 
        0x00, 0x01, 0x00, 0x01  
    };
    int packet_len = sizeof(base_packet);
    int num_iterations = 5;

    for (int iter = 0; iter < num_iterations; iter++) {
        printf("\n--- Iteração %d ---\n", iter + 1);
        
        // Modificar um byte do payload para garantir que os dados mudam
        base_packet[packet_len - 1] = (uint8_t)iter;
        
        printf("Inserindo pacote IPv4 de %d bytes no RX_RB...\n", packet_len);
        for (int i = 0; i < packet_len; i++) {
            for (int b = 7; b >= 0; b--) {
                uint8_t bit = (base_packet[i] >> b) & 1;
                put_bits(echo.rx_rb, &bit);
            }
        }

        printf("Chamando rb_to_tun...\n");
        rb_to_tun(&echo, &packet_len);

        // 2. Verificar o resultado no pipe
        uint8_t result[2048];
        int n = read(pipefd[0], result, sizeof(result));

        if (n > 0) {
            printf("Sucesso! Recebidos %d bytes do TUN.\n", n);
            if (n == packet_len && memcmp(result, base_packet, n) == 0) {
                printf("✓ SUCESSO: Pacote %d reconstruído perfeitamente!\n", iter + 1);
            } else {
                printf("[FALHA] Pacote %d: Conteúdo ou tamanho incorreto.\n", iter + 1);
                return 1;
            }
        } else {
            printf("[FALHA] O TUN não recebeu nenhum dado na iteração %d.\n", iter + 1);
            return 1;
        }
    }

    printf("\n✓ TODOS OS TESTES PASSARAM: Fluxo de múltiplos pacotes validado.\n");

    close(pipefd[0]);
    close(pipefd[1]);
    return 0;
}
