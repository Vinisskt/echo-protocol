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
- **Lógica de Agrupamento (LSB):** O bit é inserido no byte apontado por `head` usando deslocamento para a esquerda (`<<`) baseado no contador `count_put`. Quando 8 bits são acumulados, `head` avança.

### `get_bits(Buffer *buf, uint8_t *bit)`
- **Descrição:** Extrai um bit individual do buffer.
- **Argumentos:**
    - `buf`: Ponteiro para o buffer.
    - `bit`: Ponteiro para armazenar o bit extraído (0 ou 1).
- **Retorno:**
    - `1`: Sucesso na extração.
    - `0`: Buffer vazio.
- **Lógica de Extração (LSB):** O bit é extraído do byte apontado por `tail` usando deslocamento para a direita (`>>`) baseado no contador `count_get`. Quando 8 bits são consumidos, `tail` avança.

## Verificação e Testes (Resultados)

Foram realizados testes de estresse para garantir a integridade dos dados em nível de bit sob a convenção LSB:

1.  **Inserção Bit-a-Bit (LSB):** Confirmado que os bits são agrupados corretamente começando pelo bit menos significativo.
2.  **Fronteira de Byte:** Validado que a transição entre bytes mantém a ordem correta dos bits.
3.  **Limpeza de Memória:** Confirmado que cada novo byte é zerado antes da escrita.
4.  **Buffer Cheio:** O sistema detecta corretamente o estado de buffer cheio usando a máscara `BUFFER_MASK`.
5.  **Fluxo Contínuo (Stress):** Validado via `test_rb_flow_stress.c` e `test_rb_to_tun_stress.c`, processando fluxos contínuos de pacotes sem perda de integridade.

### Status Atual
- **Estabilidade:** Alta
- **Precisão de Bit:** 100%
- **Eficiência:** Uso de `uint16_t` para índices e máscaras bitwise para controle de rotação.
