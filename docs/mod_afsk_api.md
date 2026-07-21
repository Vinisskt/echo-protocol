# API de Modulação 4-FSK

Geração de sinal de áudio usando 4-FSK (4-frequency shift keying), transmitindo 2 bits por símbolo com fase contínua (CPFSK).

## Estrutura de Dados

### `StateFSK`
```c
typedef struct StateFSK_s {
    float step_cos[4];   // Cosseno pré-calculado para cada frequência
    float step_sin[4];   // Seno pré-calculado para cada frequência
    float current_cos;   // Estado da fase (cosseno)
    float current_sin;   // Estado da fase (seno)
    uint16_t sample_count;
} StateFSK;
```

## Funções Principais

### `pre_calc_fsk(StateFSK *state)`
Calcula os coeficientes de rotação complexa para as 4 frequências e inicializa a fase em `cos=1, sin=0`.

Frequências: 2000, 3000, 4000, 5000 Hz a 48000 Hz de sample rate.

### `generate_fsk(StateFSK *state, uint8_t *symbol)`
Gera uma amostra de áudio para o símbolo (0-3). Usa rotação complexa para manter fase contínua entre símbolos. Amplitude multiplicada por 0.5f para evitar clipping.

### `push_preamble(Buffer *buf)`
Insere 24 bits de preâmbulo (8 zeros + 16 alternados `1010...` = `0x00AAAA`) no buffer. Prepara o receptor para a sincronização de clock.

### `push_sync_word(Buffer *buf)`
Insere a sync word de 32 bits (`0x930B51DE`) no buffer. O receptor usa uma janela deslizante para detectar este padrão e iniciar a recepção do frame.

## Parâmetros de Operação

| Parâmetro       | Valor  |
|-----------------|--------|
| `SYMBOL_RATE`   | 1500   |
| `SAMPLE_RATE`   | 48000  |
| `SAMPLES_PER_SYMBOL` | 32     |
| Throughput      | 3000 bps |
| Amplitude       | ±0.5   |

## Vantagens do 4-FSK sobre AFSK

- 2 bits por símbolo vs 1 bit: dobra a taxa efetiva.
- Fase contínua (CPFSK) elimina transientes entre símbolos.
- 4 frequências igualmente espaçadas (1kHz de gap) facilitam a discriminação pelo Goertzel.
