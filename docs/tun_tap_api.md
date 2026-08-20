# API de Interface TUN

Funções de baixo nível para criação e configuração da interface de rede virtual.

## Funções de Gerenciamento

### `tun_alloc(char *dev, int flags)`
Registra uma nova interface TUN no kernel via `open(/dev/net/tun)` + `ioctl(TUNSETIFF)`. Retorna o file descriptor ou -1 em erro.

### `tun_set_ip(const char *dev, const char *ip)`
Configura endereço IPv4 (via `ioctl(SIOCSIFADDR)`) e máscara `255.255.255.0` (via `ioctl(SIOCSIFNETMASK)`).

### `tun_set_mtu(const char *dev, int mtu)`
Define o MTU da interface (padrão: 1000). MTU menor reduz latência por pacote.

### `tun_set_up(const char *dev)`
Sobe a interface com flags `IFF_UP | IFF_RUNNING`.

## I/O de Dados

### `tun_read(int fd, uint8_t *buf, uint16_t len)`
Lê um pacote IP bruto do kernel. O main loop usa `poll()` para detectar dados disponíveis.

### `tun_write(int fd, uint8_t *buf, uint16_t len)`
Injeta um pacote IP reconstruído (após decodificação FEC, descompressão ROHC ou LZ4) de volta no stack de rede.

## Pipeline de Compressão (TX)

Os pacotes lidos do TUN passam por compressão + FEC antes da transmissão:
1. **ROHC** — compressão de header IPv4 ou IPv6 (contexto separado TX/RX).
2. **LZ4** — compressão genérica de payload (fallback quando ROHC não aplicável).
3. **Raw** — sem compressão (enviado como está, com sincronização de contexto ROHC no receptor).
4. **FEC (Reed-Solomon + Interleaver)** — **sempre aplicado** após o passo 1/2/3, adiciona paridade para correção de erros no canal acústico.

## Pipeline de Descompressão (RX)
1. **FEC decode** — Reed-Solomon + deinterleaver (corrige erros do canal).
2. **ROHC decompress** — reconstrói header IP original.
3. **LZ4 decompress** — reconstrói payload original.
4. **Raw** — copia direto (sincroniza contexto ROHC no RX).

## Backpressure

Quando o buffer TX atinge >50% de ocupação, o main loop interrompe a leitura do TUN. O kernel bufferiza os pacotes excedentes na fila de saída da interface, aplicando pressão natural de volta para a pilha TCP/IP.

## Privilégios

O binário deve ser executado como root ou com `CAP_NET_ADMIN`:
```bash
sudo ./echo-protocol echo0 10.99.0.1 4 2
```
