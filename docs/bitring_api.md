# API do Bit Ring Buffer

Documentação técnica do mecanismo de filas circulares por bit, o coração do desacoplamento de dados no Echo Protocol.

## Estrutura de Dados

### `Buffer`
```c
#define BUFFER_SIZE 8192 // 8KB de capacidade por canal (TX/RX)

typedef struct {
    uint8_t buf[BUFFER_SIZE]; 
    uint16_t head;            // Escrita (Byte)
    uint16_t tail;            // Leitura (Byte)
    uint8_t count_put;        // Offset de bit (Escrita)
    uint8_t count_get;        // Offset de bit (Leitura)
} Buffer;
```

## Funções e Lógica

### `put_bits(Buffer *buf, uint8_t *bit)`
- **Lógica LSB (Least Significant Bit):** Os bits são inseridos da direita para a esquerda dentro de cada byte. 
- **Decoupling:** Esta função permite que o loop principal insira milhares de bits (como um pacote IP comprimido) instantaneamente, sem precisar esperar o tempo do áudio.

### `get_bits(Buffer *buf, uint8_t *bit)`
- **Uso no Callback:** É chamada pela thread de áudio a cada 1/1800 de segundo. 
- **Idle State:** Se retornar `0` (vazio), o modulador deve assumir um estado de repouso (frequência Mark) para manter a portadora sincronizada.

## Por que 8KB?

Aumentamos o `BUFFER_SIZE` de 1KB para **8KB** por três motivos principais:
1.  **MTU 1000:** Um pacote IP cheio gera 8000 bits. Precisamos de espaço para pelo menos um pacote inteiro + o preâmbulo.
2.  **Retransmissão TCP:** O SSH pode disparar vários pacotes de uma vez. O buffer de 8KB age como uma "represa", absorvendo a pressão da rede e liberando os bipes de som em um fluxo constante.
3.  **LZ4 Efficiency:** Permite lidar com blocos de descompressão maiores sem risco de overflow.

## Estabilidade de Memória
- **Zero Alocação Dinâmica no Loop:** Os buffers são alocados no `echo_init` e reutilizados durante toda a sessão, garantindo que o programa nunca trave por falta de memória (OOM) ou latência de Garbage Collector.
