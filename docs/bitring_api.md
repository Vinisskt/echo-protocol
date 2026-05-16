# API do Bit Ring Buffer

Documentação técnica das funções de manipulação de buffer circular em nível de bit para o Echo Protocol.

## Estrutura de Dados

### `Buffer`
```c
typedef struct {
    uint8_t buf[BUFFER_SIZE]; // Array de dados (1024 bytes)
    uint16_t head;            // Índice de escrita (byte atual)
    uint16_t tail;            // Índice de leitura (byte atual)
    uint8_t count_put;        // Contador de bits inseridos no byte head (0-7)
    uint8_t count_get;        // Contador de bits consumidos no byte tail (0-7)
} Buffer;
```

## Funções Principais

### `rb_init()`
- **Descrição:** Aloca e inicializa um novo buffer circular.
- **Retorno:** Ponteiro para a struct `Buffer`.
- **Detalhes:** Zera o primeiro byte e os índices de controle.

### `check_rb(Buffer *buf)`
- **Descrição:** Verifica se o buffer foi alocado corretamente.
- **Retorno:** `0` se OK, `1` se houver erro de alocação.

### `put_bits(Buffer *buf, uint8_t *bit)`
- **Descrição:** Insere um bit individual no buffer, agrupando-os em bytes.
- **Argumentos:** 
    - `buf`: Ponteiro para o buffer.
    - `bit`: Ponteiro para o bit (0 ou 1) a ser inserido.
- **Retorno:**
    - `1`: Sucesso na inserção.
    - `0`: Buffer cheio.
- **Lógica de Agrupamento:** O bit é inserido no byte apontado por `head` usando deslocamento para a esquerda (`<<`). Quando 8 bits são acumulados, `head` avança para a próxima posição.

### `get_bits(Buffer *buf, uint8_t *bit)`
- **Descrição:** Extrai um bit individual do buffer.
- **Argumentos:**
    - `buf`: Ponteiro para o buffer.
    - `bit`: Ponteiro para armazenar o bit extraído (0 ou 1).
- **Retorno:**
    - `1`: Sucesso na extração.
    - `0`: Buffer vazio.
- **Lógica de Extração:** O bit é extraído do byte apontado por `tail`. Quando 8 bits são consumidos, `tail` avança para a próxima posição.

## Verificação e Testes (Resultados)

Foram realizados testes de estresse para garantir a integridade dos dados em nível de bit:

1.  **Inserção Bit-a-Bit:** Confirmado que os bits são agrupados corretamente (ex: `10101010` vira `0xAA`).
2.  **Fronteira de Byte:** Validado que nenhum bit é perdido na transição entre um byte e outro (o 9º bit é salvo corretamente no início do novo byte).
3.  **Limpeza de Memória:** Confirmado que cada novo byte é zerado antes da escrita, evitando corrupção por lixo de memória do `malloc`.
4.  **Buffer Cheio:** O sistema detecta corretamente o estado de buffer cheio usando a máscara `BUFFER_MASK (1023)`.

### Status Atual
- **Estabilidade:** Alta
- **Precisão de Bit:** 100% (Verificado via `test_rb_stress.c`)
- **Eficiência:** Uso de `uint16_t` para índices, otimizando memória.
