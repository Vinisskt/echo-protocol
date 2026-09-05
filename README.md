# Echo Protocol 🔊

Protocolo de rede acústico para Linux que tunela tráfego IP através de frequências de áudio usando modulação **4-FSK** com compressão **ROHC** (IPv4 e IPv6), **LZ4** e checksum **CRC16** end-to-end por frame.

## Arquitetura

- **Modulação 4-FSK:** 4 frequências (2000/3000/4000/5000 Hz), 2 bits por símbolo, fase contínua (CPFSK).
- **Demodulação Goertzel:** 4 filtros paralelos para detecção de tom em tempo real.
- **Buffer circular lock-free SPSC:** 8KB com operações atômicas, thread-safe sem mutex.
- **Interface TUN:** Integração direta com o kernel Linux via ioctl.
- **Compressão ROHC:** Compressão de header IPv4 e IPv6 com contextos TX/RX separados.
- **CRC16 end-to-end:** Todo frame carrega checksum CCITT-FALSE (header + payload) validado no RX antes da decompressão (categoria de corrupção `crc_fail`).
- **Compressão LZ4:** Fallback para payloads não comprimíveis por ROHC.

## Status Técnico

- **Protocolo principal:** Funcional.
- **Taxa de símbolos:** 1500 baud (3000 bps) a 48 kHz.
- **Pipeline TX/RX:** Full-duplex, validado com SSH sobre áudio.
- **Timeout de RX:** 6 segundos com reset automático para busca de sync word.
- **Backpressure:** TX bloqueia leitura do TUN quando buffer >50% para evitar estouro.

## Estrutura do Projeto

```
include/     — Headers públicos
src/         — Implementação (main, protocolo, mod/demod, áudio, TUN, ROHC)
tests/       — Suíte de testes (26 testes ROHC, 20 echo_protocol, etc.)
docs/        — Documentação técnica detalhada
```

## Como Buildar e Testar

```bash
make clean && make

# Listar dispositivos de áudio para descobrir input_id e output_id
sudo ./echo-protocol -l

# Executar (substitua input_id e output_id pelos valores listados)
sudo ./echo-protocol echo0 10.99.0.1 <input_id> <output_id>

# Testes
make -C tests run_tests
```

Testes em hardware real foram feitos com **2 cabos P2** (3.5mm) conectando a saída analógica de uma placa à entrada analógica da outra, garantindo isolamento acústico e enlace estável para validação do protocolo.

## Atribuição

Este projeto foi desenvolvido com assistência de IA (agente OpenCode) para:
- **Código fonte:** Refatoração de pipelines, implementação de compressão ROHC IPv6, buffer circular atômico, padrão de guard clauses e backpressure.
- **Testes:** Geração e manutenção da suíte de testes.
- **Documentação:** Elaboração e atualização da documentação técnica.

## Licença

GNU Affero General Public License v3.0 (AGPL-3.0). Consulte o arquivo LICENSE.
