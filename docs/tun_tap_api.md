# API de Interface TUN/TAP

Documentação técnica da interface com o kernel Linux para criação de dispositivos de rede virtuais no Echo Protocol.

## Funções

### `tun_alloc(char *dev, int flags)`
- **Descrição:** Abre o dispositivo especial `/dev/net/tun` e registra uma nova interface virtual.
- **Argumentos:**
    - `dev`: Ponteiro para string com o nome desejado (ex: "echo0"). O kernel pode alterar este nome se houver colisão.
    - `flags`: Configurações da interface (`IFF_TUN` para pacotes IP, `IFF_NO_PI` para omitir cabeçalhos extras).
- **Retorno:** Descritor de arquivo (fd) para operações de I/O ou `-1` em caso de erro.

### `tun_read(int fd, uint8_t *buf, uint16_t len)`
- **Descrição:** Wrapper para leitura de pacotes brutos do kernel.
- **Retorno:** Quantidade de bytes lidos do pacote IP.

### `tun_write(int fd, uint8_t *buf, uint16_t len)`
- **Descrição:** Wrapper para escrita de pacotes brutos de volta para o stack de rede do Linux.

## Uso e Permissões
A manipulação de interfaces de rede no Linux exige privilégios de superusuário ou a capacidade `CAP_NET_ADMIN`.
Comandos úteis para configuração após a criação:
```bash
sudo ip link set dev echo0 up
sudo ip addr add 10.0.0.1/24 dev echo0
```
