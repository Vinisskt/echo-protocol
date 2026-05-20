# Arquitetura do Echo Protocol (Orquestrador)

O Orquestrador é a camada de abstração que une o processamento de sinal (AFSK/Goertzel) à interface de rede (TUN/TAP). Ele opera seguindo a filosofia de **Bitstream Contínuo** e **Estágios Desacoplados**.

## Estrutura de Dados `EchoProtocol`
Centraliza o estado de todos os módulos:
- `tun_fd`: Link com o kernel.
- `tx_rb` / `rx_rb`: Filas de bits para transmissão e recepção.
- `mod_state` / `space_state` / `mark_state`: Estados internos do DSP.

## Pipeline de Processamento (Pipeline Stages)

O protocolo é dividido em 4 estágios independentes para evitar acoplamento e lógica aninhada:

### 1. `tun_to_rb` (Networking -> Data Link)
- **Lógica:** Implementa um **Shift Register Virtual**.
- **Processamento:** Lê pacotes IP (IPv4 ou IPv6) do TUN e converte os bytes em um fluxo contínuo de bits para o `tx_rb`.
- **Diferencial:** Não espera "fechar um byte" para processar o próximo; trata o pacote como uma linha infinita de bits usando aritmética de ponteiros global.

### 2. `rb_to_audio` (Data Link -> Physical Layer)
- **Status:** Em implementação.
- **Lógica:** Consome bits do `tx_rb` e gera amostras de áudio AFSK de 48kHz.

### 3. `audio_to_rb` (Physical Layer -> Data Link)
- **Status:** Planejado.
- **Lógica:** Alimenta o algoritmo Goertzel com amostras e injeta os bits detectados no `rx_rb`.

### 4. `rb_to_tun` (Data Link -> Networking)
- **Status:** Planejado.
- **Lógica:** Monitora o `rx_rb` em busca da `Sync Word` e reconstrói pacotes IP para entrega ao kernel.

## Filosofia de Design
- **Zero Acoplamento:** As funções de áudio não conhecem protocolos de rede, e as funções de rede não conhecem frequências.
- **Eficiência de Bitwise:** Uso intensivo de Bit Shift (`>>`, `<<`) para garantir performance em tempo real.
- **Resiliência:** Preparado para lidar com tráfego IPv4 e IPv6 de forma transparente.
