# API de Interface TUN/TAP

Documentação técnica das funções de baixo nível para criação e configuração automática da interface de rede virtual do Echo Protocol.

## Funções de Gerenciamento

### `tun_alloc(char *dev, int flags)`
- **Descrição:** Registra uma nova interface TUN no kernel.
- **Diferencial:** Agora suporta nomes de interface dinâmicos e garante a persistência do file descriptor durante a execução do programa.

### `tun_set_ip(const char *dev, const char *ip)`
- **Descrição:** Configura o endereço IPv4 da interface (ex: `10.0.0.1`) e define a máscara de rede padrão (`255.255.255.0`).
- **Lógica:** Utiliza a `ioctl(SIOCSIFADDR)` para comunicação direta com o kernel, sem chamadas ao shell.

### `tun_set_mtu(const char *dev, int mtu)`
- **Descrição:** Define o tamanho máximo do pacote IP (MTU).
- **MTU Padrão:** **1000 bytes**.
- **Impacto:** Ajustar o MTU via `ioctl(SIOCSIFMTU)` é vital para equilibrar a eficiência do bleeep de áudio com a tolerância a ruído do sinal.

### `tun_set_up(const char *dev)`
- **Descrição:** Sobe a interface (`UP` e `RUNNING`). Sem esta chamada, o kernel não enviaria pacotes para o nosso programa.

## I/O de Dados

### `tun_read(int fd, uint8_t *buf, uint16_t len)`
- **Descrição:** Lê um pacote IP bruto vindo do sistema operacional.
- **Ponto Chave:** No `main.c`, o `poll()` detecta quando esta função tem dados prontos.

### `tun_write(int fd, uint8_t *buf, uint16_t len)`
- **Descrição:** Injeta um pacote IP reconstruído (após descompressão LZ4) de volta no stack de rede do Linux.

## Segurança e Privilégios
O Echo Protocol agora realiza toda a configuração via **IOCTL**. Isso significa:
1.  **Segurança:** Não há risco de injeção de comandos via shell (não usamos `system()`).
2.  **Privilégios:** O binário deve ser executado como `root` ou possuir a capacidade `CAP_NET_ADMIN`.
    ```bash
    sudo ./echo-protocol tun0 10.0.0.1 6 6
    ```
