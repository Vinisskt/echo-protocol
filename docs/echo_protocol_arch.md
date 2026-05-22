# Arquitetura do Echo Protocol (Orquestrador)

O Orquestrador é a camada de abstração que une o processamento de sinal (AFSK/Goertzel) à interface de rede (TUN/TAP). Ele opera seguindo a filosofia de **Bitstream Contínuo** e **Estágios Desacoplados**.

## Estrutura de Dados `EchoProtocol`
Centraliza o estado de todos os módulos:
- `tun_fd`: Link com o kernel.
- `tx_rb` / `rx_rb`: Filas de bits para transmissão e recepção.
- `mod_state` / `space_state` / `mark_state`: Estados internos do DSP.
- `rx_sample_count`: Contador de amostragem gerenciado externamente.
- `current_bit`: Bit atual de modulação.
- `sync_accumulator`: Acumulador para sincronismo.

## Pipeline de Processamento (Pipeline Stages)

O protocolo é dividido em estágios independentes para evitar acoplamento e lógica aninhada:

### 1. `tun_to_rb` (Networking -> Data Link)
- **Status:** Validado.
- **Lógica:** Converte bytes de pacotes IP do TUN em fluxo contínuo de bits para o `tx_rb`.

### 2. `rb_to_audio` (Data Link -> Physical Layer)
- **Status:** 100% Funcional e Validado.
- **Assinatura:** `void rb_to_audio(EchoProtocol *echo, uint8_t *bit);`
- **Lógica:** Gera amostras individuais de áudio AFSK, mantendo o estado de fase.

### 3. `audio_to_rb` (Physical Layer -> Data Link)
- **Status:** 100% Funcional e Validado.
- **Assinatura:** `void audio_to_rb(EchoProtocol *echo, float *sample);`
- **Lógica:** Processa amostras via Goertzel, com detecção de bits gerenciada externamente via `rx_sample_count`.

### 4. `rb_to_tun` (Data Link -> Networking)
- **Status:** 100% Funcional e Validado.
- **Lógica:** Reconstrói pacotes IP a partir do fluxo de bits no `rx_rb`.
- **Assinatura:** `void rb_to_tun(EchoProtocol *echo, int *packet_len);`
- **Diferencial:** Implementa reconstrução linear de bits (`Shift-in`) diretamente no buffer de destino. O tamanho do pacote (`packet_len`) é gerenciado por referência.
- **Validação:** Aprovada em testes de estresse com 1000 pacotes de tamanhos variáveis sem corrupção de dados.

## Filosofia de Design
- **Zero Acoplamento:** As funções de áudio não conhecem protocolos de rede, e as funções de rede não conhecem frequências.
- **Eficiência de Bitwise:** Uso intensivo de Bit Shift (`>>`, `<<`) para garantir performance em tempo real.
- **Resiliência:** Preparado para lidar com tráfego IPv4 e IPv6 de forma transparente.
