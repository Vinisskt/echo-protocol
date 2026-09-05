# Arquitetura do Echo Protocol

Sistema de rede acústico que transforma a placa de som em uma interface de rede IP transparente. Utiliza modulação 4-FSK, compressão ROHC+LZ4, CRC16 end-to-end e buffer circular thread-safe para comunicação via ondas sonoras.

## Estrutura de Dados `EchoProtocol`

Centraliza o estado de todos os módulos com sincronização atômica entre a thread de áudio e o main loop:

- `tun_fd`: Descritor do túnel TUN.
- `tx_rb` / `rx_rb`: Ring buffers de bits (16KB cada, lock-free SPSC com atomics).
- `mod_state`: Estado do modulador 4-FSK (4 tons: 2000/3000/4000/5000 Hz).
- `freq_states[4]`: 4 filtros Goertzel para demodulação.
- `rx`: Estado do receptor com flag atômica `packet_ready`, timeout de 6s.
- `tx`: Controle de amostras por símbolo.
- `rohc_tx` / `rohc_rx`: Contextos ROHC separados para compressão IPv4 e IPv6.

## Pipeline de Processamento

### 1. `tun_to_rb` (TX: TUN → Compressão → CRC → Buffer)
Lê pacote IP do TUN. Tenta compressão na ordem:
1. **ROHC** — compressão de header (IPv4 ou IPv6)
2. **LZ4** — compressão genérica do payload (se ROHC não aplicável)
3. **Raw** — sem compressão (contexto ROHC é sincronizado no RX)

Frame no ar: `[header 16 bits][payload N*8 bits][CRC16 16 bits]`. O CRC16
(CCITT-FALSE, poli 0x1021, init 0xFFFF) cobre **header + payload** — é a única
proteção de integridade do payload, pois ROHC/LZ4 não têm checksum e o IP
recomputado no RX mascara corrupções (bug de entrega silenciosa eliminado).
FEC (Reed-Solomon) está desativado; sem paridade no ar.

Header de 16 bits: bit 15 = LZ4, bit 14 = ROHC, bit 13 = reservado, bits 12-0 = tamanho (max 8191 bytes).

### 2. `rb_to_audio` (Callback de Áudio — thread separada)
Consome símbolos (2 bits) do `tx_rb` a 1500 baud (3000 bps). Se vazio, envia tom idle (símbolo 3 = 5000 Hz). O callback roda em tempo real pelo PortAudio.

### 3. `audio_to_rb` (Callback → Demodulação → Buffer)
Processa amostras do microfone via 4 filtros Goertzel. A cada `SAMPLES_PER_SYMBOL` amostras (32 em 48kHz), decide o símbolo de maior energia e extrai 2 bits. Detecta sync word (`0x930B51DE`) para inicio de frame.

### 4. `rb_to_tun` (RX: Buffer → CRC16 → Decompressão → TUN)
Reconstrói pacote do `rx_rb`. Primeiro valida o **CRC16 end-to-end** (header +
payload): qualquer flip em header/payload/crc é descartado como `crc_fail`
antes de qualquer decompressão — zero caminho de entrega sem verificação.
Depois descomprime ROHC ou LZ4. Cada erro (crc_fail, ROHC fail, LZ4 fail,
checksum IP inválido) causa return imediato com log. Sem flag `corrupted`
carregada entre etapas.

## Filosofia de Design

- **Thread-safe:** Buffer circular lock-free SPSC com `atomic_load`/`atomic_store`. Sem mutex no callback de áudio.
- **Guard Clauses:** Toda condição de erro é negada com return imediato. Zero `else` em todo o pipeline de RX.
- **Backpressure TX:** Só lê do TUN quando o `tx_rb` está abaixo de 50% da capacidade. O kernel segura os pacotes excedentes.
- **Latência:** Loop baseado em `poll` com timeout de 5ms.
- **ROHC bidirecional:** Contextos TX/RX separados para IPv4 e IPv6.
