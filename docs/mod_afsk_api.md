# API de Modulação AFSK (Audio Frequency Shift Keying)

Documentação técnica das funções de geração de sinal de áudio modulado por deslocamento de frequência para o Echo Protocol.

## Estrutura de Dados

### `StateAFSK`
Esta estrutura armazena o estado interno do modulador, garantindo a continuidade da fase entre amostras e bits.

```c
typedef struct {
    float step_cos_space; // Pré-cálculo da variação de cosseno para Space (1200Hz)
    float step_cos_mark;  // Pré-cálculo da variação de cosseno para Mark (2400Hz)
    float step_sin_space; // Pré-cálculo da variação de seno para Space (1200Hz)
    float step_sin_mark;  // Pré-cálculo da variação de seno para Mark (2400Hz)
    float current_sin;    // Estado atual da fase (componente Seno)
    float current_cos;    // Estado atual da fase (componente Cosseno)
    uint16_t sample_count; // Contador de amostras geradas para o bit atual (0-39)
} StateAFSK;
```

## Funções Principais

### `pre_calc_afsk(StateAFSK *state)`
- **Descrição:** Pré-calcula os coeficientes trigonométricos baseados nas frequências definidas e inicializa o estado de fase.
- **Argumentos:**
    - `state`: Ponteiro para a estrutura de estado.
- **Constantes Utilizadas:**
    - `FREQ_SPACE`: 1200 Hz
    - `FREQ_MARK`: 2400 Hz
    - `SAMPLE_RATE`: 48000 Hz
- **Efeito:** Inicializa `current_cos` em 1.0 e `current_sin` em 0.0 para garantir que o sinal comece no início da fase.

### `generate_afsk(StateAFSK *state, uint8_t *bit)`
- **Descrição:** Gera uma única amostra de áudio (seno/cosseno) baseada no bit fornecido.
- **Argumentos:**
    - `state`: Ponteiro para a estrutura de estado.
    - `bit`: Ponteiro para o bit (0 para Space, 1 para Mark).
- **Lógica de Modulação:**
    - Utiliza o método de rotação complexa para atualizar `current_cos` e `current_sin` sem a necessidade de chamar funções pesadas como `sin()` ou `cos()` a cada amostra.
    - Garante a **Continuidade de Fase (CPFSK)**, evitando cliques ou ruídos na transição entre bits.
- **Orquestração:** Esta função é "pura" em relação ao processamento de sinal; ela não consome bits do buffer circular. O chamador (orquestrador) é responsável por manter o mesmo ponteiro de bit durante as `SAMPLES_PER_BIT` (40 amostras por padrão).

## Arquitetura e Performance

1.  **Separação de Camadas:** A biblioteca de modulação é isolada da camada de dados (`Buffer`). Isso permite que ela seja usada em sistemas de tempo real, como interrupções de áudio.
2.  **Eficiência Matemática:** A atualização da fase via rotação matricial (`next_cos = cos*step_cos - sin*step_sin`) é significativamente mais rápida que o cálculo direto de funções trigonométricas em loops.
3.  **Amostragem:** Otimizado para 48kHz, proporcionando 40 amostras por bit a uma taxa de 1200bps.

## Verificação e Testes

A biblioteca foi validada através de testes de rigor em `test_mod_afsk.c`:
- **Consistência de Fase:** Verificado que a variação angular entre amostras corresponde exatamente à frequência desejada (0.314 rad para 2400Hz).
- **Stress de Orquestração:** Validado em fluxo contínuo de 100.000 bits sem desvio de frequência ou corrupção de estado.

### Status Atual
- **Estabilidade:** Alta
- **Arquitetura:** DSP puro (Single sample processing)
- **Fase:** Contínua (Zero clicks)
