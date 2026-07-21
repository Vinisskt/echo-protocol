# API de Demodulação 4-FSK (Goertzel)

Motor de recepção do Echo Protocol. Usa 4 filtros Goertzel paralelos para demodular 2 bits por símbolo a partir do áudio capturado pelo microfone.

## Estrutura de Dados

### `StateGoertzel`
Filtro de banda estreita para detectar uma frequência específica em tempo real.

```c
typedef struct StateGoertzel_s {
    int n;              // Janela de integração (SAMPLES_PER_SYMBOL = 32)
    float k, omega, coeff, q1, q2;
} StateGoertzel;
```

### `RxState`
Gerencia o fluxo de bits desde a detecção até a montagem do pacote:
- `state`: `SEARCHING` (buscando sync word) ou `DATA` (recebendo payload).
- `sync_accumulator`: Janela deslizante de 32 bits para detecção da sync word.
- `packet_ready`: Flag atômica notificando o main loop que um pacote completo chegou.
- `packet_len`: Tamanho do payload (extraído do header de 16 bits).
- `is_compressed` / `is_rohc`: Flags de compressão do pacote atual.
- `last_rx_time`: Timestamp do último bit (usado para timeout de 6s).

## Funções Principais

### `audio_to_rb(EchoProtocol *echo, float *sample)`
Processa cada amostra do microfone pelos 4 filtros Goertzel (frequências 2000, 3000, 4000, 5000 Hz). A cada 32 amostras (`SAMPLES_PER_SYMBOL`), seleciona a frequência de maior magnitude e extrai 2 bits. Chama `process_rx_bit` para cada bit.

### `process_rx_bit(EchoProtocol *echo, uint8_t bit)`
Máquina de estados de recepção:
1. **Timeout:** Se em estado `DATA` sem atividade por >6s, reseta para `SEARCHING`.
2. **Sync word:** Em `SEARCHING`, desliza o accumulador de 32 bits. Se igual a `0x930B51DE`, transiciona para `DATA`.
3. **Dado:** Em `DATA`, acumula bits. Após 16 bits, parseia o header (flags de compressão + tamanho). Ao completar o payload, seta `packet_ready` atomicamente.

### `rb_to_tun(EchoProtocol *echo, int *packet_len)`
Lê o pacote completo do `rx_rb` e tenta descomprimir:
- Se `is_rohc`: descomprime ROHC (IPv4 ou IPv6).
- Se `is_compressed`: descomprime LZ4.
- Se raw: copia direto e sincroniza contexto ROHC.

Cada falha (ROHC fail, LZ4 fail, checksum IP inválido) retorna imediatamente com log `[RX] CORRUPT`. Sem flag de erro carregada entre etapas.

## Frequências e Temporização

| Símbolo | Frequência | Bits  |
|---------|------------|-------|
| 00      | 2000 Hz    | 0, 0  |
| 01      | 3000 Hz    | 0, 1  |
| 10      | 4000 Hz    | 1, 0  |
| 11      | 5000 Hz    | 1, 1  |

- `SAMPLE_RATE`: 48000 Hz
- `SYMBOL_RATE`: 1500 baud
- `SAMPLES_PER_SYMBOL`: 32
- Throughput máximo: ~3000 bps (375 bytes/s)
