# API do Bit Ring Buffer

Fila circular por bit com sincronização atômica lock-free (Single Producer, Single Consumer), coração do desacoplamento entre a thread de áudio e o main loop.

## Estrutura de Dados

### `Buffer`
```c
#define BUFFER_SIZE 16384

typedef struct {
    uint8_t buf[BUFFER_SIZE];
    atomic_uint_fast16_t head;      // Escrita (byte index)
    atomic_uint_fast16_t tail;      // Leitura (byte index)
    atomic_uint_fast8_t count_put;  // Offset de bit no byte atual (escrita)
    atomic_uint_fast8_t count_get;  // Offset de bit no byte atual (leitura)
} Buffer;
```

Todos os campos de controle (`head`, `tail`, `count_put`, `count_get`) são atômicos, garantindo segurança entre threads sem mutex.

## Funções

### `put_bits(Buffer *buf, uint8_t *bit)`
Produtor (main loop para `tx_rb`, callback de áudio para `rx_rb`). Insere 1 bit no buffer em ordem LSB-first. Se o buffer estiver cheio (`(head+1) & MASK == tail`), retorna 0 e loga `[BUF FULL]`.

### `get_bits(Buffer *buf, uint8_t *bit)`
Consumidor (callback de áudio para `tx_rb`, main loop para `rx_rb`). Lê 1 bit do buffer. Se vazio (`head == tail && count_get == count_put`), retorna 0 e o modulador envia tom idle.

### `rb_reset(Buffer *buf)`
Reseta o buffer para estado vazio (head=tail, count_put=count_get=0).

## Modelo de Concorrência (SPSC Lock-Free)

| Buffer   | Produtor          | Consumidor        |
|----------|-------------------|--------------------|
| `tx_rb`  | Main loop         | Callback de áudio  |
| `rx_rb`  | Callback de áudio | Main loop          |

Cada buffer tem exatamente 1 produtor e 1 consumidor. Não há escrita concorrente no mesmo campo — `head` só é escrito pelo produtor, `tail` só pelo consumidor. As operações usam `atomic_load`/`atomic_store` com ordenação `seq_cst` por padrão.

## Capacidade

16KB = 16384 bytes = 131072 bits. Com MTU de 2048 bytes (16384 bits) + 72 bits de overhead (preamble+sync+header), o buffer comporta cerca de 3-4 pacotes completos. Quando o uso ultrapassa 50%, o main loop para de ler do TUN (backpressure), deixando o kernel bufferizar os pacotes.
