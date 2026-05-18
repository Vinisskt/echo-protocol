# API de Demodulação AFSK (Goertzel)

Documentação técnica das funções de detecção de sinal e recuperação de dados via algoritmo de Goertzel para o Echo Protocol.

## Estrutura de Dados

### `StateGoertzel`
Armazena os coeficientes pré-calculados e as variáveis de estado para a detecção de uma frequência específica.

```c
typedef struct {
    int n;          // Tamanho do bloco (SAMPLES_PER_BIT, padrão 40)
    float k;        // Índice da frequência alvo
    float omega;    // Frequência angular
    float coeff;    // Coeficiente de feedback (2 * cos(omega))
    float q1;       // Estado n-1 (Memória do filtro)
    float q2;       // Estado n-2 (Memória do filtro)
} StateGoertzel;
```

## Funções Principais

### `pre_calc_goertzel(StateGoertzel *state, uint16_t *freq)`
- **Descrição:** Calcula os coeficientes necessários para monitorar uma frequência específica e inicializa os estados como zero.
- **Argumentos:**
    - `state`: Ponteiro para a estrutura de estado.
    - `freq`: Ponteiro para a frequência alvo em Hz (ex: 1200 ou 2400).
- **Detalhes:** Utiliza a taxa de amostragem definida no projeto (48000 Hz) e o tamanho do bloco de 40 amostras.

### `process_goertzel(StateGoertzel *state, float *sample)`
- **Descrição:** Processa uma única amostra de áudio, atualizando a energia acumulada no filtro.
- **Retorno:** A magnitude quadrática (potência) acumulada até o momento.
- **Lógica:** Implementa o loop de feedback de Goertzel. Para obter a decisão final de um bit, deve-se ler o retorno desta função na 40ª amostra de cada bit.

### `check_sync_word(uint32_t *check_word, uint8_t *bit)`
- **Descrição:** Implementa uma janela deslizante (sliding window) de 32 bits para detectar a `SYNC_WORD` (`0x930B51DE`).
- **Argumentos:**
    - `check_word`: Ponteiro para um acumulador de 32 bits que persiste entre as chamadas.
    - `bit`: Ponteiro para o bit recém-demodulado.
- **Retorno:**
    - `0`: Sync Word detectada (alinhamento de frame encontrado).
    - `1`: Sync Word ainda não detectada.

## Performance e Resiliência

1.  **Eficiência de CPU:** O processamento de amostras utiliza apenas uma multiplicação e duas adições por canal, tornando-o ideal para sistemas de tempo real.
2.  **Imunidade a Ruído:** Validado via `test_demod_noise_stress.c` com **0% de erro (BER)** sob condições de até **125% de ruído branco** em relação à amplitude do sinal.
3.  **Seletividade:** A janela de 40 amostras ($N=40$) proporciona uma separação clara entre as frequências de Mark (2400Hz) e Space (1200Hz), garantindo decodificação robusta mesmo em canais degradados.

## Guia de Implementação (Orquestrador)
Para demodular um bit:
1.  Zerar `q1` e `q2` das structs `StateGoertzel`.
2.  Chamar `process_goertzel` 40 vezes com as amostras de áudio.
3.  Comparar a magnitude retornada pelo canal Space (1200Hz) vs Mark (2400Hz).
4.  O maior valor define o bit (Mark = 1, Space = 0).
