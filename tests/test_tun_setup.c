#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <net/if.h>
#include <linux/if_tun.h>
#include "../include/tun_tap.h"

int main() {
    char dev[IFNAMSIZ] = "echo0";
    int fd;
    uint8_t buffer[2048];
    int nread;

    printf("--- Teste de Configuração TUN/TAP ---\n");
    printf("Tentando alocar interface: %s\n", dev);

    // IFF_TUN: Camada 3 (IP)
    // IFF_NO_PI: Não adiciona informações extras de cabeçalho
    fd = tun_alloc(dev, IFF_TUN | IFF_NO_PI);

    if (fd < 0) {
        fprintf(stderr, "ERRO: Não foi possível criar a interface. Você rodou como sudo?\n");
        return 1;
    }

    printf("Sucesso! Interface %s criada (fd: %d).\n", dev, fd);
    printf("Dica: Em outro terminal, rode:\n");
    printf("  sudo ip link set dev %s up\n", dev);
    printf("  sudo ip addr add 10.0.0.1/24 dev %s\n", dev);
    printf("  ping 10.0.0.2\n");
    printf("Aguardando pacotes (Ctrl+C para sair)...\n");

    while (1) {
        nread = tun_read(fd, buffer, sizeof(buffer));
        if (nread < 0) {
            perror("Erro ao ler do TUN");
            break;
        }
        printf("Recebeu pacote de %d bytes!\n", nread);
        
        // Exibe os primeiros bytes para ver se parece um pacote IP (primeiro nibble deve ser 4 para IPv4)
        if (nread > 0) {
            printf("Primeiro byte: 0x%02X (Versão IP: %d)\n", buffer[0], buffer[0] >> 4);
        }
    }

    close(fd);
    return 0;
}
