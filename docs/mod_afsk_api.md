# API de Modulação AFSK (Audio Frequency Shift Keying)

Documentação técnica das funções de geração de sinal de áudio para o Echo Protocol, otimizada para alta velocidade e estabilidade.

## Estrutura de Dados

### `StateAFSK`
Armazena o estado do modulador, garantindo a continuidade da fase entre amostras e bits (**CPFSK**).

```c
typedef struct StateAFSK_s {
    float step_cos_space; // Pré-cálculo para Space (2400Hz)
    float step_cos_mark;  // Pré-cálculo para Mark (4800Hz)
    float step_sin_space; 
    float step_sin_mark;  
    float current_sin;    // Estado da fase seno
    float current_cos;    // Estado da fase cosseno
    uint16_t sample_count; // Controle de tempo do bit
} StateAFSK;
```

## Funções Principais

### `pre_calc_afsk(StateAFSK *state)`
- **Descrição:** Calcula os coeficientes trigonométricos e inicializa o estado de fase.
- **Parâmetros de Operação:**
    - `FREQ_SPACE`: **2400 Hz**
    - `FREQ_MARK`: **4800 Hz**
    - `BIT_RATE`: **1800 bps** (estável até 2400 bps)
    - `SAMPLE_RATE`: 48000 Hz

### `generate_afsk(StateAFSK *state, uint8_t *bit)`
- **Descrição:** Gera uma amostra de áudio individual baseada no bit.
- **Amplitude:** O sinal é multiplicado por **0.5f** (50% de ganho) para evitar distorção (clipping) nas placas de som e facilitar a detecção pelo receptor.
- **Fase Contínua:** Utiliza rotação complexa para garantir que não haja saltos de fase na troca de bits, eliminando ruídos de alta frequência.

### `push_preamble(Buffer *buf)`
- **Descrição:** Insere um preâmbulo de **24 bits** (`1010...`).
- **Finalidade:** Fornece uma janela de sincronização suficiente para que o receptor estabilize o clock de bits antes de pacotes longos (até MTU 1000).

### `push_sync_word(Buffer *buf)`
- **Descrição:** Insere a palavra de sincronismo de frame de 32 bits (`0x930B51DE`).

## Performance e Estabilidade

1.  **Bitrate Calibrado:** A taxa de 1800 bps permite que cada bit tenha ciclos de onda suficientes para uma detecção robusta via Goertzel, mesmo em cabos virtuais ou ambientes ruidosos.
2.  **Separação de Frequência:** O uso de 2.4kHz e 4.8kHz (oitavas) minimiza a interferência harmônica entre os tons de 0 e 1.
3.  **Eficiência Matemática:** A geração de senoides via rotação complexa (`next_sin = current_sin*cos_step + current_cos*sin_step`) economiza CPU ao evitar chamadas repetitivas a `sinf()`.

## Verificação

O modulador foi testado para:
- **Zero Clipping:** Saída estabilizada em ±0.5.
- **Continuidade:** Verificação visual do espectro de sinal sem transientes abruptos.
