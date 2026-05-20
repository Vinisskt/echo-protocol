#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <net/if.h>
#include "../include/echo_protocol.h"

int main() {
    EchoProtocol echo;
    char dev[IFNAMSIZ] = "echo0";

    printf("--- Teste de Validação: tun_to_rb ---\n");

    if (echo_init(&echo, dev) < 0) {
        fprintf(stderr, "Erro ao inicializar protocolo. Use sudo.\n");
        return 1;
    }

    printf("Interface %s pronta. Aguardando 1 pacote...\n", dev);
    printf("Dica: Em outro terminal: sudo ip link set dev %s up && ping -c 1 10.0.0.2\n", dev);

    // Chama a função que você está implementando
    tun_to_rb(&echo);

    printf("\nBits recebidos no Ring Buffer (TX):\n");
    
    uint8_t bit;
    uint8_t current_byte = 0;
    int bit_count = 0;
    int bytes_reconstructed = 0;

    // Extrai os bits do buffer para validar a reconstrução
    while (get_bits(echo.tx_rb, &bit)) {
        current_byte = (current_byte << 1) | bit;
        bit_count++;

        if (bit_count == 8) {
            printf("%02X ", current_byte);
            bytes_reconstructed++;
            if (bytes_reconstructed % 16 == 0) printf("\n");
            
            current_byte = 0;
            bit_count = 0;
        }
    }

    printf("\n\nTotal de bytes reconstruídos: %d\n", bytes_reconstructed);
    if (bytes_reconstructed > 0) {
        printf("Sucesso: Bits convertidos de volta para bytes com sucesso!\n");
    }

    echo_close(&echo);
    return 0;
}
