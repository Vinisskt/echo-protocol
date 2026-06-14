# API de Demodulação AFSK (Goertzel)

Documentação técnica do motor de recepção do Echo Protocol, com foco em resiliência e sincronização de frames.

## Estrutura de Dados

### `StateGoertzel`
Representa um filtro de banda estreita otimizado para detectar tons específicos em tempo real.

```c
typedef struct StateGoertzel_s {
    int n;          // Janela de integração (SAMPLES_PER_BIT)
    float k, omega, coeff, q1, q2; 
} StateGoertzel;
```

### `RxState` (Estado de Recepção)
Gerencia o fluxo de bits desde a detecção até a montagem do pacote IP:
- `state`: SEARCHING ou DATA.
- `packet_ready`: Flag atômica para notificar o loop principal.
- `last_rx_time`: Timestamp do último bit recebido (usado para timeout).
- `is_compressed`: Flag indicando se o payload atual usa LZ4.

## Funções Principais

### `audio_to_rb(EchoProtocol *echo, float *sample)`
- **Descrição:** Processa cada amostra do microfone. Realiza a decisão de bit (Mark vs Space) ao completar `SAMPLES_PER_BIT`.
- **Sincronização:** Após a decisão, chama `process_rx_bit` para gerenciar a máquina de estados de recepção.

### `process_rx_bit(EchoProtocol *echo, uint8_t bit)`
- **Sincronismo de Frame:** Monitora a janela deslizante em busca da `SYNC_WORD`.
- **Processamento de Cabeçalho:** Ao detectar o início do frame, extrai os primeiros 16 bits:
    - Bit 15: Flag de Compressão.
    - Bits 14-0: Tamanho do Payload (MTU até 32KB teóricos, limitado pelo software a 1000).
- **Timeout Automático (6s):** Se o modem entrar no estado `DATA` mas não completar o pacote em 6 segundos, ele volta para `SEARCHING`. Isso evita o travamento do terminal em caso de ruído.

## Performance e Sincronia

1.  **Frequências:** Opera em 2400Hz (Space) e 4800Hz (Mark).
2.  **Sincronismo Atômico:** O uso de `stdatomic.h` garante que o loop principal (`main.c`) saiba exatamente quando injetar o pacote no TUN sem conflitos de memória com a thread de áudio.
3.  **Resiliência:** O algoritmo de Goertzel é reiniciado a cada bit para evitar o acúmulo de erros de fase entre bipes.

## Guia de Recuperação de Erros
Se o terminal (`socat` ou `ssh`) parar de responder:
- Aguarde o **RX Timeout** de 6 segundos.
- O sistema mostrará `[RX] Timeout de recepção! Resetando...` no log.
- Aperte **ENTER** para forçar o envio de um novo pacote de sincronismo.
