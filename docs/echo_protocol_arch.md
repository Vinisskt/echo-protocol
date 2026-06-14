# Arquitetura do Echo Protocol (Orquestrador)

O Echo Protocol é um sistema de rede acústico de alta performance que transforma a placa de som em uma interface de rede IP transparente. Ele utiliza modulação AFSK, compressão LZ4 em tempo real e sincronização atômica para permitir comunicações interativas como SSH e Neovim através de ondas sonoras.

## Estrutura de Dados `EchoProtocol`
Centraliza o estado de todos os módulos e garante a segurança entre threads (Áudio vs. Main Loop):
- `tun_fd`: Descritor do túnel Linux.
- `tx_rb` / `rx_rb`: Ring Buffers de bits (8KB cada) para desacoplamento de I/O.
- `mod_state`: Estado do modulador AFSK (frequências de 2400/4800Hz).
- `rx`: Estado do receptor, incluindo flag atômica `packet_ready` e temporizador de timeout (6s).
- `tx`: Estado de transmissão para controle de amostras por bit.

## Pipeline de Processamento (High-Performance)

### 1. `tun_to_rb` (Networking -> Compression -> Link)
- **Lógica:** Lê pacotes IP do TUN (MTU 1000). Tenta compressão via **LZ4**. 
- **Encapsulamento:** Adiciona um cabeçalho de 16 bits (1 bit flag de compressão + 15 bits de tamanho do payload) antes de converter em bits para o `tx_rb`.

### 2. `rb_to_audio` (Data Link -> Physical Layer)
- **Status:** Tempo real (Callback).
- **Lógica:** Consome bits do `tx_rb` a 1800 bps. Se o buffer estiver vazio, transmite um tom de "Idle" (Mark - 4800Hz) para manter a sincronia da fase.

### 3. `audio_to_rb` (Physical Layer -> Data Link)
- **Lógica:** Processa amostras do microfone via Goertzel. Ao detectar a `SYNC_WORD`, inicia a captura do cabeçalho de 16 bits seguido pelo payload.
- **Resiliência:** Implementa **RX Timeout** de 6 segundos. Se um pacote for corrompido, o receptor reseta automaticamente para o estado de busca.

### 4. `rb_to_tun` (Link -> Decompression -> Networking)
- **Lógica:** Reconstrói o pacote do `rx_rb`. Se o flag de compressão estiver ativo, realiza a descompressão LZ4 antes de injetar o pacote IP no kernel.

## Filosofia de Design "Turbo"
- **Latência Mínima:** Loop principal baseado em `poll` com timeout de 5ms.
- **Eficiência de Payload:** MTU de 1000 bytes para minimizar o overhead de preâmbulos em grandes transferências (como desenhos de tela do Neovim).
- **Sincronia de Fase:** Modulação CPFSK (Continuous Phase) que garante zero ruído de transição, permitindo bitrates mais altos (até 2400bps).
- **Isolamento de Erro:** Uso de UDP via `socat` para shells interativos, garantindo que a sessão não caia por erros de bit isolados.
